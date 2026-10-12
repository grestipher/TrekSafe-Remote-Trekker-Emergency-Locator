# Payload contract

Version 1. Ratified August 2026, before the firmware was written.

The frames below are fixed. Nothing about the wire format changed at
ratification. Two things were clarified that do not alter a single byte, and
they are marked as clarifications in the field tables. Anything else changes in
this document, the console parser and the firmware together, in one commit.

## Why pipe delimited text

Protobuf is the right answer for production. Text is the right answer now,
because it can be read in any serial terminal and typed by hand, which is what
made the contract testable long before there was firmware to produce it. Moving
to protobuf later is a version bump, which the schema field already allows for.

The reason originally given here was that a frame could be typed into the
Meshtastic phone app. That stopped being true with D10 and the argument survived
it intact, which is a reasonable sign the format was chosen for the right
reasons.

## Frames

### Distress, beacon to base station

```
SOS|1|<devid>|<lat>|<lon>|<acc_m>|<epoch>|<cat>|<batt>
SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M|87
```

| Field | Meaning | Notes |
| --- | --- | --- |
| 0 | frame type | `SOS` |
| 1 | schema version | integer, currently `1` |
| 2 | device id | 2 to 8 hex characters, uppercase |
| 3 | latitude | decimal degrees, `0` or empty means no fix |
| 4 | longitude | decimal degrees, `0` or empty means no fix |
| 5 | position accuracy | metres, empty means unknown |
| 6 | timestamp | unix seconds, sender clock, `0` or empty means the clock is not set |
| 7 | category | `M` medical, `I` injury, `L` lost, `U` unspecified |
| 8 | battery | percent, empty means unknown |

**Clarification, ratification.** An empty numeric field means the beacon does
not have that value, and a reader must show it as unknown rather than
substituting one. This is not new syntax. The console already reads an empty
field this way and the firmware already emits one, but it was never written
down, and the difference between an unknown battery level and a battery level
of zero matters to whoever is deciding how long someone can wait.

**Clarification, ratification.** A timestamp of `0` or empty means the sending
device has no valid clock, which is the normal case for a beacon that has never
had a GPS lock. It does not mean 1970. A reader must not present a substituted
local time as the time the beacon sent. The console showed a substituted time
when this was written, which was issue I8, and no longer does: it reports the
send time as not reported and keeps first heard separate.

**Clarification, ratification.** The device id is four uppercase hex characters,
derived from the low sixteen bits of the board's hardware serial number. The
contract still permits 2 to 8, so nothing else needs to change if that ever
moves. The mesh carries this and never a name. See I9.

It was the Meshtastic node number until D10 and is now the ESP32's factory
programmed MAC address, which changed nothing on the wire. Derived rather than
provisioned so that it survives a reflash and needs no step at the trailhead
that somebody would eventually get wrong by issuing two beacons the same id.
The cost is a roughly one in 65536 chance of two boards in a fleet colliding,
caught at the base station, which is the only place that knows the fleet.

### Check-in, beacon to base station

```
OK|1|<devid>|<lat>|<lon>|<acc_m>|<epoch>
```

Non-emergency. Does not raise an alarm. Updates node health and last known
position.

### Telemetry, node to base station

```
TEL|1|<devid>|<batt>|<volts>|<snr>
```

From fixed repeater nodes. Feeds the node health panel so a dead repeater is known
before it matters rather than during an incident.

### Acknowledgement, base station to beacon

```
ACK|1|<devid>|<incident_id>
```

Sent only after a human presses acknowledge. This is what lets a frightened person
believe the advice to stay in one place.

## The link layer, version 1

Added September 2026 with decision D10, when TrekSafe stopped being a Meshtastic
module. Until then the mesh was somebody else's and this document only had to
describe what a frame said. Now it has to describe how one travels as well.

The two are deliberately separate. A payload is what a distress call says; the
envelope is how it gets there. They version independently, and the payload
above did not change by a single byte when the entire radio stack underneath it
was replaced, which is the clearest evidence the line is drawn in the right
place.

```
TS1|<origin>|<seq>|<hops>|<payload>
TS1|A4F2|41|1|SOS|1|A4F2|52.8734|-118.0814|12|1755000000|I|86
```

| Field | Meaning | Notes |
| --- | --- | --- |
| 0 | link version | `TS1`. A node that does not recognise it drops the frame rather than guessing at the fields behind it |
| 1 | origin | device id of the node that first put this on the air, 2 to 8 hex characters |
| 2 | sequence | 0 to 65535, from that origin, wraps. Starts at an unpredictable value at boot |
| 3 | hops | how many times this frame has been relayed. 0 means heard directly |
| 4+ | payload | a contract frame, verbatim, pipes and all |

**The payload is everything after the fourth pipe.** It is itself pipe
delimited and is never split by this layer. A link layer that reads a payload is
a second interpreter of a distress call, and two interpreters drifting apart is
how one call gets read two ways by two halves of one system.

**Hops count up rather than a time to live counting down.** A TTL tells a
receiver how much further a frame may go but not how far it has come, and how
far it has come is the number the coverage survey is built from. Counting up
gives both: the field answers "how many relays did this take", and "stop
relaying at `kMaxHops`" is the same rule a TTL would have enforced. One field,
two jobs, and no constant the sender and receiver have to agree on.

**Deduplication is on origin and sequence together.** Every repeater and the
gateway remember which messages they have already handled. Without it two
repeaters in range of each other rebroadcast each other's rebroadcasts and the
mesh saturates on one distress call. It is the only part of a flood mesh that is
not optional.

**Each repeat from a beacon is a new sequence number.** The beacon repeats an
unacknowledged call on a widening schedule, and those repeats must reach the
base station rather than being deduplicated away. A repeat is a new message
carrying the same payload, not the same message sent twice.

**Memory of seen messages is bounded and time limited.** Bounded because a
repeater runs all season on a fixed amount of memory. Time limited because a
beacon that reboots starts its sequence numbers again, and a repeater that
remembered the old ones forever would silently drop the first frames of a
genuine new distress call. Starting the sequence somewhere unpredictable at boot
makes that collision unlikely; forgetting bounds it when it happens. Both,
because either alone leaves a case where a real call disappears with nothing
logged anywhere.

**A frame with an unreadable origin is never sent and never relayed.** It cannot
be deduplicated, and a frame that cannot be deduplicated is one a flood mesh
rebroadcasts forever. That is the single failure that takes the whole mesh down
rather than one message.

**What this layer does not do.** No routing table, no neighbour discovery, no
acknowledgement of its own, no encryption, no duty cycle management. Those are
real omissions, they are written down in D10, and a distress frame travels in
clear text. Do not read this section and conclude the radio is solved.

The implementation is `firmware/src/link.{h,cpp}`, host tested. The simulated
mesh in `sim/mesh.py` implements the same rules and is tested against the same
cases, because a simulator more generous than the real thing is worse than no
simulator at all.

## Design notes

**Schema version is field 2, not field 8.** A parser can reject an unknown version
after reading eleven bytes, without trusting anything that follows.

**No fix is valid.** `0|0` or empty coordinates is a real distress message from a
beacon that has not acquired a lock, which happens under heavy canopy. The console
alarms, marks the incident `NO FIX`, and expects a position to follow. Treating it
as malformed would discard a genuine call for help.

**Size.** An SOS frame is roughly 45 bytes. Airtime matters on Long Fast, and the
distress path must not compete with itself.

## Open questions

- Move to protobuf on a private port number, and when
- Whether the beacon should repeat the SOS on an interval until acknowledged, and
  what that interval costs in airtime. The firmware currently repeats at 30s,
  60s, 120s, then every 5 minutes, stopping only on acknowledgement. That is a
  proposal sitting in one constant in `firmware/src/TrekSafeModule.h`, not a
  decision. The console correlates the repeats into one incident, so the cost of
  being wrong here is airtime rather than a cluttered screen.
- Whether hop count and SNR should be carried in the payload or read from the
  packet layer
- Whether a position that arrives after a `NO FIX` distress call needs a frame
  of its own. The firmware sends a fresh `SOS`, which is the safe behaviour, and
  the console lands that position on the existing incident rather than creating
  a second one. It works, but it works by correlation rather than by anything
  the contract says, which is worth closing.
- Whether a beacon can cancel a false alarm. There is no frame for it. Somebody
  will press the button by accident, and at present the only way to stand a
  response down is by voice, which a person in a valley does not have. Adding a
  cancel frame also raises whether a cancel can be trusted, so this is a product
  decision before it is a protocol one.
