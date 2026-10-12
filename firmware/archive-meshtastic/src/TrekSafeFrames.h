// TrekSafe payload contract v1, frame construction and acknowledgement parsing.
//
// This file deliberately includes nothing from Meshtastic or Arduino. It is
// plain C++ so it can be compiled and tested on a development machine, which
// matters because this is the one piece of firmware where a bug means a real
// distress call is malformed or an acknowledgement is missed. See
// firmware/test/test_frames.cpp, which runs without a board.
//
// Contract: docs/payload-contract.md, version 1, ratified August 2026.
//
//   SOS|1|<devid>|<lat>|<lon>|<acc_m>|<epoch>|<cat>|<batt>
//   OK |1|<devid>|<lat>|<lon>|<acc_m>|<epoch>
//   TEL|1|<devid>|<batt>|<volts>|<snr>
//   ACK|1|<devid>|<incident_id>        base station to beacon

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace treksafe {

// Schema version. Field 2 of every frame. See the contract on why it sits
// this early: an unknown version can be spotted before anything after it is
// trusted.
static const int kSchema = 1;

// Longest frame this contract can produce, plus room for a terminator. An SOS
// is roughly 45 bytes, which is what keeps airtime low on Long Fast.
static const size_t kFrameMax = 96;

// A device id is 2 to 8 uppercase hex characters. Four is what the beacon
// uses, taken from the low half of the Meshtastic node number.
static const size_t kDevIdMax = 9;

// Sentinels for values the beacon does not have. Each one is emitted as an
// empty field, which the console already reads as unknown.
static const int kUnknownInt = -1;
static const uint32_t kClockNotSet = 0;

enum Category {
    CAT_MEDICAL = 'M',
    CAT_INJURY = 'I',
    CAT_LOST = 'L',
    CAT_UNSPECIFIED = 'U',
};

// One distress reading. Nothing here is required to be present. A beacon that
// has no GPS lock, no clock and no battery reading still sends a valid frame,
// because a distress call with nothing but an identifier is still a distress
// call.
struct Distress {
    const char *devid;   // null or empty means the beacon could not identify itself
    bool hasFix;         // false emits 0|0, which the contract defines as no fix yet
    double lat;
    double lon;
    int accM;            // kUnknownInt emits an empty field
    uint32_t epoch;      // kClockNotSet emits an empty field
    char cat;            // anything unrecognised is emitted as U
    int battPct;         // kUnknownInt emits an empty field
};

// A parsed acknowledgement from the base station.
struct Ack {
    char devid[kDevIdMax];
    char incident[24];
    bool hasIncident;    // an acknowledgement without an incident id is still valid
};

// Derives the four character device id from a Meshtastic node number. out must
// hold at least kDevIdMax bytes.
void deviceIdFromNodeNum(uint32_t nodeNum, char *out);

// True if s is 2 to 8 uppercase hex characters, which is what the contract
// requires of a device id.
bool isValidDevId(const char *s);

// Frame builders. Each writes a null terminated frame into out and returns its
// length, or 0 if out is too small. None of them can fail on bad input: a
// value that cannot be represented is emitted as an empty field rather than
// stopping the frame from being sent.
size_t buildSos(char *out, size_t cap, const Distress &d);
size_t buildOk(char *out, size_t cap, const Distress &d);
size_t buildTel(char *out, size_t cap, const char *devid, int battPct, double volts, double snr);

// Parses an acknowledgement addressed to ourDevId. Returns false if the line
// is not an acknowledgement for us.
//
// This parser is deliberately more forgiving than the console's. A malformed
// acknowledgement that is thrown away leaves a person believing nobody heard
// them, so anything that plainly says ACK, speaks our schema and names our
// device is accepted, including one with the incident id missing.
bool parseAck(const char *line, const char *ourDevId, Ack &out);

// When to repeat an unacknowledged distress call.
//
// Kept here rather than in the module because it is timing logic, and timing
// logic in glue is timing logic nobody tests. An off by one here once made the
// first repeat arrive a minute late.
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
// Every periodic check in the module goes through this rather than writing
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

// Press and hold gesture. Kept here rather than in the module so the timing
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
