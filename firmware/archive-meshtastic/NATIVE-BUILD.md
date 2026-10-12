# Building and running the module natively

Meshtastic builds natively for macOS and Linux, and its native binary can
simulate the LoRa chip. That is how this module gets exercised without a board,
and it is how the first real bugs in it were found.

Done once on macOS 15, arm64, August 2026. Everything below actually ran.

## What it proves

Running natively, the module has been observed to:

- start inside real Meshtastic firmware and derive its device id from the node
  number
- build a contract frame from real device state and transmit it, at
  `meshtastic_MeshPacket_Priority_HIGH` on `TEXT_MESSAGE_APP`
- repeat an unacknowledged call on the documented schedule
- receive an `ACK` addressed to it, match its own device id, and stop repeating

The bridge has been observed to read those frames through the real Meshtastic
Python library over TCP, and to transmit the acknowledgement back.

What it does not prove is radio: range, interference, trees, cold batteries,
duty cycle in the field. That still needs hardware.

## Setup

```bash
mkdir -p ~/Desktop/treksafe-native && cd ~/Desktop/treksafe-native
python3 -m venv venv && ./venv/bin/pip install platformio meshtastic

git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/meshtastic/firmware.git firmware

# macOS prerequisites, as documented in variants/native/portduino/platformio.ini
brew install yaml-cpp libuv openssl@3 libusb argp-standalone pkg-config jsoncpp

cd firmware
ln -sfn ~/Desktop/TrekSafe/firmware/src src/modules/TrekSafe
```

Then register the module. The patch in `integration/` will not apply, because
upstream moves that file constantly. It is two lines, in `src/modules/Modules.cpp`:

```cpp
#include "modules/TrekSafe/TrekSafeModule.h"          // with the other module includes
trekSafeModule = new TrekSafeModule(TrekSafeModule::ROLE_BEACON);   // in setupModules()
```

Two lines is the whole diff to upstream. If it ever grows past a handful, that is
the signal D1 deserves re-opening, not a reason to quietly fork.

## Three local fixes the macOS target needs

None of these are ours, and none are Meshtastic design faults. They are gaps
between a Linux-first codebase and BSD libc. They are worth reporting upstream.

1. **`src/platform/portduino/PortduinoGlue.cpp`** streams a
   `std::filesystem::directory_entry` to `std::cout`. libstdc++ provides that
   operator, libc++ does not. Use `entry.path().string()`.
2. **`framework-portduino/cores/portduino/AsyncUDP.h`** uses `std::vector`
   without including `<vector>`. libstdc++ pulls it in transitively, libc++ does
   not. This one lives in the downloaded PlatformIO package, so it comes back
   whenever that package updates.
3. **`strchrnul`** is a GNU extension macOS libc does not have, and Homebrew's
   `argp-standalone` references it, so the link fails. A four line shim in
   `src/macos_gnu_shims.c` guarded by `#ifdef __APPLE__` is enough.

## Build and run

```bash
# a build that fires one distress call fifteen seconds after boot, for testing
PLATFORMIO_BUILD_FLAGS="-DTREKSAFE_TRIGGER_AFTER_MS=15000" \
    ../venv/bin/pio run -e native-macos

# a node, with a simulated radio
.pio/build/native-macos/meshtasticd -s -e -d /tmp/ts-beacon -p 4403 &

# the bridge, attached to it, serving the console
~/Desktop/treksafe-native/venv/bin/python \
    ~/Desktop/TrekSafe/bridge/treksafe_bridge.py --source meshtastic-tcp:4403 --open
```

`TREKSAFE_TRIGGER_AFTER_MS` exists only because a native node has no button to
hold. It defaults to zero, meaning never. A shipped beacon fires when a person
holds the button and at no other time.

## One node, or several

One node exercises everything above: the module transmits, and because the
firmware also delivers locally originated packets to its own modules, an `ACK`
sent back through the same node reaches the beacon and stops it repeating. That
was verified.

Several nodes, which is what tests real relaying and multi-hop, needs
[Meshtasticator](https://github.com/meshtastic/Meshtasticator) to act as the
radio medium between them. Its interactive simulator launches each node in a
terminal and wants `gnome-terminal` or `xterm`, so on macOS it needs XQuartz or
a Linux box. Not done here.

Note also that a node serves **one API client at a time**. The bridge is that
client. A second script connecting to the same port will fight it.

## What running it found

Two real bugs, neither visible to any amount of reading:

- **The beacon transmitted every distress call twice**, two milliseconds apart.
  `sendDistress()` recorded `millis()` after the tick that was about to check
  it, so the elapsed time underflowed in unsigned arithmetic to about four
  billion milliseconds and the repeat fired instantly. `RepeatSchedule::due()`
  now uses a signed difference, which survives both the 49 day `millis()` wrap
  and a timestamp slightly in the future, and the tick's clock is threaded
  through rather than sampled again.
- **The bridge counted one transmission as two frames**, because the Meshtastic
  library publishes a received packet on more than one pubsub topic and both
  propagate to the parent topic the bridge subscribes to. Deduplicated on the
  packet id, which leaves a genuine repeat from the beacon intact because that
  is a new packet with a new id.
