# Beacon firmware

A Meshtastic module, not a fork. See D1 in `docs/decisions.md`. The code lives
here and is symlinked into an unmodified upstream checkout at build time, so a
Meshtastic release can be pulled without a merge.

## What is here

```
src/TrekSafeFrames.h/.cpp   payload contract v1, no Meshtastic dependency
src/TrekSafeModule.h/.cpp   the module: button, GPS, radio, LED
test/test_frames.cpp        host tests, no board and no toolchain needed
test/emit_frames.cpp        prints frames to paste into the console
test/stubs/                 enough of the Meshtastic API to compile check the module
integration/                the two line change upstream needs
```

The split is deliberate. Everything that decides what goes on the air is in
`TrekSafeFrames`, which compiles on a laptop and is tested there, because that
is where a bug means a malformed distress call or a missed acknowledgement.
`TrekSafeModule` is glue and holds no logic worth testing away from hardware.

## Compile check, no toolchain required

```bash
c++ -std=c++11 -Wall -Wextra -Wformat=2 -fsyntax-only \
    -I firmware/test/stubs -I firmware/src \
    -D TREKSAFE_BUTTON_PIN=4 -D LED_PIN=35 \
    firmware/src/TrekSafeModule.cpp
```

Read `test/stubs/README.md` before trusting a green run here. It is the cheap
check, not the real one.

## Run the tests, no hardware required

```bash
c++ -std=c++11 -Wall -Wextra -o /tmp/test_frames firmware/test/test_frames.cpp firmware/src/TrekSafeFrames.cpp && /tmp/test_frames
```

64 checks. They cover a complete distress call, a beacon under canopy with no
lock, a missing clock, unknown accuracy and battery, an unrecognised category,
an unusable device id, a frame where nothing at all is known, acknowledgement
parsing including the lenient cases, the press and hold timing, and the repeat
schedule including its behaviour when the millisecond counter wraps at 49 days.

## Check the two ends agree, still no hardware

```bash
c++ -std=c++11 -o /tmp/emit_frames firmware/test/emit_frames.cpp firmware/src/TrekSafeFrames.cpp && /tmp/emit_frames
```

That prints the exact bytes the firmware would transmit. Paste any line into
the raw box in `console/index.html`. All six currently parse with no
rejections, and the acknowledgement the console sends back is accepted by
`parseAck`. That round trip is the only test that crosses the firmware and
console boundary, and it is worth keeping green.

## Build it

You need PlatformIO. Nothing on this repo's console side needs a build step;
this does.

```bash
git clone https://github.com/meshtastic/firmware.git meshtastic-firmware
cd meshtastic-firmware
git submodule update --init --recursive
ln -s ../../../TrekSafe/firmware/src src/modules/TrekSafe
git apply ../TrekSafe/firmware/integration/register-module.patch
pio run -e heltec-v4 -t upload
```

Adjust the symlink path to wherever this repo sits. Set the button pin for your
hardware first, see below.

## The button

`TREKSAFE_BUTTON_PIN` has no default and the module will not fire without it.
That is on purpose. On a Heltec V4 running stock Meshtastic, a long press on
the board's user button is the shutdown gesture, so putting a hold to fire
distress button on the same pin means the board powers off partway through a
distress call. Wire a button to a free pin, to ground, and set:

```
-D TREKSAFE_BUTTON_PIN=<pin>
```

A press must be held for three seconds. A beacon that fires on a knock in a
backpack sends wardens up a mountain for nothing. The LED quickens as the hold
builds so the gesture is learnable and can be abandoned.

## What the LED says

The board has one LED, so the status is carried by rhythm, not colour, and
never by words. This is the beacon side of the rule in `CLAUDE.md`: assume the
person holding it is cold, injured, frightened, or does not read English.

| Pattern | Meaning |
| --- | --- |
| Fast flicker, quickening | The button is held, the call has not gone yet |
| Two short pulses, repeating | Distress sent, nobody has confirmed |
| Slow steady breathing pulse | A human at the base station has seen it |

The third one is the whole reason the return path exists. See D3.

## Testing the whole path with no board

This is done and written up in `NATIVE-BUILD.md`. What follows is the summary.

## Testing the whole path with one board

You do not need a second radio to exercise beacon to base station. Meshtastic
builds natively for macOS and Linux, and since firmware 1.3.42 a native node can
**simulate its LoRa chip over a local TCP port**, so several native nodes on one
laptop mesh with each other as though they were on the air. That covers the part
this project has never been able to test: the module running, a frame crossing a
mesh, and the acknowledgement coming back.

```bash
# one native binary, built once
cd meshtastic-firmware
ln -s ../../../TrekSafe/firmware/src src/modules/TrekSafe
git apply ../TrekSafe/firmware/integration/register-module.patch
pio run -e native

# two nodes on one machine, meshing over simulated LoRa.
# the first is the beacon, the second is the gateway.
.pio/build/native/meshtasticd -d /tmp/ts-beacon  -p 4403 &
.pio/build/native/meshtasticd -d /tmp/ts-gateway -p 4404 &

# point the bridge at the gateway
python3 bridge/treksafe_bridge.py --source meshtastic-tcp:4404 --open
```

Then trigger a distress call on the beacon node and watch it arrive in the
console, acknowledge it, and watch the beacon stop repeating. Add a third node
between them and multi-hop becomes testable too, which `CLAUDE.md` had written
off as impossible until there were three boards.

What this does not test is radio: real range, real interference, real trees,
real battery behaviour in the cold. It tests everything above the antenna, which
is where all of this project's code lives. A second board is still what proves
the link; this proves the logic.

The [interactive simulator](https://meshtastic.org/docs/software/meshtasticator/interactive-sim/)
drives the same native binaries with a script if you want repeatable scenarios
rather than pressing things by hand.

## What is not tested

Be blunt about this when demonstrating it.

- **Nothing has run on a radio.** The module has been built against real
  Meshtastic and run, but natively, with a simulated LoRa chip. Range,
  interference, trees and cold batteries are all still unknown.
- ~~The module has never been built against real Meshtastic.~~ It has, on macOS,
  August 2026. See `NATIVE-BUILD.md`. It compiled, ran, transmitted a distress
  call at HIGH priority, repeated it on schedule, received an acknowledgement
  and stopped. Two real bugs came out of that first run.
- **Old note, kept because it explains the stubs.** It does now reach
  a compiler: `run-tests.sh` compiles it against the stubs in `test/stubs`, in
  five configurations, under `-Wall -Wextra -Wformat=2`, clean. That proves the
  file is internally consistent, which it had never been shown to be. It does not
  prove it agrees with upstream, because the stubs are a checked-in fixture that
  cannot notice upstream changing. `pio run` is still the real check.
- **Nothing has been flashed.** No board has run this.
- **The path between beacon and base station has never been exercised.** There
  is one Heltec V4. Flashing beacon firmware onto it means there is nothing
  left to receive what it sends. A second board is what unblocks this, not more
  code.
- **Multi-hop is untested and will stay untested** until there are at least
  three boards. Do not write tests that assume otherwise.

What has been tested is every frame this firmware can produce, against the
console that has to read it, in both directions.

## Open questions this code takes a position on

Both are marked in the source at the setting they control, and neither is
ratified.

- **The SOS repeats until acknowledged**, at 30s, 60s, 120s, then every 5
  minutes, stopping only on acknowledgement. The schedule lives in
  `treksafe::RepeatSchedule`, which is host tested: an off by one there once
  made the first repeat arrive a minute late instead of thirty seconds. A single call that does not reach
  a gateway is lost and the person never knows. The console correlates the
  repeats into one incident and shows a repeat count, so this does not multiply
  incidents on screen. Issue I10.
- **A late GPS lock sends a fresh distress call immediately** rather than
  waiting for the next repeat, because the console is holding an incident with
  no position on it. The console lands that position on the existing incident.
  The contract still has no frame dedicated to a position update, which is a
  question left open under the payload contract.
