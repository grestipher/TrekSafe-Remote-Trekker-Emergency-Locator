// A transport that carries frames over the USB cable instead of the radio.
//
// This is what makes the whole system demonstrable with one board, and it is
// not a trick: the USB cable is standing in for the air, and everything above
// it, the roles and the link layer and the contract, behaves exactly as it
// would with a radio underneath. A laptop on the other end plays the part of
// the mesh, running simulated repeaters and a simulated gateway. See sim/.
//
// The line format is the frame with an AIR prefix:
//
//   AIR TS1|A4F2|41|0|SOS|1|A4F2|52.8734|-118.0814|12|1755000000|I|86
//
// Prefixed so that log output on the same port cannot be mistaken for a frame,
// and so a person watching in a serial terminal can see which lines are traffic
// and which are the firmware talking about itself. Anything that is not a
// prefixed line is ignored in both directions.
//
// It reports no signal readings, and that is the honest answer rather than a
// missing feature: a cable has no signal to measure. The simulator on the
// laptop supplies plausible ones when it plays the part of a radio, and it
// labels them as simulated all the way through to the console, so nothing that
// came from a cable is ever presented to a warden as a measurement.

#pragma once

#include "link.h" // for kLinkFrameMax: the buffer has to hold a whole frame
#include "transport.h"

#include <Arduino.h>

namespace treksafe {

class SerialTransport : public Transport {
  public:
    // stream is the port to use. Kept as a parameter rather than hard-wired to
    // Serial so a board with a spare UART can put traffic on one port and human
    // readable logging on another.
    explicit SerialTransport(Stream &stream);

    bool send(const char *frame) override;
    bool receive(char *out, size_t cap, LinkQuality &quality) override;

  private:
    Stream &stream_;

    // One line being assembled as bytes arrive. Serial delivers a few
    // characters at a time, so a frame is built up across many calls.
    char buffer_[kLinkFrameMax + 8];
    size_t len_;

    // Set when a line arrived that did not fit. The rest of that line is
    // discarded up to its newline, so a single oversized line cannot glue
    // itself onto the front of the next real frame.
    bool overflowed_;
};

} // namespace treksafe
