// The seam between what this firmware decides and what carries it.
//
// Everything above this line is plain C++ with no hardware in it: the contract,
// the link layer, the three roles. Everything below it is a radio, a cable, or
// a test double. That boundary is the single most load-bearing decision in the
// firmware and it is worth being explicit about why it exists.
//
// It exists for three reasons, in ascending order of importance:
//
//   1. It is how the whole system can be demonstrated with one board. Build the
//      beacon against a transport that writes to the USB cable instead of the
//      radio, and a laptop can play the part of the mesh. Nothing above the
//      transport knows or cares. See sim/ and firmware/README.md.
//
//   2. It is how the roles are tested. A fake transport in a host test can
//      assert what went on the air and hand back what came off it, so the
//      beacon's entire state machine, button to acknowledgement, runs in a
//      compiler with no hardware involved. The firmware this replaced could not
//      do that: its own header admitted that none of the glue could be tested
//      without a board.
//
//   3. It is the escape hatch. The mesh in link.h is a prototype and is
//      documented as one. If it has to be replaced with Meshtastic, with a
//      commercial stack, or with a satellite backhaul, that is a new file
//      implementing this interface, and the roles do not change. A safety
//      system should be able to change its radio without reopening the logic
//      that decides when to shout for help.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace treksafe {

// What the radio reported about a frame it received.
//
// Each reading is flagged rather than using a sentinel value, because "no
// reading" and "a reading of zero" mean opposite things to the coverage survey
// at the base station: zero dB of signal to noise is a real and marginal link,
// and no measurement at all is silence about it. A survey that cannot tell
// those apart is the false confidence D7 exists to prevent.
struct LinkQuality {
    bool hasSnr;
    float snr; // dB
    bool hasRssi;
    int rssi; // dBm

    LinkQuality() : hasSnr(false), snr(0), hasRssi(false), rssi(0) {}
};

// One way to move a frame between this node and whatever is next to it.
//
// Frames are complete link frames, envelope and all, as built by buildLink.
// This interface never looks inside one.
class Transport {
  public:
    virtual ~Transport() {}

    // Puts one frame out. Returns false if it could not go, which the caller
    // must treat as the frame not having been sent: a beacon that believes it
    // transmitted when it did not will stop repeating, and the person will be
    // waiting on an answer that was never asked for.
    virtual bool send(const char *frame) = 0;

    // Collects one waiting frame, if there is one. Returns false when there is
    // nothing, which is the ordinary case on a quiet mesh and never an error.
    //
    // Polled rather than delivered by callback, because a callback from a radio
    // interrupt would run role logic in interrupt context, and role logic
    // allocates nothing and blocks on nothing precisely so it never has to
    // think about that. out must hold at least kLinkFrameMax bytes.
    virtual bool receive(char *out, size_t cap, LinkQuality &quality) = 0;
};

// The wire between a gateway and the computer running the base station.
//
// Deliberately not a Transport. A Transport carries link frames between peers
// on the mesh; this carries a different vocabulary in both directions, because
// the host is told things the mesh has no way to say, above all the signal and
// hop count the gateway measured. Making them one interface would mean one of
// them pretending, and the pretence would land in the coverage survey.
//
// The vocabulary is in role_gateway.h.
class HostPort {
  public:
    virtual ~HostPort() {}

    // Sends one line to the host. The implementation appends the newline.
    virtual void writeLine(const char *line) = 0;

    // Collects one complete line from the host, without its newline. Returns
    // false when no complete line is waiting.
    virtual bool readLine(char *out, size_t cap) = 0;
};

// Everything the hardware knows about this device right now, gathered once per
// tick and handed to the role.
//
// A struct filled by the caller rather than an interface the role calls into,
// so that a role reads a consistent snapshot: a GPS fix that appeared halfway
// through a tick cannot produce a frame that has a position but claims not to.
// It also means a test sets up any device state it likes by assignment.
//
// Nothing here is required to be present. A beacon with no fix, no clock and no
// battery reading still sends a valid distress call, because a distress call
// with nothing in it but an identifier is still a distress call.
struct DeviceState {
    bool buttonPressed;

    bool hasFix;
    double lat;
    double lon;
    int accM; // kUnknownInt when the receiver does not report it in metres

    uint32_t epoch; // kClockNotSet when the clock was never set. Never 1970.

    int battPct;      // kUnknownInt when there is no battery reading
    double battVolts; // meaningful only when battPct is known

    DeviceState()
        : buttonPressed(false), hasFix(false), lat(0), lon(0), accM(-1), epoch(0), battPct(-1),
          battVolts(0)
    {
    }
};

// What the node should be showing, decided by the role and rendered by the
// hardware layer.
//
// The role returns an intention and never touches a pin, which keeps the roles
// testable and means a board with an LED, a board with a screen and a board
// with neither all run the same logic.
//
// On the beacon this is the whole user interface. The person holding it may be
// cold, frightened, or unable to read English, so the hardware layer renders
// these as rhythms rather than words, and never as colour alone.
enum Indication {
    IND_IDLE,          // nothing has happened
    IND_HOLDING,       // the button is down, the hold is building
    IND_AWAITING_ACK,  // the call has gone out, nobody has answered yet
    IND_ACKNOWLEDGED,  // a human at the base station has said they have it
    IND_LISTENING,     // infrastructure: alive and carrying traffic
    IND_SEND_FAILED,   // the transport refused, so nothing went out
};

} // namespace treksafe
