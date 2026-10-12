// The gateway: the radio sitting next to the computer at the staffed facility.
//
// It is the only node with a wire attached to it, and its job is to be a
// translator and nothing more. Anything it hears on the mesh goes up the cable
// with what the radio measured about it; anything the base station wants to say
// goes out over the mesh. It never interprets a payload, never raises an alarm
// and never decides anything about an incident. The console does all of that,
// because there must be exactly one thing that understands what a distress call
// means, and putting a second one here is how the two halves of a system start
// disagreeing about the same call.
//
// THE HOST PROTOCOL
//
// Lines of text, because a person debugging this at two in the morning should
// be able to read them in any serial terminal.
//
//   gateway to host
//     RX <snr> <rssi> <hops> <payload>   one frame heard on the mesh
//     BEAT <uptime_ms>                   still alive, sent whether or not the
//                                        mesh has said anything
//     LOG <text>                         a human readable note
//
//   host to gateway
//     TX <payload>                       put this payload on the mesh
//
// Space delimited with the payload last, so the payload's own pipes need no
// escaping and cannot shift a field. A missing signal reading is written as a
// single "-" rather than as zero, because zero dB is a real and marginal
// measurement and silence about it is not; the coverage survey at the base
// station is built from these and a survey that confuses the two is the false
// confidence D7 exists to prevent.
//
// BEAT is the load-bearing one and the easiest to think is decoration. It is
// what becomes the console's gateway heartbeat, and the distinction it carries
// is that a quiet mesh at three in the morning is normal while a gateway that
// has stopped listening is not. Only a signal independent of traffic can tell
// those apart. Without it the console cannot know the difference between a calm
// night and a dead radio, and it would show a calm night.

#pragma once

#include "config.h"
#include "contract.h"
#include "link.h"
#include "timing.h"
#include "transport.h"

namespace treksafe {

class GatewayRole {
  public:
    GatewayRole(Transport &radio, HostPort &host, const char *devid, uint16_t startSeq);

    void tick(uint32_t nowMs, const DeviceState &state);

    Indication indication() const { return indication_; }
    const char *deviceId() const { return devid_; }

    uint32_t heardCount() const { return heard_; }
    uint32_t sentCount() const { return sent_; }
    uint32_t droppedCount() const { return dropped_; }
    uint32_t failedCount() const { return failed_; }

  private:
    void pollRadio(uint32_t nowMs);
    void pollHost(uint32_t nowMs);
    void beat(uint32_t nowMs);

    Transport &radio_;
    HostPort &host_;
    char devid_[kDevIdMax];
    uint16_t seq_;

    // The gateway deduplicates too. Without it the console would see one
    // distress call once per path it took through the mesh and count the copies
    // as repeats, which reads on screen as a beacon calling again because
    // nobody answered.
    SeenCache seen_;

    Indication indication_;
    uint32_t lastBeatMs_;
    bool beatenOnce_;

    uint32_t heard_;
    uint32_t sent_;
    uint32_t dropped_;
    uint32_t failed_;
};

} // namespace treksafe
