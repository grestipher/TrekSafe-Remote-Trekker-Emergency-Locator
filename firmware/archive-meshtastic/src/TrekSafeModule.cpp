#include "TrekSafeModule.h"

#include "MeshService.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "RTC.h"
#include "configuration.h"
#include "main.h"

#include <string.h>

TrekSafeModule *trekSafeModule;

static const uint32_t kRepeatSchedule[] = TREKSAFE_SOS_REPEAT_MS;
static const uint8_t kRepeatSteps = sizeof(kRepeatSchedule) / sizeof(kRepeatSchedule[0]);

TrekSafeModule::TrekSafeModule(Role role)
    : SinglePortModule("treksafe", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("TrekSafe"),
      role_(role), state_(STATE_IDLE), category_(treksafe::CAT_UNSPECIFIED), hold_(TREKSAFE_HOLD_MS),
      repeats_(kRepeatSchedule, kRepeatSteps), lastSosHadFix_(false),
#if TREKSAFE_TRIGGER_AFTER_MS > 0
      testTriggerFired_(false),
#endif
      lastCheckInMs_(0),
      lastTelemetryMs_(0), sentTelemetry_(false), ackedAtMs_(0)
{
    // The device id is the low half of the Meshtastic node number, which is
    // stable for the life of the board and needs no separate provisioning
    // step. The mesh carries this and never a name. See I9.
    treksafe::deviceIdFromNodeNum(nodeDB->getNodeNum(), devid_);

#if TREKSAFE_BUTTON_PIN >= 0
    if (role_ == ROLE_BEACON) {
#if TREKSAFE_BUTTON_ACTIVE_LOW
        pinMode(TREKSAFE_BUTTON_PIN, INPUT_PULLUP);
#else
        pinMode(TREKSAFE_BUTTON_PIN, INPUT_PULLDOWN);
#endif
    }
#endif

#ifdef LED_PIN
    pinMode(LED_PIN, OUTPUT);
#endif

    LOG_INFO("TrekSafe module up, device id %s, role %s", devid_, role_ == ROLE_BEACON ? "beacon" : "fixed");
}

bool TrekSafeModule::buttonPressed() const
{
#if TREKSAFE_BUTTON_PIN >= 0
    int v = digitalRead(TREKSAFE_BUTTON_PIN);
#if TREKSAFE_BUTTON_ACTIVE_LOW
    return v == LOW;
#else
    return v == HIGH;
#endif
#else
    return false;
#endif
}

void TrekSafeModule::fillPosition(treksafe::Distress &d)
{
    d.devid = devid_;
    d.hasFix = false;
    d.lat = 0;
    d.lon = 0;
    d.accM = treksafe::kUnknownInt;
    d.epoch = treksafe::kClockNotSet;
    d.battPct = treksafe::kUnknownInt;
    d.cat = category_;

    // No lock is a first class state. The frame goes out either way and the
    // position follows when it arrives.
    if (gpsStatus && gpsStatus->getHasLock()) {
        d.hasFix = true;
        d.lat = gpsStatus->getLatitude() * 1e-7;
        d.lon = gpsStatus->getLongitude() * 1e-7;
    }

    // Accuracy is left unknown on purpose. Meshtastic exposes HDOP, which is a
    // unitless dilution figure, not metres. Multiplying it by a guessed
    // constant would put a number on the warden's screen that looks like a
    // measurement and is not one. The console already shows accuracy as
    // unknown when the field is empty.

    // Zero means the clock was never set, which the contract reads as no valid
    // sender clock rather than 1970.
    d.epoch = getValidTime(RTCQualityDevice);

    if (powerStatus && powerStatus->getHasBattery())
        d.battPct = powerStatus->getBatteryChargePercent();
}

void TrekSafeModule::sendFrame(const char *frame, bool distress)
{
    size_t len = strlen(frame);
    if (len == 0)
        return;

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        LOG_ERROR("TrekSafe could not allocate a packet for: %s", frame);
        return;
    }

    p->to = NODENUM_BROADCAST;
    p->want_ack = false; // the contract's ACK frame is the acknowledgement that matters
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, frame, len);

    // A distress call outranks routine traffic on a congested mesh. This is
    // the routing priority that D1 named as the one reason a fork might be
    // needed, and a module can set it.
    p->priority = distress ? meshtastic_MeshPacket_Priority_HIGH : meshtastic_MeshPacket_Priority_DEFAULT;

    LOG_INFO("TrekSafe tx: %s", frame);
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

void TrekSafeModule::sendDistress(uint32_t now)
{
    treksafe::Distress d;
    fillPosition(d);

    char frame[treksafe::kFrameMax];
    if (treksafe::buildSos(frame, sizeof(frame), d) == 0) {
        LOG_ERROR("TrekSafe could not build a distress frame");
        return;
    }

    sendFrame(frame, true);
    // The tick's clock, not a fresh millis(): a timestamp taken after the
    // tick began would sit in the future relative to this tick's checks.
    repeats_.sent(now);
    lastSosHadFix_ = d.hasFix;
}

// now is the tick's clock, for the same reason sendDistress takes it: a stamp
// taken with a fresh millis() here would sit after the now this tick just
// compared against, and the next comparison would underflow and fire again
// immediately. See elapsedAtLeast.
void TrekSafeModule::sendCheckIn(uint32_t now)
{
    treksafe::Distress d;
    fillPosition(d);

    char frame[treksafe::kFrameMax];
    if (treksafe::buildOk(frame, sizeof(frame), d) == 0)
        return;

    sendFrame(frame, false);
    lastCheckInMs_ = now;
}

void TrekSafeModule::sendTelemetry(uint32_t now)
{
    int batt = treksafe::kUnknownInt;
    double volts = 0;
    if (powerStatus && powerStatus->getHasBattery()) {
        batt = powerStatus->getBatteryChargePercent();
        volts = powerStatus->getBatteryVoltageMv() / 1000.0;
    }

    char frame[treksafe::kFrameMax];
    if (treksafe::buildTel(frame, sizeof(frame), devid_, batt, volts, 0.0) == 0)
        return;

    sendFrame(frame, false);
    lastTelemetryMs_ = now;
    sentTelemetry_ = true;
}

void TrekSafeModule::triggerDistress(char category)
{
    category_ = category;

    if (state_ == STATE_IDLE || state_ == STATE_ACKNOWLEDGED)
        repeats_.restart();
    state_ = STATE_AWAITING_ACK;

    LOG_WARN("TrekSafe distress raised on %s", devid_);
    sendDistress(millis());
}

void TrekSafeModule::onAcknowledged(const treksafe::Ack &ack)
{
    if (state_ != STATE_AWAITING_ACK)
        return;

    state_ = STATE_ACKNOWLEDGED;
    // One reading, used for both. Two calls to millis() here would put the
    // check-in clock after the acknowledgement clock for no reason.
    ackedAtMs_ = millis();
    lastCheckInMs_ = ackedAtMs_;

    LOG_INFO("TrekSafe acknowledged by the base station, incident %s",
             ack.hasIncident ? ack.incident : "(none given)");
}

ProcessMessage TrekSafeModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag)
        return ProcessMessage::CONTINUE;

    char line[treksafe::kFrameMax];
    size_t n = mp.decoded.payload.size;
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    memcpy(line, mp.decoded.payload.bytes, n);
    line[n] = '\0';

    treksafe::Ack ack;
    if (treksafe::parseAck(line, devid_, ack))
        onAcknowledged(ack);

    // Never swallow the packet. Anything that is not for us is ordinary mesh
    // traffic and must still reach the phone app and the other modules.
    return ProcessMessage::CONTINUE;
}

void TrekSafeModule::updateIndicator(uint32_t now)
{
#ifdef LED_PIN
    // Status has to be readable by someone who is cold, frightened, or does
    // not read English, so it is carried by rhythm rather than by words. The
    // board has one LED, so pattern is the only channel there is.
    //
    //   holding the button   fast flicker that quickens as the hold completes
    //   waiting to be heard  two short pulses, repeating
    //   help is coming       slow steady breathing pulse
    bool on = false;

    if (hold_.holding()) {
        uint32_t held = hold_.heldMs(now);
        uint32_t period = held > (TREKSAFE_HOLD_MS * 2 / 3) ? 80 : 200;
        on = (now % period) < (period / 2);
    } else if (state_ == STATE_AWAITING_ACK) {
        uint32_t t = now % 2000;
        on = (t < 120) || (t >= 300 && t < 420);
    } else if (state_ == STATE_ACKNOWLEDGED) {
        uint32_t t = now % 3000;
        on = t < 1500;
    }

    digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
    (void)now;
#endif
}

int32_t TrekSafeModule::runOnce()
{
    uint32_t now = millis();

    if (role_ == ROLE_BEACON) {
        if (hold_.update(buttonPressed(), now))
            triggerDistress(treksafe::CAT_UNSPECIFIED);

#if TREKSAFE_TRIGGER_AFTER_MS > 0
        // Simulator only. See the note in the header.
        if (!testTriggerFired_ && now >= (uint32_t)TREKSAFE_TRIGGER_AFTER_MS) {
            testTriggerFired_ = true;
            LOG_WARN("TrekSafe test trigger firing, this build is not for a real beacon");
            triggerDistress(treksafe::CAT_INJURY);
        }
#endif

        if (state_ == STATE_AWAITING_ACK) {
            // A lock that arrives after the first call goes out immediately
            // rather than waiting for the next repeat. The console is holding
            // an incident with no position on it.
            bool haveFixNow = gpsStatus && gpsStatus->getHasLock();
            if (!lastSosHadFix_ && haveFixNow) {
                LOG_INFO("TrekSafe acquired a fix, sending position");
                sendDistress(now);
            } else if (repeats_.due(now)) {
                sendDistress(now);
            }
        } else if (state_ == STATE_ACKNOWLEDGED) {
            if (treksafe::elapsedAtLeast(now, lastCheckInMs_, TREKSAFE_CHECKIN_MS))
                sendCheckIn(now);
        }
    } else {
        // A repeater with a stamp of zero has never sent, which is not the same
        // as having sent at millis() zero. Tracked with a flag rather than by
        // reading a sentinel out of the timestamp.
        if (!sentTelemetry_ || treksafe::elapsedAtLeast(now, lastTelemetryMs_, TREKSAFE_TELEMETRY_MS))
            sendTelemetry(now);
    }

    updateIndicator(now);

    // Fast enough that the button feels immediate and the LED pattern is
    // smooth. This runs on the Meshtastic thread scheduler, not a busy loop.
    return 50;
}
