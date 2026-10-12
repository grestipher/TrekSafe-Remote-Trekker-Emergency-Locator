#include "role_repeater.h"

#include <stdio.h>
#include <string.h>

namespace treksafe {

RepeaterRole::RepeaterRole(Transport &transport, const char *devid, uint16_t startSeq,
                           uint32_t jitterSeed)
    : transport_(transport), seq_(startSeq), indication_(IND_LISTENING), hasPending_(false),
      pendingDueMs_(0), rng_(jitterSeed ? jitterSeed : 1u), lastTelemetryMs_(0),
      sentTelemetry_(false), relayed_(0), dropped_(0), failed_(0)
{
    snprintf(devid_, sizeof(devid_), "%s", devid ? devid : "");
    pending_[0] = '\0';
}

// A small deterministic generator, seeded per board.
//
// Deliberately not the C library's rand(): this has to be reproducible in a
// test, independent of any other code that might seed the global one, and
// available on a target whose libc may not have it. The constants are the usual
// 32 bit xorshift, which is far better than good enough for spreading a
// hundred millisecond delay.
uint32_t RepeaterRole::nextJitter()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_ % (uint32_t)TREKSAFE_RELAY_JITTER_MS;
}

// Takes everything waiting off the radio and decides what to do with it.
void RepeaterRole::receiveAndQueue(uint32_t nowMs)
{
    char line[kLinkFrameMax];
    LinkQuality quality;

    while (transport_.receive(line, sizeof(line), quality)) {
        LinkFrame f;
        if (!parseLink(line, f)) {
            // Not ours, or corrupted beyond reading. Nothing to relay and
            // nothing to say: the base station is where a malformed distress
            // call gets reported, and this node cannot tell the difference
            // between a corrupted frame and somebody else's radio.
            dropped_++;
            continue;
        }

        // Our own frame coming back from another repeater. Dropped before the
        // deduplication cache sees it, so that a repeater's own telemetry
        // cannot evict entries it needs to suppress a loop.
        if (strcmp(f.origin, devid_) == 0) {
            dropped_++;
            continue;
        }

        if (!seen_.isNew(f.origin, f.seq, nowMs)) {
            // Already relayed. This is the ordinary case in a working mesh and
            // it is what stops two repeaters in range of each other
            // rebroadcasting each other forever.
            dropped_++;
            continue;
        }

        if (f.hops >= kMaxHops) {
            // It has come as far as it is allowed to. Recorded as seen above,
            // deliberately: if it arrives again by a shorter path we still do
            // not want to relay it, and the alternative is a frame that has
            // exhausted its hops being reconsidered on every copy.
            dropped_++;
            continue;
        }

        if (hasPending_) {
            // See role_repeater.h on why there is no queue.
            dropped_++;
            continue;
        }

        // Rebuilt rather than edited in place. The hop count is a different
        // width once it crosses from 9 to 10, so incrementing it inside the
        // original string would need the rest of the frame shifted, and a frame
        // shifted wrongly is a distress call corrupted by the thing that was
        // supposed to carry it.
        if (buildLink(pending_, sizeof(pending_), f.origin, f.seq, (uint8_t)(f.hops + 1),
                      f.payload) == 0) {
            failed_++;
            continue;
        }

        hasPending_ = true;
        pendingDueMs_ = nowMs + nextJitter();
    }
}

void RepeaterRole::sendPending(uint32_t nowMs)
{
    if (!hasPending_)
        return;
    // Signed comparison, so a due time computed just ahead of this tick's clock
    // does not underflow into "four billion milliseconds overdue". See
    // elapsedAtLeast in timing.h.
    if (!elapsedAtLeast(nowMs, pendingDueMs_, 0))
        return;

    if (transport_.send(pending_))
        relayed_++;
    else
        failed_++;

    hasPending_ = false;
    pending_[0] = '\0';
}

void RepeaterRole::sendTelemetry(uint32_t nowMs, const DeviceState &s)
{
    char payload[kFrameMax];
    // SNR is reported as zero: this node has no reading about itself, and the
    // number that matters for coverage is what a receiver measured, which the
    // gateway supplies from the packet layer. Left in the frame because the
    // contract has the field and changing that is a schema change.
    if (buildTel(payload, sizeof(payload), devid_, s.battPct, s.battVolts, 0.0) == 0) {
        failed_++;
        return;
    }

    char frame[kLinkFrameMax];
    if (buildLink(frame, sizeof(frame), devid_, seq_, 0, payload) == 0) {
        failed_++;
        return;
    }

    // Recorded on the attempt, not the success. Retrying a health report at
    // tick speed would flood the mesh with the one kind of frame nobody is
    // waiting for, and the next one is due in fifteen minutes anyway.
    lastTelemetryMs_ = nowMs;
    sentTelemetry_ = true;

    if (transport_.send(frame))
        seq_++;
    else
        failed_++;
}

void RepeaterRole::tick(uint32_t nowMs, const DeviceState &s)
{
    receiveAndQueue(nowMs);

    // Relaying comes before telemetry on purpose. If both are ready in the same
    // tick, somebody else's distress call goes out ahead of this node's report
    // about its own battery.
    sendPending(nowMs);

    if (!sentTelemetry_ || elapsedAtLeast(nowMs, lastTelemetryMs_, TREKSAFE_TELEMETRY_MS))
        sendTelemetry(nowMs, s);

    indication_ = (failed_ > 0 && relayed_ == 0) ? IND_SEND_FAILED : IND_LISTENING;
}

} // namespace treksafe
