// The repeater: a solar box bolted to a ridge that nobody visits.
//
// It does two things. It rebroadcasts anything it has not heard before, which
// is the entire mesh. And it reports its own health on a schedule, so that a
// repeater which is dying is known about while somebody can still walk up there
// in daylight, rather than discovered during an incident.
//
// The second job is not secondary. A flood mesh with a dead relay in it does
// not announce that it has a hole; it simply stops carrying frames through a
// valley, silently, which is precisely the false confidence D7 exists to
// prevent. The telemetry is what turns that into something visible.
//
// Like the beacon, this contains no hardware and is driven by a Transport and a
// per-tick snapshot, so its relaying and deduplication behaviour is exercised
// in a host test rather than on a ridge.

#pragma once

#include "config.h"
#include "contract.h"
#include "link.h"
#include "timing.h"
#include "transport.h"

namespace treksafe {

class RepeaterRole {
  public:
    // jitterSeed seeds the small random delay before a rebroadcast. Every
    // repeater that hears a frame decides to relay it at the same instant, so
    // without a spread they transmit together and collide, and the frame that
    // reaches nobody is a distress call. Two repeaters seeded identically would
    // pick the same delays and collide every time, so this must differ per
    // board; the hardware layer derives it from the device id.
    RepeaterRole(Transport &transport, const char *devid, uint16_t startSeq, uint32_t jitterSeed);

    void tick(uint32_t nowMs, const DeviceState &state);

    Indication indication() const { return indication_; }
    const char *deviceId() const { return devid_; }

    // Counters, for the log and for tests.
    uint32_t relayedCount() const { return relayed_; }
    uint32_t droppedCount() const { return dropped_; } // already seen, or out of hops
    uint32_t failedCount() const { return failed_; }

  private:
    void receiveAndQueue(uint32_t nowMs);
    void sendPending(uint32_t nowMs);
    void sendTelemetry(uint32_t nowMs, const DeviceState &state);
    uint32_t nextJitter();

    Transport &transport_;
    char devid_[kDevIdMax];
    uint16_t seq_;

    SeenCache seen_;
    Indication indication_;

    // One frame waiting out its collision-avoidance delay.
    //
    // Exactly one, not a queue. A repeater that had fallen far enough behind to
    // need a queue would be transmitting a backlog of stale frames into a mesh
    // that is already too busy, which makes the congestion worse at the moment
    // it matters most. Holding one and counting the rest as dropped is the
    // honest behaviour, and the counter is what makes it visible.
    bool hasPending_;
    char pending_[kLinkFrameMax];
    uint32_t pendingDueMs_;

    uint32_t rng_;
    uint32_t lastTelemetryMs_;
    bool sentTelemetry_; // never sent is not the same as sent at millis() zero

    uint32_t relayed_;
    uint32_t dropped_;
    uint32_t failed_;
};

} // namespace treksafe
