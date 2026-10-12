// Timing and gesture logic, with no payload and no radio in it.
//
// Everything here answers a question of the form "has enough time passed" or
// "did the person mean that". It is separated from contract.h because that file
// is about what goes on the wire and this one is about when, and separated from
// the roles because timing logic living in glue is timing logic nobody tests.
// All of it is plain C++ and all of it is exercised by
// firmware/test/test_timing.cpp on the host.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace treksafe {

// When to repeat an unacknowledged distress call.
//
// Kept out of the beacon role because it is timing logic, and timing logic
// sitting in glue is timing logic nobody tests. An off by one here once made
// the first repeat arrive a minute late.
//
// The schedule widens and then holds at its last step. It never gives up: a
// beacon that stops calling before anyone answers leaves a person believing
// nobody heard them, which is the failure this whole product exists to prevent.
// Stopping is the acknowledgement's job, not this one's.
class RepeatSchedule {
  public:
    RepeatSchedule(const uint32_t *stepsMs, size_t count);

    // A new distress call. The next send is immediate.
    void restart();

    // Record that a frame went out at nowMs.
    void sent(uint32_t nowMs);

    // True when the next repeat is due. False until something has been sent.
    bool due(uint32_t nowMs) const;

    // How long the current wait is, for logging and for tests.
    uint32_t waitMs() const { return waitMs_; }

  private:
    const uint32_t *steps_;
    size_t count_;
    size_t step_;
    uint32_t lastMs_;
    uint32_t waitMs_;
    bool sentAnything_;
};

// Has at least intervalMs passed since sinceMs, given the clock reads nowMs.
//
// Every periodic check in every role goes through this rather than writing
// `now - since >= interval` inline, because that expression is wrong in a way
// that does not look wrong. millis() is unsigned, so a `since` a single
// millisecond ahead of `now` underflows the subtraction to about four billion,
// which passes any interval test and fires immediately. That is not
// hypothetical: it is exactly how the beacon came to transmit every distress
// call twice, two milliseconds apart, on the first run against real Meshtastic.
// A stamp taken with a fresh millis() inside a tick sits after the `now` that
// tick is comparing against, and that is the ordinary case, not a rare one.
//
// A signed difference is right for both that and the 49 day millis() wrap, as
// long as intervals stay well under 24 days. The longest here is fifteen
// minutes.
inline bool elapsedAtLeast(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs)
{
    return (int32_t)(nowMs - sinceMs) >= (int32_t)intervalMs;
}

// Press and hold gesture. Kept out of the beacon role so the timing
// can be tested without a board.
//
// A beacon that fires on a light knock sends wardens up a mountain for
// nothing, and a beacon that needs a deliberate gesture is still usable by
// someone cold and frightened as long as the gesture is one motion. Holding is
// that compromise. heldMs lets the caller show the hold building up, so the
// press is learnable and can be abandoned before it fires.
class HoldToFire {
  public:
    explicit HoldToFire(uint32_t holdMs);

    // Feed the debounced button state on every tick. Returns true exactly once,
    // on the tick where the hold completes.
    bool update(bool pressed, uint32_t nowMs);

    // Milliseconds the current hold has been running, 0 if not holding or
    // already fired.
    uint32_t heldMs(uint32_t nowMs) const;

    bool holding() const { return down_ && !fired_; }
    void reset();

  private:
    uint32_t holdMs_;
    bool down_;
    bool fired_;
    uint32_t downAt_;
};


} // namespace treksafe
