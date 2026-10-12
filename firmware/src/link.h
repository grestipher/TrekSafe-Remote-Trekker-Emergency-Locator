// TrekSafe link layer, version 1.
//
// This is the envelope a payload travels in, and it is the whole of the mesh.
// It answers two questions and deliberately nothing else: who first put this on
// the air, and how many times has it been relayed. Everything a flood mesh
// actually needs falls out of those two.
//
//     TS1|<origin>|<seq>|<hops>|<payload>
//     TS1|A4F2|41|1|SOS|1|A4F2|52.8734|-118.0814|12|1755000000|I|86
//
// The payload is a contract frame, verbatim, pipes and all. The link layer
// never looks inside it. That separation is the same one the base station keeps
// between the bridge and the console, and for the same reason: one thing should
// understand what a distress call means, and it should not be the thing moving
// bytes.
//
// WHAT THIS IS NOT
//
// It is a flood mesh, which is the simplest thing that works: every repeater
// rebroadcasts anything it has not heard before, until the hop count runs out.
// There is no routing table, no neighbour discovery, no acknowledgement at this
// layer, no encryption and no duty cycle management. Those are real omissions
// and they are written down in docs/decisions.md under D10. Do not let anyone
// read this file and come away thinking the radio is solved.
//
// WHY HOPS COUNT UP RATHER THAN A TTL COUNTING DOWN
//
// A time to live that counts down tells a receiver how much further a packet
// may go, but not how far it has come, and how far it has come is the number
// the coverage survey is built from. Counting up gives both: the number in the
// field is the answer to "how many relays did this take", and "drop it once it
// reaches kMaxHops" is the same rule a TTL would have enforced. One field, two
// jobs, and no constant that the sender and the receiver have to agree on.

#pragma once

#include "contract.h"

#include <stddef.h>
#include <stdint.h>

namespace treksafe {

// Link format version, field 0. Separate from the payload's schema version on
// purpose: how a frame is routed and what a frame says are allowed to change
// independently, and a node that speaks a link version it does not recognise
// must drop the frame rather than guess at the fields behind it.
static const char *const kLinkTag = "TS1";

// How many relays a frame may take before it stops being rebroadcast.
//
// Three is a judgement, not a measurement. Each extra hop multiplies the
// airtime one distress call costs the whole mesh, and in a flood mesh that cost
// is paid by every node in range of every relay. Three covers a valley with two
// repeaters between a beacon and the gateway, which is the deployment shape
// this is being designed for. Raise it only with a coverage survey in hand.
static const uint8_t kMaxHops = 3;

// Longest complete on-air frame: the payload plus the envelope around it.
// "TS1|" + 8 origin + "|" + 5 seq + "|" + 1 hops + "|" is 20 characters, and
// this rounds up generously rather than sitting exactly on the boundary.
static const size_t kLinkFrameMax = kFrameMax + 32;

// One frame, taken apart. payload points into the buffer that was parsed, so it
// lives exactly as long as that buffer does and never longer.
struct LinkFrame {
    char origin[kDevIdMax];
    uint16_t seq;
    uint8_t hops;
    const char *payload;
};

// Wraps a payload for transmission. Returns the length written, or 0 if out is
// too small, in which case out is left as an empty string.
//
// Refuses an unusable origin, because a frame nobody can attribute cannot be
// deduplicated and would be rebroadcast by every repeater that hears it, every
// time it hears it. That is the one failure in a flood mesh that takes the
// whole mesh down rather than one message, so it is worth refusing to send.
size_t buildLink(char *out, size_t cap, const char *origin, uint16_t seq, uint8_t hops,
                 const char *payload);

// Takes a received frame apart. Returns false if it is not a link frame this
// version understands.
//
// Splits in place, so line is modified and out.payload points into it. line
// must be writable and must outlive out.
bool parseLink(char *line, LinkFrame &out);

// How many messages a node remembers having seen. Each entry is about sixteen
// bytes, so the default costs a kilobyte, which is nothing on an ESP32 and
// still worth stating for a repeater that might one day be something smaller.
#ifndef TREKSAFE_SEEN_CAPACITY
#define TREKSAFE_SEEN_CAPACITY 64
#endif

// Remembers which messages have already been handled, so a flood mesh floods
// once rather than forever.
//
// Without this, two repeaters in range of each other rebroadcast each other's
// rebroadcasts and the mesh saturates on a single distress call. It is the one
// piece of a flood mesh that is not optional.
//
// Entries are forgotten after a while, which matters more than it looks. A
// beacon that reboots starts its sequence numbers again, and a repeater that
// remembered the old ones forever would silently drop the first several frames
// of a genuine new distress call, which is the worst failure this system can
// have. Forgetting bounds that to the window. Starting the sequence somewhere
// unpredictable at boot, which is what the beacon role does, makes the
// collision unlikely in the first place. Both, because either alone leaves a
// case where a real call disappears with nothing logged anywhere.
class SeenCache {
  public:
    // forgetAfterMs of zero means never forget, which is only useful in tests.
    explicit SeenCache(uint32_t forgetAfterMs = 15u * 60u * 1000u);

    // True the first time this message is offered, false for a repeat of one
    // already remembered. Records it either way.
    //
    // A frame with no usable origin is always new, because there is no key to
    // remember it under. Callers must not rebroadcast such a frame; see
    // buildLink on why it should never have been sent.
    bool isNew(const char *origin, uint16_t seq, uint32_t nowMs);

    // Deliberately exposed for tests, which need to see that the cache stays
    // bounded rather than trusting that it does.
    size_t remembered() const;

  private:
    struct Entry {
        char origin[kDevIdMax];
        uint16_t seq;
        uint32_t at;
        bool used;
    };

    Entry entries_[TREKSAFE_SEEN_CAPACITY];
    size_t next_; // oldest slot, overwritten when the ring is full
    uint32_t forgetAfterMs_;
};

} // namespace treksafe
