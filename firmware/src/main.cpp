// Startup and the tick loop. The only file here that talks to pins.
//
// Everything this file does is glue: read the hardware, hand a snapshot to a
// role, render whatever the role asked for on the LED. It holds no decisions,
// because a decision living in here is a decision that cannot be tested, and
// the rest of this firmware is arranged specifically so that none of them have
// to be.
//
// WHICH ROLE AND WHICH TRANSPORT ARE BUILD TIME CHOICES.
//
// A board is flashed as one thing. A beacon that could be talked into being a
// repeater is a beacon with a way to stop being a beacon, and there is no
// version of this product where that is worth having. Both come from -D flags
// in platformio.ini, so a build is reproducible from its environment name.
//
//   TREKSAFE_ROLE_BEACON     carried by a visitor. A button and a GPS.
//   TREKSAFE_ROLE_REPEATER   a solar box on a ridge. Relays and reports in.
//   TREKSAFE_ROLE_GATEWAY    the radio beside the base station computer.
//
//   TREKSAFE_TRANSPORT_LORA    the real radio
//   TREKSAFE_TRANSPORT_SERIAL  the USB cable, standing in for the air, which is
//                              how this is demonstrated with one board

#include <Arduino.h>

#include "../boards/heltec_v4.h"
#include "config.h"
#include "contract.h"
#include "display.h"
#include "role_beacon.h"
#include "role_gateway.h"
#include "role_repeater.h"
#include "transport.h"

#if defined(TREKSAFE_TRANSPORT_LORA)
#include "transport_lora.h"
#else
#include "transport_serial.h"
#endif

// ---------------------------------------------------------------------------
// Build configuration checks, done by the compiler rather than discovered on a
// ridge. Exactly one role, and a gateway needs a cable by definition.
// ---------------------------------------------------------------------------

#if defined(TREKSAFE_ROLE_BEACON) + defined(TREKSAFE_ROLE_REPEATER) +                               \
        defined(TREKSAFE_ROLE_GATEWAY) !=                                                          \
    1
#error "Define exactly one of TREKSAFE_ROLE_BEACON, TREKSAFE_ROLE_REPEATER, TREKSAFE_ROLE_GATEWAY"
#endif

#if defined(TREKSAFE_ROLE_GATEWAY) && !defined(TREKSAFE_TRANSPORT_LORA)
#error "A gateway is the node with a radio on one side and a cable on the other. Build it with TREKSAFE_TRANSPORT_LORA."
#endif

// How often a role is asked to do its work.
//
// Fast enough that the button feels immediate and the LED pattern is smooth,
// slow enough to leave the processor idle most of the time. Every role's timing
// is expressed in real milliseconds and compared against the clock, so nothing
// depends on this number being exact; changing it changes responsiveness and
// nothing else.
static const uint32_t kTickMs = 50;

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

#if defined(TREKSAFE_TRANSPORT_LORA)
static treksafe::LoraTransport radio;
#else
static treksafe::SerialTransport radio(Serial);
#endif

// The gateway's other side: the cable to the computer running the console.
#if defined(TREKSAFE_ROLE_GATEWAY)
class ArduinoHostPort : public treksafe::HostPort {
  public:
    ArduinoHostPort() : len_(0), overflowed_(false) { buffer_[0] = '\0'; }

    void writeLine(const char *line) override
    {
        Serial.print(line);
        Serial.print('\n');
    }

    bool readLine(char *out, size_t cap) override
    {
        while (Serial.available() > 0) {
            int c = Serial.read();
            if (c < 0)
                break;
            if (c != '\n' && c != '\r') {
                if (len_ + 1 < sizeof(buffer_))
                    buffer_[len_++] = (char)c;
                else
                    overflowed_ = true; // consume to the newline, then discard
                continue;
            }

            buffer_[len_] = '\0';
            size_t len = len_;
            bool bad = overflowed_;
            len_ = 0;
            overflowed_ = false;

            if (bad || len == 0)
                continue;
            snprintf(out, cap, "%s", buffer_);
            return true;
        }
        return false;
    }

  private:
    char buffer_[treksafe::kFrameMax + 48];
    size_t len_;
    bool overflowed_;
};
static ArduinoHostPort host;
#endif

static char deviceId[treksafe::kDevIdMax];
static treksafe::Display display;

// How often the screen may be redrawn, and how often it is redrawn anyway.
//
// A redraw pushes a kilobyte over I2C. Doing that at tick speed would spend a
// large part of every second drawing a picture nobody can read changing that
// fast, on a beacon whose battery has to last until somebody walks in.
//
// So: at most four times a second, and only when the picture would actually
// differ. The exception is a hold in progress, where the bar has to move even
// though the state has not changed, and a slow floor so a screen that somehow
// lost its contents recovers on its own rather than staying wrong until
// something else happens.
static const uint32_t kScreenMs = 250;
static const uint32_t kScreenRefreshMs = 5000;
static uint32_t lastScreenMs = 0;
static uint32_t lastScreenDrawMs = 0;

// A cheap summary of what is currently on the screen, so an unchanged picture
// is not redrawn. Anything that appears on screen has to be in here, or the
// screen will sit stale showing the old value of it.
static uint32_t screenSignature(treksafe::Indication ind, int battPct, bool hasFix,
                                uint32_t sent, uint32_t extra)
{
    uint32_t h = 2166136261u;
    const uint32_t parts[] = {(uint32_t)ind, (uint32_t)(battPct + 1), (uint32_t)hasFix, sent,
                              extra};
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        h ^= parts[i];
        h *= 16777619u;
    }
    return h;
}
static uint32_t lastScreenSignature = 0;

// Roles are constructed in setup(), once the device id is known, so each is
// held as a pointer. Placement is static; nothing here is ever freed.
#if defined(TREKSAFE_ROLE_BEACON)
static treksafe::BeaconRole *role = 0;
#elif defined(TREKSAFE_ROLE_REPEATER)
static treksafe::RepeaterRole *role = 0;
#else
static treksafe::GatewayRole *role = 0;
#endif

static uint32_t lastTickMs = 0;
static bool halted = false;
static char haltReason[160];

// ---------------------------------------------------------------------------
// Reading the hardware
// ---------------------------------------------------------------------------

// The board's factory programmed MAC address, low 32 bits. Stable for the life
// of the board and needs no provisioning step at the trailhead, which is a step
// somebody would eventually get wrong by flashing two beacons the same.
static uint32_t hardwareSerial()
{
    uint64_t mac = ESP.getEfuseMac();
    return (uint32_t)(mac & 0xFFFFFFFFu);
}

#if defined(TREKSAFE_ROLE_REPEATER)
// Seeds the per-board relay delay. Two repeaters that picked the same delays
// would collide on every single relay, so this has to differ per board, and the
// device id is the thing that already does.
//
// Compiled only into the repeater, because it is the only role that relays.
static uint32_t jitterSeed()
{
    uint32_t h = 2166136261u; // FNV-1a, for no reason beyond being short and adequate
    for (const char *p = deviceId; *p; p++) {
        h ^= (uint32_t)(unsigned char)*p;
        h *= 16777619u;
    }
    return h ? h : 1u;
}
#endif

// Battery voltage at the cell, or a negative number if it could not be read.
//
// Two things here are easy to get wrong and both were, first time round.
//
// The divider is not powered all the time, because leaving it connected would
// drain the battery it measures. It has to be switched on, given a moment to
// settle, read, and switched off again. Reading without enabling it returns a
// number, and that number is meaningless.
//
// The conversion uses analogReadMilliVolts rather than scaling a raw count.
// That function uses the calibration burned into the chip at the factory, so it
// accounts for this particular ESP32's reference voltage and the attenuation
// actually in effect. Scaling a raw count means guessing both, and the guess is
// wrong by a few per cent per board.
static double readBatteryVolts()
{
#ifdef TS_BATTERY_ADC_PIN
#ifdef TS_BATTERY_ENABLE_PIN
    pinMode(TS_BATTERY_ENABLE_PIN, OUTPUT);
#if TS_BATTERY_ENABLE_ACTIVE_LOW
    digitalWrite(TS_BATTERY_ENABLE_PIN, LOW);
#else
    digitalWrite(TS_BATTERY_ENABLE_PIN, HIGH);
#endif
    delay(10); // let the divider settle before believing it
#endif

    uint32_t mv = analogReadMilliVolts(TS_BATTERY_ADC_PIN);

#ifdef TS_BATTERY_ENABLE_PIN
    // Back off, so the divider is not left draining the cell.
#if TS_BATTERY_ENABLE_ACTIVE_LOW
    digitalWrite(TS_BATTERY_ENABLE_PIN, HIGH);
#else
    digitalWrite(TS_BATTERY_ENABLE_PIN, LOW);
#endif
#endif

    if (mv == 0)
        return -1.0; // nothing connected, or no divider fitted
    return (mv / 1000.0) * TS_BATTERY_MULTIPLIER;
#else
    return -1.0;
#endif
}

// How often the battery is actually measured. See the note at the call site.
//
// Thirty seconds is far faster than a battery changes and far slower than the
// tick. The first reading is taken on demand so a frame sent in the first
// half minute still carries one.
static const uint32_t kBatteryReadMs = 30000;

static double cachedBatteryVolts(uint32_t nowMs)
{
    static double volts = -1.0;
    static uint32_t lastReadMs = 0;
    static bool everRead = false;

    if (!everRead || treksafe::elapsedAtLeast(nowMs, lastReadMs, kBatteryReadMs)) {
        volts = readBatteryVolts();
        lastReadMs = nowMs;
        everRead = true;
    }
    return volts;
}

// Roughly how much is left, as a percentage.
//
// A straight line between an empty and a full lithium cell. It is not a fuel
// gauge and does not pretend to be: the voltage of a lithium cell under load
// says only approximately how much is in it, and the multiplier this depends on
// has not been calibrated against this board. The console shows this as a
// number a warden uses to judge how long somebody can wait, so it is reported
// as approximate and the firmware says so in its log at startup. Being roughly
// right and honest about it beats being precisely wrong.
static int batteryPercent(double volts)
{
    const double kEmptyVolts = 3.30;
    const double kFullVolts = 4.20;

    if (volts < 0)
        return treksafe::kUnknownInt;

    double pct = (volts - kEmptyVolts) / (kFullVolts - kEmptyVolts) * 100.0;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return (int)(pct + 0.5);
}

static treksafe::DeviceState readDevice()
{
    treksafe::DeviceState s;

#ifdef TS_BUTTON_PIN
    int level = digitalRead(TS_BUTTON_PIN);
#if TS_BUTTON_ACTIVE_LOW
    s.buttonPressed = (level == LOW);
#else
    s.buttonPressed = (level == HIGH);
#endif
#endif

    // One reading, used for both, so the percentage and the voltage on the same
    // frame cannot disagree with each other.
    //
    // Cached, and that matters more than it looks. This runs every tick, twenty
    // times a second, and taking a reading means powering the divider, waiting
    // ten milliseconds for it to settle, reading, and powering it down again.
    // Done per tick that is a fifth of the beacon's entire running time spent in
    // a delay, and a divider switched across the cell twenty times a second on a
    // device whose battery has to last until somebody walks in. A battery level
    // does not change twenty times a second, so there is nothing to buy with it.
    double volts = cachedBatteryVolts(millis());
    s.battPct = batteryPercent(volts);
    s.battVolts = volts < 0 ? 0.0 : volts;

    // No GPS is fitted yet, so there is no fix and no clock.
    //
    // This is not a gap being papered over. The contract defines a distress
    // call with no position as valid and the console raises the alarm on it and
    // marks it NO FIX, because a beacon under canopy that has not acquired a
    // lock is exactly the case the product has to handle. A board with no
    // receiver at all reports the same thing honestly.
    //
    // TREKSAFE_SIMULATED_POSITION exists for the demonstration and says what it
    // is in the log every time it is used. It is not a default and a build with
    // it set is not a beacon.
#ifdef TREKSAFE_SIMULATED_POSITION
    s.hasFix = true;
    s.lat = TREKSAFE_SIM_LAT;
    s.lon = TREKSAFE_SIM_LON;
    s.accM = 12;
    s.epoch = treksafe::kClockNotSet; // no GPS means no clock, and no clock means say so
#endif

    return s;
}

// ---------------------------------------------------------------------------
// The indicator
// ---------------------------------------------------------------------------

// Status carried by rhythm rather than by words.
//
// The person holding a beacon may be cold, frightened, or unable to read
// English, and this board has one LED, so pattern is the only channel there is.
// Nothing is conveyed by brightness alone and nothing depends on a colour.
//
//   holding the button   a fast flicker that quickens as the hold completes
//   waiting to be heard  two short pulses, repeating
//   help is coming       a slow steady breathing pulse
//   nothing sent         a fast insistent blink, different from every other
static void renderIndicator(treksafe::Indication what, uint32_t now, uint32_t heldMs)
{
#ifdef TS_LED_PIN
    bool on = false;
    switch (what) {
    case treksafe::IND_HOLDING: {
        // Quickens as the hold nears completion, so the gesture is learnable
        // and can be abandoned before it fires.
        uint32_t period = heldMs > (TREKSAFE_HOLD_MS * 2 / 3) ? 80 : 200;
        on = (now % period) < (period / 2);
        break;
    }
    case treksafe::IND_AWAITING_ACK: {
        uint32_t t = now % 2000;
        on = (t < 120) || (t >= 300 && t < 420);
        break;
    }
    case treksafe::IND_ACKNOWLEDGED:
        on = (now % 3000) < 1500;
        break;
    case treksafe::IND_SEND_FAILED:
        on = (now % 200) < 100;
        break;
    case treksafe::IND_LISTENING:
        // Infrastructure. A brief wink so somebody standing at the box can see
        // it is alive, without spending the solar budget on being seen.
        on = (now % 4000) < 60;
        break;
    case treksafe::IND_IDLE:
    default:
        // A slow, brief wink: "switched on, nothing wrong".
        //
        // Not decoration. A beacon showing nothing at all is indistinguishable
        // from a beacon with a flat battery or a crashed one, and the person
        // carrying it has no way to tell which they have until the moment they
        // need it and press the button. Somebody handed one of these at a
        // trailhead has to be able to see that it is alive.
        //
        // Deliberately brief and slow, because the battery has to last until
        // somebody reaches them: 40 ms every four seconds is one per cent duty,
        // which is nothing next to the radio. It is also a different rhythm
        // from every other state, so it cannot be mistaken for one.
        on = (now % 4000) < 40;
        break;
    }
    digitalWrite(TS_LED_PIN, on ? HIGH : LOW);
#else
    (void)what;
    (void)now;
    (void)heldMs;
#endif
}

// Refuses to run, loudly and forever, saying why.
//
// Used for the failures where carrying on is worse than stopping: no radio, or
// a device id nothing can be attributed to. A node in either state cannot do
// its job and, crucially, looks perfectly healthy from the outside, so the only
// safe behaviour is to be obviously broken.
static void halt(const char *reason)
{
    halted = true;
    snprintf(haltReason, sizeof(haltReason), "%s", reason);
    // Said on the screen as well as the cable. Somebody standing in front of a
    // broken beacon should not have to attach a laptop to discover it is broken.
    display.showHalted(reason);
}

void setup()
{
    Serial.begin(115200);
    delay(200); // let USB enumerate, so the first lines are not lost

#ifdef TS_LED_PIN
    pinMode(TS_LED_PIN, OUTPUT);
    digitalWrite(TS_LED_PIN, LOW);
#endif

#ifdef TS_BUTTON_PIN
#if TS_BUTTON_ACTIVE_LOW
    pinMode(TS_BUTTON_PIN, INPUT_PULLUP);
#else
    pinMode(TS_BUTTON_PIN, INPUT_PULLDOWN);
#endif
#endif

    // Before anything that can fail, so a refusal to run can be shown on the
    // screen rather than only to a laptop that may not be attached.
    if (display.begin())
        display.showStarting("starting");

    treksafe::deviceIdFromSerial(hardwareSerial(), deviceId);

    Serial.println("LOG TrekSafe starting");
    Serial.println(display.present() ? "LOG display found" : "LOG no display on this board");
    Serial.print("LOG device id ");
    Serial.println(deviceId);

    // A device id that is not valid hex cannot be put in a frame: the contract
    // rejects it, so the link layer refuses to attribute anything to it and
    // every single transmission fails. The node would sit there looking alive
    // and send nothing, which for a beacon means a person pressing the button
    // and never being heard. Caught here, said out loud, and stopped.
    if (!treksafe::isValidDevId(deviceId)) {
        halt("this board's device id is not valid hex, so nothing it sends could be "
             "attributed to it and every transmission would fail silently");
        return;
    }

#if defined(TREKSAFE_TRANSPORT_LORA)
    char reason[160];
    if (!radio.begin(reason, sizeof(reason))) {
        halt(reason);
        return;
    }
    Serial.println("LOG radio up");
#else
    Serial.println("LOG no radio in this build: frames travel over this cable, "
                   "prefixed AIR. See firmware/src/transport_serial.h");
#endif

#ifdef TS_BATTERY_ADC_PIN
    // Said every time, because a battery percentage is a number somebody uses
    // to decide how long a person can wait, and this one has never been checked
    // against a multimeter on this board.
    Serial.println("LOG battery readings are UNCALIBRATED and approximate. "
                   "Check TS_BATTERY_MULTIPLIER in boards/heltec_v4.h against a "
                   "known cell before relying on them.");
#endif

    // The link layer's sequence numbers start somewhere unpredictable.
    //
    // Starting at zero every time would mean a beacon that reboots re-using
    // numbers a repeater still remembers, and the repeater would silently drop
    // the first frames of a genuine new distress call. The repeater forgetting
    // old entries is the other half of this; both are needed, because either
    // alone leaves a case where a real call disappears with nothing logged.
    randomSeed(hardwareSerial() ^ micros());
    uint16_t startSeq = (uint16_t)random(0, 65536);

#if defined(TREKSAFE_ROLE_BEACON)
    static treksafe::BeaconRole beacon(radio, deviceId, startSeq);
    role = &beacon;
    Serial.println("LOG role beacon");
#ifdef TREKSAFE_SIMULATED_POSITION
    Serial.println("LOG THIS BUILD REPORTS A SIMULATED POSITION. It is not a beacon.");
#endif
#if TREKSAFE_TRIGGER_AFTER_MS > 0
    Serial.println("LOG THIS BUILD RAISES A DISTRESS CALL BY ITSELF. It is not a beacon.");
#endif
#elif defined(TREKSAFE_ROLE_REPEATER)
    static treksafe::RepeaterRole repeater(radio, deviceId, startSeq, jitterSeed());
    role = &repeater;
    Serial.println("LOG role repeater");
#else
    static treksafe::GatewayRole gateway(radio, host, deviceId, startSeq);
    role = &gateway;
    Serial.println("LOG role gateway");
#endif
}

void loop()
{
    uint32_t now = millis();

    if (halted) {
        // Says why, repeatedly but not constantly, and blinks the failure
        // pattern. A node in this state must be obviously broken rather than
        // quietly useless.
        static uint32_t lastComplaint = 0;
        if (now - lastComplaint >= 5000) {
            lastComplaint = now;
            Serial.print("LOG HALTED: ");
            Serial.println(haltReason);
        }
        renderIndicator(treksafe::IND_SEND_FAILED, now, 0);
        delay(20);
        return;
    }

    // Signed comparison, so a tick boundary computed just ahead of the clock
    // does not underflow into "four billion milliseconds overdue". The same
    // reasoning as everywhere else; see treksafe::elapsedAtLeast.
    if (!treksafe::elapsedAtLeast(now, lastTickMs, kTickMs)) {
        delay(1);
        return;
    }
    lastTickMs = now;

    treksafe::DeviceState state = readDevice();

#if defined(TREKSAFE_ROLE_BEACON)
    // Says when the button goes down and comes up.
    //
    // Kept rather than deleted after it earned its place: the first time this
    // firmware was flashed and the button pressed, nothing happened at all, and
    // there was no way to tell whether the pin was being read, whether the hold
    // was timing out, or whether the tick was running. One line of output
    // separates three completely different faults, and a beacon whose button
    // does nothing is the failure the whole product turns on.
    //
    // Transitions only, never the level, so it cannot flood the cable.
    {
        static bool wasPressed = false;
        if (state.buttonPressed != wasPressed) {
            wasPressed = state.buttonPressed;
            Serial.print("LOG button ");
            Serial.println(state.buttonPressed ? "down" : "up");
        }
    }
#endif

    role->tick(now, state);

#if defined(TREKSAFE_ROLE_BEACON)
    renderIndicator(role->indication(), now, role->heldMs(now));
#else
    renderIndicator(role->indication(), now, 0);
#endif

    // The screen, rendered from exactly the same role state the LED is. The
    // role knows nothing about either.
    if (treksafe::elapsedAtLeast(now, lastScreenMs, kScreenMs)) {
        lastScreenMs = now;

#if defined(TREKSAFE_ROLE_BEACON)
        // A hold in progress redraws every time, because the bar has to move.
        const bool holding = role->indication() == treksafe::IND_HOLDING;
        const uint32_t signature = screenSignature(role->indication(), state.battPct,
                                                   state.hasFix, role->sentCount(),
                                                   holding ? role->heldMs(now) / 100 : 0);
#elif defined(TREKSAFE_ROLE_REPEATER)
        const uint32_t signature = screenSignature(role->indication(), state.battPct, false,
                                                   role->relayedCount(), role->droppedCount());
#else
        const uint32_t signature = screenSignature(role->indication(), state.battPct, false,
                                                   role->sentCount(), role->heardCount());
#endif
        const bool stale = treksafe::elapsedAtLeast(now, lastScreenDrawMs, kScreenRefreshMs);
        if (signature == lastScreenSignature && !stale)
            return;
        lastScreenSignature = signature;
        lastScreenDrawMs = now;

#if defined(TREKSAFE_ROLE_BEACON)
        treksafe::BeaconScreen screen;
        screen.indication = role->indication();
        screen.devid = deviceId;
        screen.hasFix = state.hasFix;
        screen.lat = state.lat;
        screen.lon = state.lon;
        screen.battPct = state.battPct;
        screen.heldMs = role->heldMs(now);
        screen.holdMs = TREKSAFE_HOLD_MS;
        screen.sent = role->sentCount();
        screen.note = 0;
        display.showBeacon(screen);
#elif defined(TREKSAFE_ROLE_REPEATER)
        display.showNode("REPEATER", deviceId, role->relayedCount(), role->droppedCount(),
                         state.battPct);
#else
        display.showNode("GATEWAY", deviceId, role->sentCount(), role->heardCount(),
                         state.battPct);
#endif
    }
}
