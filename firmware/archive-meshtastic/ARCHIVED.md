# Archived: the Meshtastic module

Nothing in this directory is built, tested, flashed or referenced by anything
else in the repository. It is kept because the work and the reasoning are worth
being able to read, not because any of it still runs.

If you are looking for the firmware that is actually built, it is one level up,
in `firmware/src`. Do not include a file from here into that build. An archive
that something depends on stops being an archive and becomes a directory nobody
maintains but everything needs.

## What this was

TrekSafe as a custom module inside upstream Meshtastic firmware, which was
decision D1. `TrekSafeModule.*` is the Meshtastic glue: a `SinglePortModule`
plus an `OSThread`, reading Meshtastic's GPS, battery and button state and
handing packets to its `MeshService`. `TrekSafeFrames.*` is the payload contract
v1, with no Meshtastic dependency at all, and it was tested on the host by
`test/test_frames.cpp`, 77 checks.

`test/stubs/` is a set of fake Meshtastic headers that existed so
`TrekSafeModule.cpp` could be compile checked without an upstream checkout. They
were a fixture, and they could not notice when upstream changed.

## Why it was dropped

Recorded properly in `docs/decisions.md` as D10. The short version is that the
module never ran on a radio, and the cost of keeping it able to was visible and
growing:

- it needed three local patches before upstream would build on macOS at all
- the one file the module has to register itself in moves upstream constantly,
  so the integration patch did not apply and the instructions were "edit these
  two lines by hand"
- a Meshtastic node serves one API client at a time, so the bridge and any other
  tool fought over it
- exercising more than one node needed Meshtastic's own simulator, which wants
  an X11 terminal and therefore XQuartz or a Linux box

Against that it bought mesh routing that had never actually been used, on
hardware that had never actually been flashed.

## What survived

The payload contract v1, unchanged on the wire. `firmware/src/contract.{h,cpp}`
is seeded directly from `src/TrekSafeFrames.{h,cpp}` here, and the host checks
came with it. The frames a beacon puts on the air did not change when the
firmware underneath them was replaced, which is the whole reason the contract
has a schema version sitting in field 2.
