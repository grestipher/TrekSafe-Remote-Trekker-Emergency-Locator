# Decision log

Short records of what was decided and why, so the reasoning survives.

## D1. Build on Meshtastic as a custom module, not a hard fork

**Decided.** Alternatives were a hard fork of the firmware, or a bespoke protocol
on the SX1262.

A module keeps the project able to pull upstream fixes and releases. Meshtastic
ships often, and over a multi-month project a permanent divergence is expensive.
A bespoke protocol would discard the mesh routing, acknowledgement handling and
years of field testing that come free.

Revisit only if the SOS path genuinely cannot get the routing priority it needs
within a module.

## D2. Base station console before firmware

**Decided.** The console is what a Parks Canada evaluator experiences. The radio
work is invisible to them. It is also the part that can be built and demonstrated
with a single board.

## D3. Acknowledgement is a core feature

**Decided.** A beacon pressed into silence leads people to conclude it did not work
and start moving, which is the specific behaviour that turns a short rescue into a
multi-day search. Standing advice after such incidents is to carry a device and
stay in one place, and an acknowledgement is what makes the second half of that
advice credible.

## D4. Acknowledgement is manual, never automatic

**Decided.** A human confirms they have seen the incident before the beacon is told
help is coming. Automatic acknowledgement would tell someone they had been heard
when nobody had looked.

## D5. Bridge transport

**Decided, August 2026.** A separate bridge process using the Meshtastic Python
library, talking to the console over a local HTTP connection.

The two options originally recorded were serial with the library imported into
the console, or MQTT. The first makes the console a GPL-3.0 derivative on
distribution. The second avoids that but pays for it: JSON over MQTT is
unencrypted, deprecated upstream, unsupported on nRF52, and covers only a fixed
set of port numbers, which are real costs for a safety system carrying the
location of a person in trouble.

The resolution is that the licensing problem comes from linking, not from using.
Two programs at arm's length over a socket are not one derivative work, which is
the ordinary reading and the ordinary practice. So the bridge imports the GPL
library and the console never does. `bridge/` is the only GPL-facing part of the
system and the console stays independently licensable.

Transport to the console is Server-Sent Events one way and an HTTP POST the
other. That needs no WebSocket library and no dependency at all, and it works
with a console opened straight off the disk. MQTT stays available later as
another source inside the bridge if a deployment wants it, without touching the
console.

This also unblocks D6, since a process that outlives the browser is where
persistence can live.

## D6. Persistence

**Decided, August 2026.** SQLite in the bridge, holding frames and operator
actions. Not incidents.

SQLite because it is in the Python standard library, so it adds no dependency to
a project that has none; because it is one file that can be copied to back it up;
and because it is crash safe under write-ahead logging. A base station is one
machine at a staffed facility with no guaranteed internet, which is the case
SQLite is best at and a database server is worst at.

The part worth arguing about is what gets stored. Storing incidents would mean
the bridge has to understand a frame, which means a second parser alongside the
console's. Two parsers drifting apart is how the same distress call ends up read
two ways by two halves of one system, and the parser is already the piece where a
bug discards a real call. So the bridge stores what it actually sees, raw frames
in both directions, and the console rebuilds its own incidents by replaying them.
One interpreter, and what is on disk is evidence rather than somebody's reading
of it. It is also the right thing for an incident review, which will want the
frames as received and not a summary.

Operator actions are stored alongside, but only the ones a frame cannot carry.
Acknowledgement needs no record of its own: it puts an `ACK` frame on the air and
that frame is stored. Marking an unidentified incident as seen sends nothing, so
it is recorded explicitly, or a restart would alarm again for something a human
already dealt with.

A console attaching is replayed the last twelve hours. During the rebuild nothing
sounds and the map is left alone, and the alarm is judged once at the end on what
actually remains open, so a restart does not replay hours of sirens. An incident
nobody replied to before the crash is still someone waiting, and it alarms.

Verified by killing the bridge with `SIGKILL` mid-operation and restarting it:
incidents, repeat counts, a late arriving position, an acknowledgement and a seen
action all came back, an unhandled call alarmed again, and the handled ones
stayed closed.

The console still holds no durable state of its own and still uses no browser
storage API. It does not need to. The bridge outlives the browser, which is what
made this the right place for it.

## D7. Coverage is documented honestly, including gaps

**Decided.** Previously stated as a rule in CLAUDE.md with no record here.

A safety system that fails silently in a dead zone is worse than none, because it
manufactures false confidence and creates an expectation the agency then has to
meet. This runs through the product and not only the pitch, which means the
console has to make loss of coverage visible rather than sitting quietly green.
The coverage map from the site survey is published to visitors rather than held
internally, for the same reason.

## D8. An incident with no device id is cleared by a human, not by a reply

**Decided, August 2026.** A distress frame whose device id cannot be read still
raises an alarm, and there is no beacon to reply to. Such an incident is cleared
by an explicit operator action, Mark as seen, which sends no frame.

Without it, one garbled frame would hold the console in alarm for the rest of
the shift, and a console that is permanently red is one nobody looks at, which
defeats the point of the alarm being unmissable.

D4 is untouched. A human still confirms they have looked before the incident
leaves the alarm. The only thing that cannot happen is telling the beacon, and
the console says that in plain words rather than offering a button that cannot
work.

## D9. The console is several classic scripts, not one file and not modules

**Decided, August 2026.** Previously the console was one HTML file, and the
convention said to keep it that way.

The reason for that rule was never the file count. It was that the console must
open from disk, with no build step, on a machine that may have no internet. That
still holds and is now tested: logic lives in `console/lib/*.js`, loaded with
plain script tags, which are not CORS checked and load fine from `file://`.
Verified by loading the page from `file://` in a real browser, not assumed.

What this buys is that the parser is importable by `node:test`, so the piece
where a bug discards a real distress call is tested directly rather than by
copying code into a test.

ES modules were rejected. They are CORS checked, so the page would stop opening
from `file://` and would need a local server, which breaks the assumption the
rule existed to protect.

**Correction, August 2026.** The verification above was done in Chrome only and
was reported as though it settled the question. It did not. Rechecked since in
Firefox, where the console also loads from `file://`, so the decision stands, but
two things came out of looking properly:

- OpenStreetMap refuses map tiles to a request carrying no `Referer`, which is
  what a `file://` page sends. The map is blank from disk in every browser,
  whatever happens with the scripts. That is not fixable from this side.
- Browsers do not agree about what a `file://` document may load, and the set of
  browsers is not ours to choose.

So the supported way to run the console is served by the bridge on the loopback
address, which makes it an ordinary same origin page: the map works, the cross
origin question disappears, and no browser has an opinion about it. Opening from
disk still works and is still the fallback when there is no bridge, minus the
map. The no build step rule is untouched, and the files are still plain scripts
that a browser reads directly.

## D10. Own firmware, not a Meshtastic module. D1 reversed

**Decided, September 2026.** This reverses D1, which said to build on Meshtastic
as a custom module rather than fork it. That decision was right when it was made
and stopped being right, and the reason it stopped is worth recording, because
the argument that made it right is still a good argument.

### What went wrong with it

The module never ran on a radio. Everything else in the project did: the console
works, the bridge works, the payload contract is ratified and tested. The one
part that was supposed to make it a real system was the one part that stayed
theoretical, and the cost of keeping it able to run kept growing:

- upstream needed three local patches before it would build on macOS at all
- the single file a module has to register itself in moves upstream constantly,
  so the integration patch did not apply and the instructions degraded to "edit
  these two lines by hand"
- a Meshtastic node serves one API client at a time, so the bridge and any other
  tool fought over the same board
- exercising more than one node needed Meshtastic's own simulator, which wants
  an X11 terminal, so multi-hop testing needed XQuartz or a Linux box

Against that it bought mesh routing that had never been used, on hardware that
had never been flashed. The dependency was making the project look further along
than it was, which is the worst thing a dependency can do to a safety system.

### What was actually gained

Three things, and only the third was expected.

**A demonstration that runs this week.** The radio sits behind a narrow
interface, `firmware/src/transport.h`, so a beacon can be built with the USB
cable in place of the antenna. One board runs the real beacon firmware and a
laptop plays the mesh. That is not a mock: everything above the transport is the
code that would run on the air.

**Firmware that can be tested.** The old module's own header admitted that none
of its glue could be tested without hardware, and so none of it was: 77 host
checks, all of them frame parsing. The roles now take an injected transport and
clock, so the entire beacon state machine, button held to acknowledgement
received, runs in a compiler. 202 checks, and the ones that matter are the
behavioural ones that did not exist before.

**Licensing stopped being a constraint.** D5 exists because the Meshtastic
Python library is GPL-3.0-only. That pressure is gone. D5 stands anyway, on
reasons it turned out to have independently; see the note at the top of
`bridge/treksafe_bridge.py`.

### What was given up, stated plainly

This is the part that goes in front of Parks Canada rather than being discovered
by them.

**The mesh is prototype-grade and nothing here pretends otherwise.** It is a
flood mesh: every repeater rebroadcasts anything it has not heard before until
the hop count runs out. That is perhaps two hundred readable lines and it
genuinely works. What it does not have is duty cycle management, collision
avoidance beyond not transmitting at the same instant, encryption, or a single
hour of field testing. Meshtastic had all of that and years of it.

**No encryption.** Meshtastic encrypts channel traffic. This does not. A distress
frame carries a device id and a position in clear text, and anybody with a
receiver on the frequency can read it. The registry keeps names off the air,
which limits the exposure to "somebody is in trouble at this coordinate", but
that is a real disclosure and it needs deciding on before deployment rather than
after.

**No duty cycle enforcement.** Canadian use of this band falls under ISED
RSS-247. Nothing in the firmware counts airtime. Before anything transmits
outside a bench this has to be checked by somebody who has read the rules.

**The regression risk is ours now.** Upstream fixes used to arrive free.

### The mitigation, and it is the whole reason this is survivable

The radio is behind `Transport`, which is two methods. Replacing the mesh, with
Meshtastic, with a commercial stack, or with satellite backhaul, is a new file
implementing that interface. The roles, the link layer and the contract do not
change. A safety system should be able to change its radio without reopening the
logic that decides when to shout for help, and this one can.

### What was kept

The payload contract, v1, unchanged on the wire. Not one byte of a distress
frame changed when the entire radio stack underneath it was replaced, which is
what the schema version in field 2 was for and is the clearest evidence the
contract was drawn in the right place.

The old module is kept, unbuilt and unimported, in
`firmware/archive-meshtastic/`. The bridge's Meshtastic sources are kept the same
way in `bridge/archive-meshtastic/`. Neither is referenced by anything and
neither may become referenced; an archive something depends on stops being an
archive and becomes a directory nobody maintains but everything needs.

### Revisit this if

The flood mesh cannot carry the traffic a real deployment produces, or a
coverage survey shows it needs routing rather than flooding, or encryption
becomes a requirement before there is time to implement it properly. Any of
those is a reason to put Meshtastic back behind the `Transport` interface rather
than to start again.
