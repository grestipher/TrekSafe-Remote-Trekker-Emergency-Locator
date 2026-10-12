# The simulated mesh

A LoRa mesh in software, so the whole system can be demonstrated with one radio.

The Heltec runs the real beacon firmware, built so its transport is the USB
cable rather than the antenna. This plays everything on the other side of that
cable: the repeaters that relay the frame, and the gateway that hands it to the
base station. An acknowledgement travels back the same way and stops the real
firmware repeating.

## It is not a mock

It runs the same link format the firmware puts on the air, parsed from the same
field layout, and it makes the same decisions a repeater makes: deduplicate,
count the hop, drop it when it has gone far enough. `mesh.py` and
`firmware/src/link.cpp` are two implementations of one specification, and
`test_mesh.py` tests this one against the same cases the C++ suite tests that
one.

That duplication is a real cost and it is the point of the tests. A simulator
that is more generous than the real thing is worse than no simulator, because
everything passes and nothing has been learned.

## What it cannot do, and says so

- **Signal readings are invented.** They come from a model in
  `default_snr_model` that is roughly shaped like reality and is not a
  measurement. Every one of them is labelled `simulated` all the way through the
  bridge, the store and a restart, to the console, which shows it as
  `SIMULATED, not measured` and keeps it out of the coverage survey.
- **Packet loss is a coin flip**, not propagation.
- **There is no timing, no collision, no duty cycle and no interference.**

What it does prove is everything above the antenna: that a real beacon's frames
are well formed, that a repeater's rules carry them, that the gateway's host
protocol is right, and that an acknowledgement typed by a human at the console
reaches the beacon and stops it. That is most of the system and none of it was
testable before.

## Files

```
mesh.py          the link format, deduplication, repeaters, gateway
beacon.py        a beacon in software, for a demonstration with no board at all
serial_link.py   reading a USB serial port with nothing installed
test_*.py        all of the above
```

`serial_link.py` uses `termios` rather than pyserial, because this project has no
dependencies and the person getting it running at a park office may not have a
working pip. macOS and Linux only. It is tested against a pseudo-terminal, which
is a real kernel character device, so everything except the physical cable is
covered.

One thing in there is worth knowing about if you ever touch it. A quiet port and
an unplugged board both return zero bytes from a read, so distinguishing them
needs `select`, not the read alone. Getting that wrong means an unplugged board
looks exactly like a calm night: the bridge loops forever telling every console
the feed is healthy while nothing is listening. The comment in `read_lines`
records the measurements behind the fix.

## Running it

Usually through the bridge rather than directly:

```bash
python3 bridge/treksafe_bridge.py --source sim --open        # no hardware at all
python3 bridge/treksafe_bridge.py --source sim:10 --open     # ten times faster
python3 bridge/treksafe_bridge.py --source node --open       # a real board over USB
```

To check a cable is carrying data before blaming anything else:

```bash
python3 sim/serial_link.py                  # prints whatever the board says
```
