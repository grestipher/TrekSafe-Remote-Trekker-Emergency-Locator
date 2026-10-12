# Compile stubs

Just enough of the Meshtastic and Arduino API for `TrekSafeModule.cpp` to reach a
compiler without the firmware tree, the ESP32 toolchain, or a board.

## What this proves

That the module is internally consistent: it parses, its own types line up, it
calls `TrekSafeFrames` correctly, every member it uses exists, and there are no
unused or uninitialised variables. That is worth having, because until this
existed the file had never been compiled at all and could have contained a
missing semicolon.

## What this does not prove

That it compiles against real Meshtastic. The signatures here were copied from
upstream at the time of writing and are checked in as a fixture, so they cannot
notice if upstream changes, and a stub that is wrong in the same way the module
is wrong will happily agree with it.

**A green run here is not a substitute for `pio run`.** It is the cheap check you
can do in a second, so that when you do reach for the real build the only things
left are real API differences.

## Run it

```bash
c++ -std=c++11 -Wall -Wextra -fsyntax-only \
    -I firmware/test/stubs -I firmware/src \
    -D TREKSAFE_BUTTON_PIN=4 -D LED_PIN=35 \
    firmware/src/TrekSafeModule.cpp
```

`run-tests.sh` does this for you.
