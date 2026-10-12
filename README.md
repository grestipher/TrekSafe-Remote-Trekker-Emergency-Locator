# TrekSafe

A park-owned LoRa mesh emergency beacon network. A visitor is issued a beacon at
the trailhead. One button transmits an identifier and a position. Solar repeater
nodes relay it to a base station at a staffed facility, which raises an alarm on
a map, and a human there sends an acknowledgement back to the beacon.

The system does not make rescues faster to carry out. It makes them start sooner
and start with a location.

## See it working

On a Mac, double-click `start-demo.command`. It works out what you have plugged
in, starts the bridge and opens the console by itself.

With a board flashed as a beacon, the real firmware drives the demonstration:
hold the button for three seconds and watch the console change state. With
nothing plugged in, everything is simulated and a scripted incident plays.

From a terminal, from any directory:

```bash
python3 bridge/treksafe_bridge.py --source sim --open
```

The console is served at http://127.0.0.1:8730/ and exists only while the bridge
runs. No build step, nothing to install.

Press **Acknowledge** on the incident and watch the beacon stop calling. That
round trip is the product: a beacon pressed into silence makes people move,
which is what turns a short rescue into a multi-day search.

## Run it on the board

```bash
cd firmware
pio run -e beacon-serial -t upload
```

That builds the beacon with the USB cable in place of the antenna, so one radio
demonstrates the whole system. Everything above the transport is the code that
would run on the air; the repeaters, the gateway and the signal readings are
simulated on the laptop and are labelled as simulated everywhere they appear.

See `firmware/README.md` for the other roles and what has never been tested.

## The parts

| Part | State |
| --- | --- |
| Operations console | Working. 106 tests |
| Bridge | Working, its own process, SQLite persistence. 89 tests |
| Node firmware | Three roles, builds for the board. 202 host checks. **Never been on the air** |
| Simulated mesh | Working, so one board demonstrates the lot. 41 tests |
| Overdue party watch | Working. Raises a party past its return time with no button pressed |
| Coverage survey | Working, built from real signal readings only |
| Incident record export | Working, `http://127.0.0.1:8730/export.csv` |

```bash
./run-tests.sh          # all of it. No board, no internet, nothing to install
```

## What is not true yet

Written here rather than buried, because a safety system that oversells itself
is worse than one that does not exist.

- **No frame has ever crossed a radio.** Range, interference, trees, cold
  batteries and duty cycle are all unknown.
- **One board**, so relaying and deduplication across real nodes have been
  exercised in tests and in software only.
- **The mesh is prototype-grade.** No encryption, no duty cycle management, no
  field testing. A distress frame travels in clear text. See D10.
- **No regulatory check has been done.** Canadian use of this band falls under
  ISED RSS-247.

## Reading the code

Start with `CLAUDE.md`. It records which decisions are settled, which are open,
and the domain rules that must not be broken. Then `docs/decisions.md`, which
carries the reasoning, including D10 on why this stopped being a Meshtastic
module and what that cost.
