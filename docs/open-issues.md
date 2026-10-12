# Console issues

From a review of the console against the rules in `CLAUDE.md`, August 2026,
ordered originally by how much damage each defect did. All ten are now closed.
What is still open is at the bottom, and none of it is a defect: it is hardware
and one undecided question.

Two documentation errors the same review found were fixed first: the wordless
status rule now states that it governs the beacon rather than the console, and
`CLAUDE.md` no longer claims `window.feedPacket` is unimplemented.

## Closed, priority 1, safety relevant

### I1. A malformed SOS raised no alarm

A packet beginning `SOS` that failed validation was logged and nothing else
happened. The console stayed green and read Standby. Against the stated failure
mode, a warden not looking at the screen, that distress call was functionally
dropped even though the rejection was logged.

**Fixed.** Parsing is two stages. `classify()` decides only whether field 0 reads
`SOS`, and a frame that does always produces an incident and an alarm.
`hydrateDistress()` then reads whatever it can and cannot reject anything.
Non-distress frames keep the strict path, since neither a check-in nor telemetry
raises an alarm.

### I2. A distress call was discarded when only one field was bad

The `NO FIX` principle was applied to `0|0` and empty coordinates only. An
out-of-range latitude, a garbled device id or an unrecognised category threw the
whole SOS away. `SOS|1|A4F2|999|-118.08|...|M|87` is a person pressing the button
with a corrupt GPS field, and it was treated as line noise.

**Fixed.** Bad coordinates degrade to `NO FIX`, an unrecognised category to `U`,
and an unreadable device id still raises an incident marked unidentified. Every
substitution is recorded on the incident and shown in the log and under
Incomplete in the detail panel, so nothing is replaced quietly. A `NO FIX` caused
by a fault reads differently from a beacon that honestly has no lock.

Two consequences, both deliberate:

- An unrecognised schema version on an `SOS` raises the alarm but no field after
  the version is read. At v1 offsets a later schema could produce a confidently
  wrong position, which is worse than none. The raw frame stays on screen.
- An incident with no device id has nobody to reply to, so the console says so
  instead of showing an acknowledge button. See D8.

### I3. The console did not notice when the mesh went quiet

Nothing watched the feed. Last packet counted upward and the console held green
Standby whether the mesh stopped two minutes or two days ago. Exactly the failure
D7 says the product must not have.

**Fixed.** `console/lib/watchdog.js`, driven by a gateway heartbeat the bridge
sends every 15 seconds rather than by mesh traffic, because a quiet mesh at 3am
is normal and a dead gateway is not. Warning at 90 seconds, lost at 300. Loss of
feed is a full width banner and a body level state change, the same class of
visibility as an alarm and visually distinct from one. A console that has never
had a feed says so, which is its state whenever the bridge is not running.

### I4. The audible alarm probably did not sound the first time, and never repeated

`alarmTone()` fired once per incident, and browsers block audio until a user
gesture, with that failure swallowed by an empty catch. On a console sitting
untouched since the shift started, the first distress call was silent.

**Fixed.** Arming is an explicit button and the status bar reads NOT ARMED until
it succeeds. Arming plays the tone at once, so the operator hears what an
incident will sound like. If the browser refuses, the status bar reads BLOCKED
and the reason goes in the log. While anything is unacknowledged the tone repeats
every 20 seconds.

## Closed, priority 2, correctness

### I5. Acknowledgement could be lost to a re-render

The whole UI rebuilt every second from `innerHTML`, which reset the queue scroll
position and could replace the acknowledge button between mousedown and mouseup.
This is the interaction the entire design rests on.

**Fixed.** Rendering happens on state change. The one second tick touches only
the clock, the relative times carried on `data-ts` elements, and the watchdog
banner. Verified in a browser: the acknowledge button is the same DOM node before
and after a tick.

### I6. Packet text was rendered as HTML

The event log and the detail panel went through `innerHTML`. The log is the
entire safety net for never dropping a packet silently, so it was the worst place
to have this.

**Fixed.** The queue, detail panel, event log and map popup are built as DOM
nodes with `textContent`. Verified with a frame carrying an `img onerror`
payload: nothing executes and nothing is injected.

### I7. Node health mixed beacons with repeaters

`touchNode()` ran for every frame type, so visitor beacons landed in the repeater
panel and each turned red five minutes after it was issued. Nodes up was
therefore not an honest coverage figure.

**Fixed.** `console/lib/nodes.js` separates fixed infrastructure from visitor
beacons. Telemetry marks a device as fixed, and once fixed it stays fixed, so a
repeater that goes quiet still counts against coverage. The panel is now Repeater
health, and coverage reads "none known" rather than "0 / 0", which could be read
as everything being fine.

### I8. A missing sender clock silently became local time

An implausible or absent timestamp fell back to `Date.now()` and was shown under
the label Sent, presenting a fabricated time as beacon reported.

**Fixed.** The sender clock is null when absent, zero or implausible, and the
detail panel says "not reported, beacon clock not set". Sent by beacon, First
heard and Last heard are three separate rows. The contract clarification that
came with ratification says the same thing on the wire.

## Closed, priority 3, needed a decision

### I9. Incidents were headlined by a hex device id

A warden saw `A4F2`. Not a bug, a question about where identity lives, running
into the privacy position that the mesh deliberately carries no names.

**Built**, the way this issue guessed. `console/lib/registry.js` looks the device
id up locally against `console/registry-data.js` and shows name, party size,
return date and notes, with an overdue flag. The mesh carries the hex id and
nothing else, and the acknowledgement frame carries the device id only. Where
that record eventually lives is still D6.

### I10. Repeated SOS frames created separate incidents

Two frames from one beacon produced two incidents. Was blocked on whether the
beacon should repeat until acknowledged.

**Fixed**, and no longer blocked, because the firmware now repeats until
acknowledged and this stopped being hypothetical. `console/lib/incidents.js`
correlates by device id within 30 minutes, or by identical raw frame where there
is no readable id, and shows a repeat count. A repeat carrying a position lands
on the existing incident, which is what makes a `NO FIX` alarm followed by a late
fix work.

One case came out of building it. A beacon still calling well after it was
acknowledged has not heard the reply, so the person still believes nobody knows.
That re-opens the incident and offers Acknowledge again. Repeats inside a two
minute grace period do not, since those crossed with the reply in the air.

## Rejected

**The dark monospace treatment is not a defect.** It was read as debug console
aesthetics. Emergency dispatch equipment is legitimately dark and monospaced, and
data legibility at a glance is why. Jargon in labels was worth fixing, and was:
Nodes up is now Repeaters up, Node health is Repeater health, and node states
read heard, overdue and not heard rather than up, warn and down. The visual
register stays.

**Wordless status does not apply to the console.** That rule was written for the
beacon and filed without scope, which was a documentation error, now corrected. A
console operated by trained staff should use plain English. What carries over is
narrower and has been done: every tag and node state carries its meaning in
words, so nothing is conveyed by colour alone, and the alarm blink is a second
cue rather than the only one, so reduced motion loses nothing.

## Still open, not defects

- **Persistence is done.** D6 decided: SQLite in the bridge, storing frames and
  operator actions. Incidents survive a bridge crash and a browser reload.
- **Nothing has ever been on the air.** This is the one that matters. The
  firmware builds for the board and its whole beacon state machine is tested on
  a host, but no frame has crossed a radio, so range, interference, trees, cold
  batteries and duty cycle are all unknown.
- **One board**, so relaying and deduplication between real nodes have been
  exercised in host tests and in the software mesh and nowhere else. A second
  board is what unblocks that, not more code.
- **The `gateway` bridge source has never seen a radio.** The `node` source has
  been run against a real board over a real cable, which tested the serial
  layer; the mesh behind it was software.
- **No encryption and no duty cycle enforcement.** Both given up with D10 and
  both recorded there. A distress frame travels in clear text.
- The open questions under the payload contract, including whether a beacon can
  cancel a false alarm. See `docs/payload-contract.md`.
