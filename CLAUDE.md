# TrekSafe base station

Context for anyone, human or model, picking this up. Read this before changing code.

## What this is

A park-owned LoRa mesh emergency beacon network. A visitor is issued a beacon at
the trailhead. One button transmits an identifier and a GPS position. Solar-powered
repeater nodes relay it to a base station at a staffed facility, which raises an
alarm on a map. The base station then sends an acknowledgement back to the beacon.

Intended customer is Parks Canada, as an operator rather than as a user. A value
proposition document and pilot proposal already exist outside this repo.

The system does not make rescues faster to carry out. It makes them start sooner
and start with a location. Keep that framing when writing docs or copy.

## Current state

`docs/open-issues.md` holds the triaged review of the console. All ten issues
are closed. Read it before changing console behaviour, since several of the
fixes encode safety reasoning that is easy to undo by accident.

- The console works. No build step, no dependencies beyond Leaflet from a CDN,
  pinned by content hash. Logic lives in `console/lib/` as classic scripts and
  is covered by 106 tests under `node --test`.
- The bridge works, as its own process. With it running the console has a live
  feed, a gateway heartbeat and a working return path. 89 tests.
- **TrekSafe has its own firmware.** It was a Meshtastic module and is not any
  more; see D10, which records what that cost as well as what it bought. One
  codebase, three roles, two transports, and it builds for the board. 202 host
  checks, including the entire beacon state machine.
- **The whole system runs on one board.** The radio sits behind a two-method
  interface, so a beacon can be built with the USB cable in place of the
  antenna. The real firmware transmits, a simulated mesh on the laptop relays,
  the console alarms, and an acknowledgement travels back and stops it. See
  `firmware/README.md`.
- Overdue parties are watched. A party past its stated return time is raised
  from the registry and last-heard time, with no button pressed.
- Acknowledgement is two phase. The console does not treat a reply as sent until
  something outside it confirms the frame went out, and an incident stays in
  alarm until then. Clearing on the click alone turned a failed send into a
  green screen, which is the exact state D3 exists to prevent.
- Incidents survive a crash. The bridge stores every frame and every operator
  action in SQLite and replays the last twelve hours to a console that attaches.
  D6. The console itself still holds no durable state and uses no browser
  storage API.
- Simulated signal readings are labelled as simulated everywhere, including
  through the store and a restart, and are kept out of the coverage survey. A
  survey built from invented numbers is the false confidence D7 is about.

Hardware on hand: one Heltec V4 (ESP32-S3 + SX1262).

**Nothing has ever been on the air.** Not one frame has crossed a radio. The
link layer, which is the part that relaying and deduplication are entirely
about, has been exercised in host tests and in software and nowhere else. Range,
interference, trees, cold batteries and duty cycle are all unknown, and no
regulatory check has been done. Everything above the antenna is testable and
tested; the antenna is not.

## Decisions already made, do not re-open without asking

1. **TrekSafe has its own firmware, built on nothing.** D10, September 2026,
   reversing D1. The Meshtastic module never ran on a radio and the cost of
   keeping it able to kept growing. What replaced it is testable, demonstrable
   on one board, and honest about being prototype-grade radio. D10 records the
   omissions, and they are real: no encryption, no duty cycle management, no
   field testing.
2. **The radio lives behind a narrow transport interface.** `firmware/src/
   transport.h`, two methods. It is why the one-board demonstration works, why
   the roles are testable at all, and the escape hatch if the mesh has to be
   replaced. Do not let role logic reach past it.
3. **Base station first, firmware second.** The console is what a Parks Canada
   evaluator actually experiences. The radio work is invisible to them.
4. **Acknowledgement is required, not optional.** A beacon pressed into silence
   makes people move, which is what turns a short rescue into a multi-day
   search. The return path is a core feature.
5. **Acknowledgement is manual, and is not complete until confirmed.** A human
   confirms they have seen the incident before the beacon is told help is
   coming. Never auto-acknowledge, and never treat a reply as delivered because
   a button was clicked.
6. **Coverage is documented honestly, including gaps.** This runs through the
   product, not just the pitch. A safety system that fails silently in a dead
   zone is worse than none, because it manufactures false confidence. Nothing
   invented may ever be presented as measured.

## Open decisions, ask before choosing

- **Encryption.** A distress frame travels in clear text: a device id and a
  position, readable by anybody with a receiver on the frequency. The registry
  keeps names off the air, so the exposure is "somebody is in trouble here"
  rather than who, but that is still a disclosure and it needs deciding before
  deployment rather than after. Meshtastic gave this away free and D10 gave it
  up; that was a real cost.
- **Duty cycle and the regulator.** Canadian use of this band falls under ISED
  RSS-247. Nothing counts airtime. Somebody who has read the rules has to check
  this before anything transmits outside a bench.
- **The repeat schedule and the hop limit are proposals, not measurements.**
  Both sit in `firmware/src/config.h` and `link.h` with the reasoning attached.
  They should be revisited against a real coverage survey, not argued about in
  advance.
- **Where the beacon registry lives.** Today it is `console/registry-data.js`,
  edited by hand. That is fine for a pilot and not fine for a fleet.

Persistence, D6, is decided: SQLite in the bridge, storing frames and operator
actions rather than incidents, so the console stays the only thing that
interprets a frame.

Bridge transport, D5, is decided: a separate process talking to the console over
a local HTTP connection. The original reason was the GPL library, which D10
removed; it stands on what it turned out to buy, above all that it outlives the
browser and so makes D6 possible.

Firmware, D10, is decided: TrekSafe's own, not a Meshtastic module. Read it
before proposing either a return to Meshtastic or a hard fork.

Payload contract v1 was ratified in August 2026 and is no longer open. The
frames did not change when the entire radio stack underneath them was replaced.
See `docs/payload-contract.md`, which also documents the link layer that now
carries them, and the questions still open underneath both.

## Payload contract, v1

Pipe delimited on purpose: readable in any serial terminal, and typeable by
hand, which is what made it testable long before there was firmware to produce
it. Text is the right answer now and protobuf is the right answer for
production, which the schema version exists to allow.

```
SOS|1|<devid>|<lat>|<lon>|<acc_m>|<epoch>|<cat>|<batt>
OK |1|<devid>|<lat>|<lon>|<acc_m>|<epoch>
TEL|1|<devid>|<batt>|<volts>|<snr>
ACK|1|<devid>|<incident_id>        (base station to beacon)
```

- `cat`: `M` medical, `I` injury, `L` lost, `U` unspecified
- Roughly 45 bytes for an SOS, plus about 20 for the link envelope that carries
  it, which keeps airtime low at these settings
- The schema version sits in field 2 deliberately. Two bytes now buys the ability
  to change everything else later without bricking deployed beacons.
- `0|0` or empty coordinates means no GPS fix yet. This is a valid distress
  message, not an error. See the rule below.

On the air each of these travels inside a link envelope,
`TS1|<origin>|<seq>|<hops>|<payload>`, which is documented in
`docs/payload-contract.md` and implemented in `firmware/src/link.cpp` and
`sim/mesh.py`. The two layers version independently and the payload did not
change when the mesh under it was replaced.

If this contract changes, it changes in `docs/payload-contract.md`, in the console
parser, and in the firmware, in the same commit.

## Rules that come from the problem domain

These are not style preferences. Breaking them makes the product unsafe.

- **Never drop a packet silently.** Every parser rejection must log a human
  readable reason. A malformed distress message that vanishes is the worst failure
  this system can have.
- **`NO FIX` is a first-class state.** A beacon under heavy canopy that has not
  acquired a lock still raises the alarm, with position arriving later. Rejecting
  it as invalid discards a real distress call.
- **The alarm must be unmissable.** The console changes state as a whole rather
  than showing a notification, because the failure mode is a warden who is not
  looking at the screen. Do not soften this into a toast.
- **Design for the warden on shift, not for an engineer.** No debug console
  aesthetics, no jargon in labels, no required training to read the screen.
- **On the beacon, assume the user is cold, injured, frightened, or does not
  read English.** Beacon status must be legible without words: colour and
  pattern, not colour alone. This rule is about the beacon. The console is
  operated by trained staff and should use plain English words. What carries
  over to the console is the narrower requirement: no status may be conveyed by
  colour alone, and no status may depend on animation, since reduced-motion
  users must still see it.

## Conventions

- No build step for the console, ever. Classic scripts only, never ES modules:
  modules are CORS checked and would stop the page loading from `file://`. D9.
- Normal way to run the console is served by the bridge at
  `http://127.0.0.1:8730/`, which exists only while the bridge runs. Opening
  `console/index.html` from disk still works and is the fallback with no
  bridge, but the map is blank there because OpenStreetMap refuses tiles to a
  request with no `Referer`. Do not treat the disk case as the primary one when
  changing how the console loads.
- The bridge runs from any directory: its default paths are anchored to the
  repository, not the cwd. It fails setup problems as one plain sentence, and
  it kills itself if its source stops reading rather than heartbeating over a
  dead radio. Keep those properties.
- No browser storage APIs.
- Plain prose in docs, no em dashes.
- `window.feedPacket(line)` is the hook where a live feed enters the console.
  It calls `ingest()`. `console/bridge-client.js` calls it for every frame the
  bridge delivers.
- `window.sendToMesh(frame, {onSent, onFailed})` is the hook where outbound
  acknowledgements leave the console. `console/bridge-client.js` defines it, but
  only while the bridge is actually reachable. If it is undefined the console
  says out loud that the acknowledgement did not leave the machine. Do not
  define it unconditionally, and do not let it report success without having
  been told: the incident stays open and in alarm until `onSent` fires.
- `window.feedHeartbeat()` is how the gateway says it is alive. Distinct from
  traffic on purpose: a quiet mesh at 3am is normal, a dead gateway is not.
- In the firmware, nothing above `src/transport.h` may know a radio exists, and
  only `src/main.cpp` and `boards/heltec_v4.h` may know a pin exists. That is
  what keeps the roles testable on a host, and it is easy to break by reaching
  for `millis()` or a `digitalRead` inside role logic.
- Never write `now - since >= interval` on a millisecond clock. Use
  `treksafe::elapsedAtLeast`. The unsigned version underflows when the stamp is
  a millisecond ahead of the tick, which is the ordinary case, and it made the
  beacon transmit every distress call twice. It has been found three times now.
- A device id is 2 to 8 **hex** characters. Naming a node `GW01` produces a node
  that transmits nothing at all and looks perfectly healthy doing it, because
  nothing can be attributed to it. Use `BA5E`, `FA01` and so on.
- The two archive directories, `firmware/archive-meshtastic/` and
  `bridge/archive-meshtastic/`, are not built, not tested and not imported.
  Never import from them. An archive something depends on stops being an archive.

## Layout

```
console/index.html    markup and wiring only
console/lib/          parser, incidents, nodes, watchdog, registry. Tested.
console/app.js        rendering and DOM. No logic worth testing lives here.
console/test/         node --test, no dependency

bridge/               mesh to console bridge, its own process. Tested.
bridge/node_source.py the sources that talk to TrekSafe's own firmware
bridge/store.py       SQLite, frames and operator actions. D6.

firmware/             TrekSafe's own firmware. Three roles, builds for the board.
firmware/boards/      the only file that knows a pin number exists
firmware/src/         contract, link, timing, transports, roles, main
firmware/test/        host tests. The whole beacon state machine, no board.

sim/                  a LoRa mesh in software, so one board demonstrates the lot
sim/serial_link.py    USB serial with nothing installed

docs/                 payload contract, link layer, decision log, closed issues
```

Two archive directories exist and are not part of any build:
`firmware/archive-meshtastic/` and `bridge/archive-meshtastic/`. See D10.

Run everything with `./run-tests.sh`. It needs no board and no internet; the
board build is skipped if PlatformIO is not installed, and `SKIP_PIO=1` skips it
when iterating.
