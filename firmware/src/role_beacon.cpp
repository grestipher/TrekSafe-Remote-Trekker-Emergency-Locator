#include "role_beacon.h"

#include <stdio.h>
#include <string.h>

namespace treksafe {

namespace {
const uint32_t kRepeatSteps[] = TREKSAFE_SOS_REPEAT_MS;
const size_t kRepeatStepCount = sizeof(kRepeatSteps) / sizeof(kRepeatSteps[0]);
} // namespace

BeaconRole::BeaconRole(Transport &transport, const char *devid, uint16_t startSeq)
    : transport_(transport), seq_(startSeq), state_(STATE_IDLE), indication_(IND_IDLE),
      category_(CAT_UNSPECIFIED), hold_(TREKSAFE_HOLD_MS),
      repeats_(kRepeatSteps, kRepeatStepCount), lastSosHadFix_(false), lastCheckInMs_(0),
      sent_(0), failed_(0)
#if TREKSAFE_TRIGGER_AFTER_MS > 0
      ,
      testTriggerFired_(false)
#endif
{
    snprintf(devid_, sizeof(devid_), "%s", devid ? devid : "");
}

void BeaconRole::fillDistress(Distress &d, const DeviceState &s) const
{
    d.devid = devid_;
    d.hasFix = s.hasFix;
    d.lat = s.lat;
    d.lon = s.lon;
    d.accM = s.accM;
    d.epoch = s.epoch;
    d.battPct = s.battPct;
    d.cat = category_;
}

// Wraps a payload in a link envelope and hands it to the transport.
//
// Returns whether it actually went. The caller must not treat a refusal as a
// send: a beacon that believes it transmitted stops repeating on schedule, and
// the person is then waiting for an answer to a question nobody was asked.
bool BeaconRole::transmit(const char *payload)
{
    char frame[kLinkFrameMax];
    // Hops zero: this is the first time this frame has been on the air.
    if (buildLink(frame, sizeof(frame), devid_, seq_, 0, payload) == 0) {
        // Only reachable with an unusable device id, which is a board that
        // could not identify itself. Counted rather than silently swallowed, so
        // the log and the tests can see it.
        failed_++;
        indication_ = IND_SEND_FAILED;
        return false;
    }

    if (!transport_.send(frame)) {
        failed_++;
        indication_ = IND_SEND_FAILED;
        return false;
    }

    // Advanced only on a frame that actually went out, so a sequence number is
    // never burned on a frame nobody heard. A gap in the numbers would not
    // break the deduplication cache, which only ever asks whether it has seen
    // an exact pair before, but it would make the gateway's record of what was
    // transmitted disagree with what was.
    seq_++;
    sent_++;
    return true;
}

void BeaconRole::sendDistress(uint32_t nowMs, const DeviceState &s)
{
    Distress d;
    fillDistress(d, s);

    char payload[kFrameMax];
    if (buildSos(payload, sizeof(payload), d) == 0) {
        failed_++;
        return;
    }

    if (!transmit(payload))
        return;

    // The tick's clock, never a fresh reading. A timestamp taken after this
    // tick began sits in the future relative to the comparisons this tick is
    // making, and unsigned arithmetic turns that into roughly four billion
    // milliseconds elapsed, which fires the next repeat immediately. That is
    // not hypothetical; it is why this beacon once transmitted every distress
    // call twice, two milliseconds apart. See elapsedAtLeast in timing.h.
    repeats_.sent(nowMs);
    lastSosHadFix_ = d.hasFix;
}

void BeaconRole::sendCheckIn(uint32_t nowMs, const DeviceState &s)
{
    Distress d;
    fillDistress(d, s);

    char payload[kFrameMax];
    if (buildOk(payload, sizeof(payload), d) == 0) {
        failed_++;
        return;
    }

    // Recorded whether or not it went out. A check-in is not a distress call:
    // retrying it immediately would spend airtime and battery on a frame that
    // nobody is waiting for, and the next one is due in ten minutes anyway.
    lastCheckInMs_ = nowMs;
    transmit(payload);
}

void BeaconRole::raiseDistress(char category, uint32_t nowMs, const DeviceState &s)
{
    category_ = category;

    // A fresh call restarts the schedule. A second press during a call already
    // in progress does not, or somebody pressing the button repeatedly because
    // nothing visible is happening would hold the beacon at its shortest
    // interval and spend the battery that has to last until help arrives.
    if (state_ == STATE_IDLE || state_ == STATE_ACKNOWLEDGED)
        repeats_.restart();

    state_ = STATE_AWAITING_ACK;
    sendDistress(nowMs, s);
}

// Watches for an acknowledgement addressed to this beacon.
//
// Everything else on the mesh is somebody else's traffic and is dropped. The
// beacon does not relay: see the note at the top of role_beacon.h.
void BeaconRole::pollForAck(uint32_t nowMs)
{
    char line[kLinkFrameMax];
    LinkQuality quality;

    // Drained rather than taking one per tick, or a burst of mesh traffic would
    // queue ahead of our acknowledgement and delay it by a tick per frame.
    while (transport_.receive(line, sizeof(line), quality)) {
        LinkFrame f;
        if (!parseLink(line, f))
            continue;

        Ack ack;
        if (!parseAck(f.payload, devid_, ack))
            continue;

        if (state_ != STATE_AWAITING_ACK)
            continue; // a duplicate arriving after we already stopped

        state_ = STATE_ACKNOWLEDGED;
        // The first check-in is due a full interval from here, not immediately.
        lastCheckInMs_ = nowMs;
    }
}

void BeaconRole::updateIndication()
{
    // A failed send is left showing until something else happens, so a board
    // that cannot transmit does not sit looking like a board that is waiting.
    if (indication_ == IND_SEND_FAILED)
        return;

    if (hold_.holding()) {
        indication_ = IND_HOLDING;
        return;
    }
    switch (state_) {
    case STATE_AWAITING_ACK:
        indication_ = IND_AWAITING_ACK;
        break;
    case STATE_ACKNOWLEDGED:
        indication_ = IND_ACKNOWLEDGED;
        break;
    default:
        indication_ = IND_IDLE;
        break;
    }
}

void BeaconRole::tick(uint32_t nowMs, const DeviceState &s)
{
    // Acknowledgements first. A reply that arrived during the last interval
    // should stop this tick's repeat rather than racing it, which saves one
    // unnecessary transmission on every successful rescue.
    pollForAck(nowMs);

    // The gesture. Fires exactly once, on the tick the hold completes.
    if (hold_.update(s.buttonPressed, nowMs))
        raiseDistress(CAT_UNSPECIFIED, nowMs, s);

#if TREKSAFE_TRIGGER_AFTER_MS > 0
    // Simulator and bench only. A build with this set is not a beacon.
    if (!testTriggerFired_ && elapsedAtLeast(nowMs, 0, (uint32_t)TREKSAFE_TRIGGER_AFTER_MS)) {
        testTriggerFired_ = true;
        raiseDistress(CAT_INJURY, nowMs, s);
    }
#endif

    if (state_ == STATE_AWAITING_ACK) {
        // A lock that arrives after the first call goes out immediately rather
        // than waiting for the next repeat. The base station is holding an
        // incident marked NO FIX and somebody is deciding where to send people.
        if (!lastSosHadFix_ && s.hasFix) {
            sendDistress(nowMs, s);
        } else if (repeats_.due(nowMs)) {
            sendDistress(nowMs, s);
        }
    } else if (state_ == STATE_ACKNOWLEDGED) {
        if (elapsedAtLeast(nowMs, lastCheckInMs_, TREKSAFE_CHECKIN_MS))
            sendCheckIn(nowMs, s);
    }

    updateIndication();
}

} // namespace treksafe
