// The beacon: the thing a visitor is handed at the trailhead.
//
// One button. Hold it and a distress call goes out, and keeps going out, until
// a human at the base station says they have seen it. That is the entire
// product from the visitor's side, and everything in this file exists to make
// those two sentences true when the person holding it is in the worst state
// they have ever been in.
//
// This class contains no hardware. It is handed a Transport and, once per tick,
// a snapshot of what the device knows, and it returns an intention for the
// indicator. That is what lets the whole state machine below run in a host test
// with no board: see firmware/test/test_roles.cpp.
//
// A beacon does not relay other nodes' traffic. It could: every extra node that
// rebroadcasts makes the mesh better connected. It does not, because its
// battery is the one that has to last until somebody reaches the person
// carrying it, and spending that on other people's packets is a trade made
// against the wrong person. Relaying is the repeater's job and the repeater has
// a solar panel.

#pragma once

#include "config.h"
#include "contract.h"
#include "link.h"
#include "timing.h"
#include "transport.h"

namespace treksafe {

class BeaconRole {
  public:
    enum State {
        STATE_IDLE,         // nothing has happened
        STATE_AWAITING_ACK, // a call has gone out, nobody has answered
        STATE_ACKNOWLEDGED, // a human at the base station has seen it
    };

    // startSeq seeds the link layer sequence numbers.
    //
    // The caller passes something unpredictable, drawn from hardware at boot.
    // Starting at zero every time would mean that a beacon which reboots
    // re-uses sequence numbers a repeater may still remember, and the repeater
    // would silently drop the first frames of a real new distress call. The
    // repeater forgets old entries for the same reason; this is the other half,
    // and both are needed because either alone leaves a case where a genuine
    // call disappears with nothing logged anywhere. See SeenCache in link.h.
    BeaconRole(Transport &transport, const char *devid, uint16_t startSeq);

    // Call once per tick with the current clock and a fresh snapshot.
    void tick(uint32_t nowMs, const DeviceState &state);

    // Raises a distress call. The one place that happens, whatever triggered
    // it, so there is exactly one description of what raising a call means and
    // a change to it cannot apply to the button but not to the test trigger.
    //
    // Public because a second button for a specific category, a test jig, or
    // the simulator's timed trigger all need it. It takes the same snapshot a
    // tick does, deliberately: an earlier version raised the call from nothing
    // and let the position follow, which meant every trigger transmitted a
    // position-less frame and then immediately transmitted a second one as the
    // fix "arrived". Two transmissions and twice the airtime for one press.
    void raiseDistress(char category, uint32_t nowMs, const DeviceState &state);

    State state() const { return state_; }
    Indication indication() const { return indication_; }
    const char *deviceId() const { return devid_; }

    // How long the current hold has been running, so the hardware layer can
    // show it building. Zero when not holding.
    uint32_t heldMs(uint32_t nowMs) const { return hold_.heldMs(nowMs); }

    // Counters, for the log and for tests. Nothing decides anything on these.
    uint32_t sentCount() const { return sent_; }
    uint32_t failedCount() const { return failed_; }

  private:
    void sendDistress(uint32_t nowMs, const DeviceState &state);
    void sendCheckIn(uint32_t nowMs, const DeviceState &state);
    bool transmit(const char *payload);
    void pollForAck(uint32_t nowMs);
    void fillDistress(Distress &d, const DeviceState &state) const;
    void updateIndication();

    Transport &transport_;
    char devid_[kDevIdMax];
    uint16_t seq_;

    State state_;
    Indication indication_;
    char category_;

    HoldToFire hold_;
    RepeatSchedule repeats_;

    // Whether the last distress call that went out carried a position. Drives
    // the follow-up: a lock that arrives after the first call goes out at once
    // rather than waiting for the next repeat, because the base station is
    // holding an incident with no position on it and somebody is deciding where
    // to send people.
    bool lastSosHadFix_;

    uint32_t lastCheckInMs_;
    uint32_t sent_;
    uint32_t failed_;

#if TREKSAFE_TRIGGER_AFTER_MS > 0
    bool testTriggerFired_;
#endif
};

} // namespace treksafe
