# TrekSafe node firmware

One codebase, three roles, two transports. Which you get is fixed at build time,
so a board is flashed as exactly one thing and a build is reproducible from the
name of its environment.

This is TrekSafe's own firmware. It was a Meshtastic module until September 2026;
see `docs/decisions.md`, D10, for why that changed and what it cost. The old
module is in `archive-meshtastic/`, unbuilt and unimported.

## The three roles

| Role | Runs on | What it does |
| --- | --- | --- |
| beacon | the thing a visitor carries | one button. Hold it, a distress call goes out and keeps going out until a human answers |
| repeater | a solar box on a ridge | rebroadcasts anything it has not heard before, and reports its own health so a dying one is known about in daylight |
| gateway | the radio beside the base station | translates between the mesh and a USB cable. Has no opinion about what anything means |

The base station itself is not firmware. It is the computer running the console,
and the gateway is how the mesh reaches it.

## Building

Needs [PlatformIO](https://platformio.org/install). Nothing else.

```bash
cd firmware
pio run                        # every environment
pio run -e beacon-serial       # just one
pio run -e beacon-serial -t upload -t monitor   # flash it and watch it talk
```

| Environment | Role | Radio | For |
| --- | --- | --- | --- |
| `beacon-serial` | beacon | the USB cable | the one-board demonstration |
| `beacon-serial-selftrigger` | beacon | the USB cable | the same, but fires by itself 15 s after boot |
| `beacon-lora` | beacon | real | a beacon on the air. Needs a second board to hear it |
| `repeater` | repeater | real | a relay |
| `gateway` | gateway | real | the radio at the base station |
| `boardprobe` | none | none | asks an unknown board which board it is |

## The one-board demonstration

The whole system, with one radio, and it is not a mock.

`beacon-serial` builds the beacon with the USB cable in place of the antenna.
Every frame it would have transmitted comes up the cable instead, prefixed
`AIR`. A simulated mesh on the laptop, `sim/`, plays the repeaters and the
gateway, and the acknowledgement travels all the way back and stops the real
firmware repeating.

```bash
cd firmware && pio run -e beacon-serial -t upload
cd .. && ./start-demo.command          # finds the board by itself
```

**Real:** the beacon firmware, its state machine, its repeat schedule, the
frames it builds, the link envelope, and how it handles the acknowledgement.
**Simulated:** the repeaters, the gateway, the signal readings, and the air.

Every invented signal reading is labelled all the way to the console, which
shows it as `SIMULATED, not measured` and keeps it out of the coverage survey.
A survey built from invented numbers is the false confidence D7 exists to
prevent.

## The board

`boards/heltec_v4.h` is the only file that knows a pin number exists. If the
board changes, that is the one file to edit.

**Do not write that file from a web page.** Run the probe:

```bash
pio run -e boardprobe -t upload -t monitor
```

It asks the hardware which board it is: which pin powers the display, whether
the battery enable is active high or low, and which pin the button is on. It
drives no radio and writes nothing but itself, so it is safe on an unknown
board.

That exists because the obvious approach failed. Heltec state the V4 keeps the
V3's pinout, which is true of the header pins and hides four things that are
not: an RF front end the V3 has no equivalent of, a battery enable whose
polarity is inverted, a different display controller, and an onboard GPS. A
version of this firmware written from the V3 map would have run, reported
success, and transmitted nothing at all.

Four settings in there are not pins, and every one of them fails **silently**:

| | Without it |
| --- | --- |
| DIO3 TCXO at 1.8 V | the radio drifts off frequency and nothing demodulates |
| DIO2 as RF switch | transmit power never reaches the antenna |
| RF front end powered and enabled | same, on a V4: the PA sits between chip and antenna |
| Transmit power below the chip maximum | the front end adds about 11 dB, so 22 dBm in is far over the legal limit out |

All four report success while doing nothing.

## Layout

```
boards/heltec_v4.h     every pin and radio setting, and nothing else has one
src/contract.{h,cpp}   payload contract v1. What a distress call says
src/link.{h,cpp}       the mesh envelope. How one travels
src/timing.{h,cpp}     repeat schedule, hold gesture, the millis() guard
src/transport.h        the seam: Transport, HostPort, DeviceState, Indication
src/transport_lora.*   the real radio, via RadioLib
src/transport_serial.* the USB cable standing in for the air
src/role_*.{h,cpp}     the three behaviours. No hardware in any of them
src/config.h           every tunable, all overridable with -D
src/display.{h,cpp}    the 128x64 panel on the front
src/main.cpp           the only file that touches a pin
src/probe.cpp          board identification. Built only by `boardprobe`
test/                  host tests. No board, no dependency
```

## The screen

The beacon shows what it is doing in words as well as by LED rhythm:

| State | Screen |
| --- | --- |
| idle | `READY`, device id, position, battery, "Hold button for help" |
| holding | `KEEP HOLDING` with a filling progress bar |
| calling | `CALLING FOR HELP` / **Stay where you are** / calls sent |
| acknowledged | `HELP IS COMING` / They know where you are |
| could not send | `NOT SENT` / Move and try again |

The LED is not made redundant by this and must not be. CLAUDE.md's rule is that
beacon status is legible to somebody cold, frightened, or not reading English,
which a rhythm does and a sentence does not. What the screen adds is
confirmation, which is the thing a rhythm is worst at and the thing D3 says the
whole product turns on: a slow pulse means "help is coming" only to somebody who
was taught what it means. So every screen pairs a large word with a state the
LED is showing anyway, and neither is load bearing alone.

A repeater or gateway shows its role, id, how much it has heard and relayed, and
its battery, which is what somebody standing at a box on a ridge needs in order
to decide whether to open it.

A board with no panel runs identically and says so once at startup.

## Testing

```bash
../run-tests.sh        # everything, including a build for the board
```

The roles are driven by an injected transport and clock, so the whole of a
beacon's life runs in a compiler: button held, call out, repeats widening,
acknowledgement received, calling stops, check-ins begin. That is the point of
`src/transport.h` and it is why the interface is worth the indirection. The
firmware this replaced could not test any of it, and said so in its own header.

Four suites, 205 checks: contract, timing, link, roles. Plus a compile check
across every combination of build flags, and a real `pio run` of every
environment in platformio.ini.

## What has never been done

Written here rather than in a commit message, because it is what somebody
picking this up needs to know first.

- **Nothing has been on the air.** Not one frame has crossed a radio. The
  beacon has been flashed, pressed, and seen to raise a real distress call that
  reached the console and was acknowledged, but all of that travelled over the
  USB cable. Range, interference, trees, cold batteries and duty cycle are all
  unknown, and `transport_lora.cpp` has never run.
- **The RF front end has never been switched on.** Its pins are in the board
  header and nothing drives them yet, because nothing has needed the radio.
  That has to be written before `beacon-lora`, `repeater` or `gateway` can work.
- **The transmit power figure is somebody else's measurement.** 11 dBm is chosen
  so that roughly 22 dBm leaves the antenna given an 11 dB front end, and that
  gain figure comes from Meshtastic's notes on a similar board, not from an
  instrument on this one.
- **Only one board exists**, so the multi-node behaviour that the link layer is
  entirely about, relaying and deduplication across real nodes, has been
  exercised only in tests and in software.
- **The battery reading is uncalibrated.** The firmware says so at startup.
  Check `TS_BATTERY_MULTIPLIER` against a multimeter before anyone relies on it.
- **No regulatory check has been done.** See D10.
