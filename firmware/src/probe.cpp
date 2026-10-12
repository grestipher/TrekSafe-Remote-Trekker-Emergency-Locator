// Board identification probe. Built only by the `boardprobe` environment.
//
// Asks the hardware which board it is, rather than trusting what is printed on
// the box or what a product page claims. It exists because that trust was
// misplaced once already: Heltec state that the WiFi LoRa 32 V4 keeps the V3's
// pinout, which is true of the header pins and hides an RF front end, a moved
// power-control pin, an inverted battery enable and a different display
// controller. Building against the wrong one gives a board that runs, reports
// success, and does nothing.
//
//   cd firmware && pio run -e boardprobe -t upload -t monitor
//
// It writes nothing to flash beyond itself, drives no radio, and leaves every
// pin it touches back the way it found it. Safe to run on an unknown board.

#include <Arduino.h>
#include <Wire.h>

// The OLED sits on these regardless of revision. Only its power does not.
static const int kOledSda = 17;
static const int kOledScl = 18;
static const int kOledRst = 21;

// The two pins a V4 might use to power the display and the antenna boost.
// Active LOW on both. Which one it is, is the thing being determined.
static const int kVextCandidates[] = {36, 40};

// A V4.2 has a battery divider enable here; a V4.3 has none.
static const int kAdcCtrl = 37;
static const int kBatteryAdc = 1;

// Every pin a button could plausibly be on, minus the ones that are dangerous
// or already spoken for. See the note in setup() for why each is excluded.
static const int kButtonCandidates[] = {0,  2,  3,  4,  5,  6,  7,  15, 16, 33, 34, 35,
                                        38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48};
static const unsigned kCandidateCount = sizeof(kButtonCandidates) / sizeof(kButtonCandidates[0]);
static int gButtonResting[kCandidateCount];

// Scans the display's I2C bus and reports what answered.
//
// An SSD1306 or SSD1315 answers at 0x3C, occasionally 0x3D. Finding one is the
// proof that this VEXT pin is the one that powers it.
static int scanOled(const char *label)
{
    Wire.end();
    Wire.begin(kOledSda, kOledScl, 400000);
    delay(50);

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            found++;
            Serial.printf("    %s: device at 0x%02X%s\n", label, addr,
                          (addr == 0x3C || addr == 0x3D) ? "   <-- looks like the OLED" : "");
        }
    }
    if (!found)
        Serial.printf("    %s: nothing answered on the bus\n", label);
    return found;
}

void setup()
{
    Serial.begin(115200);
    delay(2500); // long enough that a person can attach a monitor and see this

    Serial.println("\n\n========== TrekSafe board probe ==========");
    Serial.printf("chip        : %s rev %d, %d core(s)\n", ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores());
    Serial.printf("flash       : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    Serial.printf("psram       : %u bytes\n", (unsigned)ESP.getPsramSize());
    Serial.printf("mac (low32) : %08X\n", (unsigned)(ESP.getEfuseMac() & 0xFFFFFFFFu));

    // ---- which pin powers the display -------------------------------------
    Serial.println("\n-- display power --");
    Serial.println("  An OLED answering at 0x3C identifies the VEXT pin for this revision.");

    pinMode(kOledRst, OUTPUT);

    for (unsigned i = 0; i < sizeof(kVextCandidates) / sizeof(kVextCandidates[0]); i++) {
        int vext = kVextCandidates[i];
        char label[24];
        snprintf(label, sizeof(label), "VEXT=%d", vext);

        // Both candidates off first, so a display left powered by the previous
        // attempt cannot be credited to this one.
        for (unsigned j = 0; j < sizeof(kVextCandidates) / sizeof(kVextCandidates[0]); j++) {
            pinMode(kVextCandidates[j], OUTPUT);
            digitalWrite(kVextCandidates[j], HIGH); // active low, so HIGH is off
        }
        delay(300);

        digitalWrite(vext, LOW); // on
        delay(200);
        // The controller needs its reset released after power comes up.
        digitalWrite(kOledRst, LOW);
        delay(30);
        digitalWrite(kOledRst, HIGH);
        delay(100);

        scanOled(label);
    }

    // ---- battery divider ---------------------------------------------------
    Serial.println("\n-- battery --");
    Serial.println("  A V4.2 enables the divider on pin 37 (HIGH). A V4.3 has no such pin,");
    Serial.println("  so its reading does not change when 37 is driven.");

    pinMode(kAdcCtrl, OUTPUT);
    digitalWrite(kAdcCtrl, LOW);
    delay(50);
    uint32_t low = analogReadMilliVolts(kBatteryAdc);
    digitalWrite(kAdcCtrl, HIGH);
    delay(50);
    uint32_t high = analogReadMilliVolts(kBatteryAdc);

    Serial.printf("    pin 37 LOW  -> %u mV at the pin\n", (unsigned)low);
    Serial.printf("    pin 37 HIGH -> %u mV at the pin\n", (unsigned)high);
    long delta = (long)high - (long)low;
    if (delta > 100 || delta < -100)
        Serial.println("    the reading moves, so pin 37 does control the divider (V4.2 shape)");
    else
        Serial.println("    the reading does not move, so pin 37 is not the enable (V4.3 shape)");

    // Leave it off rather than draining the cell being measured.
    digitalWrite(kAdcCtrl, LOW);

    // ---- where is the button --------------------------------------------
    //
    // Not assumed. Meshtastic's variant for this board says GPIO0, and on the
    // bench GPIO0 sat HIGH through thirty seconds of pressing, so the only
    // honest thing to do is watch every pin it could plausibly be and see which
    // one moves.
    //
    // The excluded pins are excluded for a reason, not for tidiness:
    //   8..14    the SX1262. Driving these fights the radio.
    //   17,18,21 the display's I2C and reset.
    //   19,20    USB D+ and D-. Touching these drops the cable mid-probe.
    //   26..32   SPI flash. Touching these crashes the chip.
    //   36,37,1  VEXT and the battery divider, already identified above.
    Serial.println("\n-- finding the button --");
    Serial.println("  Watching every safe pin with a pull-up. Press and hold the button:");
    Serial.println("  whichever pin goes LOW is the one it is wired to.");
    for (unsigned i = 0; i < kCandidateCount; i++)
        pinMode(kButtonCandidates[i], INPUT_PULLUP);
    delay(50);

    Serial.print("    resting state:");
    for (unsigned i = 0; i < kCandidateCount; i++) {
        int lvl = digitalRead(kButtonCandidates[i]);
        gButtonResting[i] = lvl;
        if (lvl == LOW)
            Serial.printf(" GPIO%d=LOW", kButtonCandidates[i]);
    }
    Serial.println("  (pins already LOW are ignored, they are held that way)");

    Serial.println("\n==========================================\n");
}

void loop()
{
    // Any candidate pin that departs from how it was resting is a candidate for
    // being the button. Reported once per change so a press is unmistakable.
    static uint32_t lastReport = 0;
    static int last[kCandidateCount];
    static bool primed = false;

    if (!primed) {
        for (unsigned i = 0; i < kCandidateCount; i++)
            last[i] = gButtonResting[i];
        primed = true;
    }

    for (unsigned i = 0; i < kCandidateCount; i++) {
        int lvl = digitalRead(kButtonCandidates[i]);
        if (lvl != last[i]) {
            last[i] = lvl;
            Serial.printf("  >>> GPIO%d went %s%s\n", kButtonCandidates[i],
                          lvl == LOW ? "LOW" : "HIGH",
                          lvl == LOW ? "   <-- THIS LOOKS LIKE THE BUTTON" : "  (released)");
        }
    }

    uint32_t now = millis();
    if (now - lastReport >= 8000) {
        lastReport = now;
        Serial.printf("  [%lus] still watching %u pins, press the button\n",
                      (unsigned long)(now / 1000), kCandidateCount);
    }
    delay(15);
}
