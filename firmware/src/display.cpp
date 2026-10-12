#include "display.h"

#include <Arduino.h>
#include <SSD1306Wire.h>

#include <stdio.h>
#include <string.h>

namespace treksafe {

namespace {

// The panel. Constructed here rather than in the class so that display.h can be
// included by anything without dragging the driver in behind it.
SSD1306Wire gPanel(TS_OLED_ADDR, TS_OLED_SDA, TS_OLED_SCL);

// Turns the display's power rail on.
//
// Separate from begin() because the ordering matters and is easy to get wrong:
// power, settle, release reset, settle, then talk. A controller addressed before
// its reset is released answers nothing, and the usual conclusion drawn from
// that is that the pin map is wrong when it is not.
void powerOn()
{
    pinMode(TS_VEXT_PIN, OUTPUT);
#if TS_VEXT_ACTIVE_LOW
    digitalWrite(TS_VEXT_PIN, LOW);
#else
    digitalWrite(TS_VEXT_PIN, HIGH);
#endif
    delay(60);

    pinMode(TS_OLED_RST, OUTPUT);
    digitalWrite(TS_OLED_RST, LOW);
    delay(30);
    digitalWrite(TS_OLED_RST, HIGH);
    delay(60);
}

// Is anything actually on the bus.
//
// Checked rather than assumed, so a board with no panel runs perfectly and says
// so once, instead of blocking or pretending it drew something.
bool panelAnswers()
{
    Wire.beginTransmission(TS_OLED_ADDR);
    return Wire.endTransmission() == 0;
}

} // namespace

Display::Display() : present_(false) {}

bool Display::begin()
{
#if !TS_HAS_DISPLAY
    return false;
#else
    powerOn();

    Wire.begin(TS_OLED_SDA, TS_OLED_SCL, 400000);
    if (!panelAnswers())
        return false;

    present_ = gPanel.init();
    if (!present_)
        return false;

    // The panel is mounted with its ribbon at the top on this board, so the
    // image needs flipping or everything reads upside down.
    gPanel.flipScreenVertically();
    gPanel.setContrast(255);
    clear();
    flush();
    return true;
#endif
}

void Display::clear()
{
    if (present_)
        gPanel.clear();
}

void Display::flush()
{
    if (present_)
        gPanel.display();
}

void Display::centred(int y, const char *text, bool large)
{
    if (!present_)
        return;
    gPanel.setTextAlignment(TEXT_ALIGN_CENTER);
    gPanel.setFont(large ? ArialMT_Plain_16 : ArialMT_Plain_10);
    gPanel.drawString(TS_OLED_WIDTH / 2, y, text);
}

void Display::showStarting(const char *role)
{
    if (!present_)
        return;
    clear();
    centred(6, "TrekSafe", true);
    centred(30, role, false);
    centred(46, "starting", false);
    flush();
}

void Display::showBeacon(const BeaconScreen &s)
{
    if (!present_)
        return;
    clear();

    char line[40];

    switch (s.indication) {
    case IND_HOLDING: {
        // A progress bar rather than a countdown in words. Somebody holding a
        // button needs to know the gesture is working and roughly how much
        // longer, and a filling bar says both without being read.
        centred(2, "KEEP HOLDING", true);
        uint32_t pct = s.holdMs ? (s.heldMs * 100u) / s.holdMs : 0;
        if (pct > 100)
            pct = 100;
        gPanel.drawProgressBar(8, 28, TS_OLED_WIDTH - 20, 14, pct);
        centred(48, "to call for help", false);
        break;
    }

    case IND_AWAITING_ACK:
        // The large word is the state a frightened person needs first. The line
        // under it is the instruction that actually keeps them alive, and it is
        // the whole reason the acknowledgement exists: people who believe
        // nobody heard them start moving.
        centred(0, "CALLING", true);
        centred(18, "FOR HELP", true);
        centred(38, "Stay where you are", false);
        snprintf(line, sizeof(line), "%s  sent %lu", s.devid, (unsigned long)s.sent);
        centred(52, line, false);
        break;

    case IND_ACKNOWLEDGED:
        // The confirmation. This screen is the product.
        centred(0, "HELP IS", true);
        centred(18, "COMING", true);
        centred(38, "They know where you are", false);
        centred(52, "Stay where you are", false);
        break;

    case IND_SEND_FAILED:
        // Never dressed up. A beacon that cannot transmit must not look like one
        // that is waiting, because the person would wait.
        centred(0, "NOT SENT", true);
        centred(22, "This beacon could not", false);
        centred(34, "call for help.", false);
        centred(50, "Move and try again", false);
        break;

    case IND_IDLE:
    default:
        centred(0, "READY", true);
        if (s.hasFix) {
            snprintf(line, sizeof(line), "%.4f, %.4f", s.lat, s.lon);
            centred(22, line, false);
        } else {
            // Not an error, and not hidden. A beacon with no lock still calls
            // for help, and the person should know the position is missing
            // rather than discover it later.
            centred(22, "no position yet", false);
        }
        if (s.battPct >= 0) {
            snprintf(line, sizeof(line), "battery %d%%", s.battPct);
            centred(34, line, false);
        }
        snprintf(line, sizeof(line), "Hold button for help");
        centred(50, line, false);
        break;
    }

    if (s.note && *s.note)
        centred(52, s.note, false);

    // The device id, always, small, in the corner. It is what a warden reads
    // back over the radio and what the registry is looked up by.
    gPanel.setTextAlignment(TEXT_ALIGN_LEFT);
    gPanel.setFont(ArialMT_Plain_10);
    if (s.indication == IND_IDLE)
        gPanel.drawString(0, 0, s.devid);

    flush();
}

void Display::showNode(const char *role, const char *devid, uint32_t relayed, uint32_t heard,
                       int battPct)
{
    if (!present_)
        return;
    clear();

    char line[40];
    centred(0, role, true);

    snprintf(line, sizeof(line), "id %s", devid);
    centred(20, line, false);

    // Both numbers, because "carried nothing" and "heard nothing" are different
    // faults and a technician on a ridge needs to tell them apart.
    snprintf(line, sizeof(line), "heard %lu  relayed %lu", (unsigned long)heard,
             (unsigned long)relayed);
    centred(32, line, false);

    if (battPct >= 0)
        snprintf(line, sizeof(line), "battery %d%%", battPct);
    else
        snprintf(line, sizeof(line), "battery unknown");
    centred(48, line, false);

    flush();
}

void Display::showHalted(const char *reason)
{
    if (!present_)
        return;
    clear();
    centred(0, "NOT WORKING", true);

    // The reason, wrapped across the remaining lines. A person standing in
    // front of a broken beacon should not have to attach a laptop to find out
    // that it is broken.
    if (present_) {
        gPanel.setTextAlignment(TEXT_ALIGN_LEFT);
        gPanel.setFont(ArialMT_Plain_10);
        gPanel.drawStringMaxWidth(0, 20, TS_OLED_WIDTH, reason);
    }
    flush();
}

void Display::showPinHunt(const char *line1, const char *line2)
{
    if (!present_)
        return;
    clear();
    centred(0, "PRESS BUTTON", true);
    centred(24, line1, false);
    centred(42, line2, false);
    flush();
}

} // namespace treksafe
