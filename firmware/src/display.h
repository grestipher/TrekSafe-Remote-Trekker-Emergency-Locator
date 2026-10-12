// The screen on the front of the board.
//
// WHAT IT IS FOR, AND WHAT IT IS NOT
//
// CLAUDE.md carries a rule about the beacon: assume the person holding it is
// cold, injured, frightened, or does not read English, so status must be
// legible without words, by colour and pattern rather than colour alone. That
// rule is not repealed by adding a screen. The LED keeps carrying the whole
// message on its own, and anybody who cannot read this display still knows what
// the beacon is doing.
//
// What the screen adds is confirmation, which is the one thing a rhythm is bad
// at. D3 says the acknowledgement is the core of the product: the reason a
// beacon must reply is that a person who thinks nobody heard them starts
// moving, and moving is what turns a short rescue into a multi-day search. A
// slow pulse means "help is coming" only to somebody who was told what it
// means. "HELP IS COMING" on a screen means it to everybody who reads English,
// and the pulse still means it to everybody who does not.
//
// So every screen here pairs a large symbol with a word. Neither is load
// bearing alone.
//
// On a repeater or a gateway the reasoning is different and simpler: nobody is
// frightened, somebody is standing at a box on a ridge with a laptop bag trying
// to work out whether it is worth opening, and a screen answers that in one
// look.
//
// HOW IT FITS THE REST OF THE FIRMWARE
//
// Roles never touch this. They report an Indication, exactly as they do for the
// LED, and main.cpp renders it. That keeps every role testable on a host with
// no display, no Arduino, and no board, which is the whole point of
// transport.h and is worth just as much here.

#pragma once

#include "../boards/heltec_v4.h"
#include "contract.h"
#include "transport.h"

#include <stdint.h>

namespace treksafe {

// What to put on a beacon's screen. Gathered into one struct rather than passed
// as eight arguments, so adding a field does not touch every call site.
struct BeaconScreen {
    Indication indication;
    const char *devid;
    bool hasFix;
    double lat;
    double lon;
    int battPct;      // kUnknownInt when there is no reading
    uint32_t heldMs;  // how long the button has been held, for the progress bar
    uint32_t holdMs;  // how long a hold has to be, so the bar has a scale
    uint32_t sent;    // how many calls have gone out
    const char *note; // one short line, or null
};

class Display {
  public:
    Display();

    // Powers the panel and initialises it. Returns false when there is none,
    // which is not an error: a repeater in a weatherproof box has no reason to
    // carry a screen, and the firmware must run identically without one.
    //
    // Powering it is the part that is easy to miss. The panel is fed through
    // VEXT, so nothing appears until that is driven, and a dark screen is far
    // more often a power problem than a driver one.
    bool begin();

    bool present() const { return present_; }

    // Beacon screens. Each pairs a large symbol with a word, per the note above.
    void showBeacon(const BeaconScreen &s);

    // Infrastructure screens: what a person standing at the box needs.
    void showNode(const char *role, const char *devid, uint32_t relayed,
                  uint32_t heard, int battPct);

    // The screen that matters most, because the alternative is a board that
    // looks fine and is not. Shown when the firmware refuses to run.
    void showHalted(const char *reason);

    // A splash at startup, so a board that is powered but idle still proves it
    // is alive to somebody looking at it rather than at a terminal.
    void showStarting(const char *role);

    // Live pin readings, for finding a button whose wiring nobody documented.
    // Kept out of every normal build; see main.cpp.
    void showPinHunt(const char *line1, const char *line2);

  private:
    void clear();
    void flush();
    void centred(int y, const char *text, bool large);

    bool present_;
};

} // namespace treksafe
