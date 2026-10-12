// The real radio: an SX1262 driven through RadioLib.
//
// This is the only file in the firmware that knows a radio exists. Everything
// above it, the roles, the link layer and the contract, is plain C++ and is
// tested on a development machine. That separation is deliberate and it is what
// makes the rest of this codebase something you can reason about; see the note
// at the top of transport.h.
//
// RadioLib rather than a driver written here, and rather than an SDK tied to
// one chip family, because the same calls drive an SX1262 on an ESP32, an
// nRF52, an RP2040 or an STM32. A solar repeater on a ridge should probably not
// be an ESP32 at all, and being able to move that role to a lower power part
// without rewriting the radio layer is the whole reason the choice was made.
//
// RECEIVE IS POLLED, NOT INTERRUPT DRIVEN.
//
// The SX1262 can raise DIO1 when a packet lands, and this does not use it. The
// reason is that an interrupt handler runs in interrupt context, where the
// rules about what may be called are different and easy to break, and the
// symptom of breaking them is a node that locks up days later in a place nobody
// can reach. Polling costs a few milliseconds of latency on a system whose
// response time is measured in hours of walking. That is not a close trade.

#pragma once

#include "transport.h"

#include <RadioLib.h>

namespace treksafe {

class LoraTransport : public Transport {
  public:
    LoraTransport();

    // Brings the radio up. Returns false if it did not come up, and the caller
    // must treat that as fatal rather than carrying on: a node that believes it
    // has a radio and does not is the failure this whole product exists to
    // prevent, and it is indistinguishable from a quiet mesh from the outside.
    //
    // reason is filled with a short human readable explanation on failure.
    bool begin(char *reason, size_t reasonCap);

    bool send(const char *frame) override;
    bool receive(char *out, size_t cap, LinkQuality &quality) override;

  private:
    // Puts the radio back into receive mode. Called after every transmission,
    // because an SX1262 that has just transmitted is not listening, and a
    // beacon that stops listening after it calls for help will never hear the
    // acknowledgement it is waiting for.
    bool listen();

    SX1262 radio_;
    bool up_;
    bool listening_;
};

} // namespace treksafe
