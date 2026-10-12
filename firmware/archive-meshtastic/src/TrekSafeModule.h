// TrekSafe beacon module for Meshtastic.
//
// This is a module, not a fork. See D1 in docs/decisions.md. Everything here
// sits alongside an unmodified upstream checkout; the only change to upstream
// is one line registering the module, kept as a patch in
// firmware/integration/. See firmware/README.md for how to build it.
//
// All frame construction and acknowledgement parsing lives in TrekSafeFrames,
// which has no Meshtastic dependency and is tested on the host. This file is
// the glue: buttons, GPS, battery, radio, LED. Keep logic out of it, because
// none of it can be tested without hardware.

#pragma once

#include "SinglePortModule.h"
#include "TrekSafeFrames.h"
#include "concurrency/OSThread.h"

// ---------------------------------------------------------------------------
// Settings. Everything below is a proposal except where it cites a decision.
// ---------------------------------------------------------------------------

// How long the button must be held before a distress call goes out. A beacon
// that fires on a knock in a backpack sends wardens up a mountain for nothing.
// A beacon that needs a deliberate gesture is still usable by someone cold and
// frightened, as long as the gesture is one motion. Not ratified.
#ifndef TREKSAFE_HOLD_MS
#define TREKSAFE_HOLD_MS 3000
#endif

// Which pin the distress button is on.
//
// This deliberately does not default to the board's user button. On a Heltec
// V4 running stock Meshtastic, a long press on that button is the shutdown
// gesture, so a hold-to-fire distress button on the same pin would fight it. A
// real beacon has its own button anyway. Set this for your hardware.
#ifndef TREKSAFE_BUTTON_PIN
#define TREKSAFE_BUTTON_PIN -1
#endif

// Active low is the usual wiring, button to ground with an internal pull-up.
#ifndef TREKSAFE_BUTTON_ACTIVE_LOW
#define TREKSAFE_BUTTON_ACTIVE_LOW 1
#endif

// PROPOSED, NOT RATIFIED. docs/payload-contract.md lists "whether the beacon
// should repeat the SOS on an interval until acknowledged, and what that
// interval costs in airtime" as an open question.
//
// This implementation repeats, on a widening interval, and stops only when an
// acknowledgement arrives. The reasoning: a single distress call that does not
// reach a gateway is lost forever, and the person has no way to know. The
// widening interval is the concession to airtime, since the first minutes are
// the ones where a repeat is most likely to find a gateway that a moving
// relay or a changing antenna angle has just brought into range.
//
// The cost is that the console shows one incident per repeat until issue I10
// is fixed. That is deliberate: duplicate incidents on a screen are a nuisance,
// a lost distress call is not.
#ifndef TREKSAFE_SOS_REPEAT_MS
#define TREKSAFE_SOS_REPEAT_MS                                                                                   \
    {                                                                                                            \
        30000, 60000, 120000, 300000                                                                             \
    }
#endif

// Test affordance for the native simulator, which has no button to press.
// When set, the module raises one distress call this many milliseconds after
// boot and then behaves exactly as it would after a real press. Zero, the
// default, means never: a shipped beacon fires only when a person holds the
// button.
//
//   pio run -e native-macos -- -DTREKSAFE_TRIGGER_AFTER_MS=10000
#ifndef TREKSAFE_TRIGGER_AFTER_MS
#define TREKSAFE_TRIGGER_AFTER_MS 0
#endif

// Once acknowledged, the beacon keeps the console's last known position fresh
// without raising anything.
#ifndef TREKSAFE_CHECKIN_MS
#define TREKSAFE_CHECKIN_MS 600000
#endif

// Fixed repeater nodes report health on this interval so a dead repeater is
// known before it matters rather than during an incident.
#ifndef TREKSAFE_TELEMETRY_MS
#define TREKSAFE_TELEMETRY_MS 900000
#endif

class TrekSafeModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    enum Role {
        ROLE_BEACON, // carried by a visitor, has a button
        ROLE_FIXED,  // solar repeater, reports telemetry, no button
    };

    enum State {
        STATE_IDLE,         // nothing has happened
        STATE_AWAITING_ACK, // distress sent, nobody has confirmed yet
        STATE_ACKNOWLEDGED, // a human at the base station has seen it
    };

    explicit TrekSafeModule(Role role = ROLE_BEACON);

    // Raises a distress call. Exposed so a different trigger, a test jig, or a
    // second button for a specific category can drive it without going through
    // the hold gesture.
    void triggerDistress(char category = treksafe::CAT_UNSPECIFIED);

    State state() const { return state_; }
    const char *deviceId() const { return devid_; }

  protected:
    // Watches for an acknowledgement addressed to this beacon. Always returns
    // CONTINUE so ordinary text messages still reach the phone app.
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    virtual int32_t runOnce() override;

  private:
    // Every one of these takes the tick's clock rather than reading millis()
    // again. See treksafe::elapsedAtLeast for what goes wrong otherwise.
    void sendFrame(const char *frame, bool distress);
    void sendDistress(uint32_t now);
    void sendCheckIn(uint32_t now);
    void sendTelemetry(uint32_t now);
    void fillPosition(treksafe::Distress &d);
    void onAcknowledged(const treksafe::Ack &ack);
    bool buttonPressed() const;
    void updateIndicator(uint32_t now);

    Role role_;
    State state_;
    char devid_[treksafe::kDevIdMax];
    char category_;

    treksafe::HoldToFire hold_;
    treksafe::RepeatSchedule repeats_;

    bool lastSosHadFix_; // drives the follow-up when a lock arrives late
#if TREKSAFE_TRIGGER_AFTER_MS > 0
    bool testTriggerFired_;
#endif

    uint32_t lastCheckInMs_;
    uint32_t lastTelemetryMs_;
    bool sentTelemetry_; // never sent is not the same as sent at millis() zero
    uint32_t ackedAtMs_;
};

extern TrekSafeModule *trekSafeModule;
