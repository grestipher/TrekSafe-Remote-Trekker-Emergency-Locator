#!/bin/sh
# Every test in the project. No dependencies to install: node:test and
# unittest ship with node and python, and the firmware frame layer is plain
# C++ that any compiler will build.
#
#     ./run-tests.sh
#
# Nothing here needs a board, a radio, or an internet connection.

set -e
cd "$(dirname "$0")"

fail=0

echo "=== console, node:test ==="
node --test 'console/test/*.test.js' || fail=1

echo
echo "=== bridge, unittest ==="
python3 -m unittest discover -s bridge -p 'test_*.py' || fail=1

echo
echo "=== simulated mesh, unittest ==="
# The software mesh the one-board demonstration runs on. Tested against the same
# rules the firmware follows, because a simulator more generous than the real
# thing is worse than no simulator: everything passes and nothing is learned.
python3 -m unittest discover -s sim -p 'test_*.py' || fail=1

# Everything under firmware/src except the hardware glue is plain C++ with no
# Arduino and no radio library in it, which is the whole point of the transport
# interface in src/transport.h. That is what lets the beacon's entire state
# machine, button to acknowledgement, run right here in a compiler.
#
# firmware/archive-meshtastic is deliberately not built. See its ARCHIVED.md.
TMP=$(mktemp -d)
PURE="firmware/src/contract.cpp firmware/src/timing.cpp firmware/src/link.cpp"
ROLES="firmware/src/role_beacon.cpp firmware/src/role_repeater.cpp firmware/src/role_gateway.cpp"

suite() {
    name="$1"; shift
    echo
    echo "=== firmware, $name ==="
    if c++ -std=c++11 -Wall -Wextra -Wformat=2 -o "$TMP/$name" "$@"; then
        "$TMP/$name" || fail=1
    else
        echo "  FAIL  $name did not build"; fail=1
    fi
}

suite contract firmware/test/test_contract.cpp firmware/src/contract.cpp
suite timing   firmware/test/test_timing.cpp   firmware/src/timing.cpp
suite link     firmware/test/test_link.cpp     firmware/src/link.cpp firmware/src/contract.cpp
suite roles    firmware/test/test_roles.cpp    $ROLES $PURE

echo
echo "=== the firmware and the simulator read a frame the same way ==="
# link.cpp and sim/mesh.py are two implementations of one specification, which
# is a standing invitation for them to drift. Both are fed the same cases and
# their answers are compared exactly.
#
# Worth the machinery: the first time this ran they disagreed on five inputs,
# and every disagreement was the simulator being the more permissive of the two.
# A simulator kinder than reality is worse than none, because the demonstration
# passes and the ridge does not.
# Plain temp files rather than process substitution, because this script is
# /bin/sh and <(...) is a bash extension.
if c++ -std=c++11 -Wall -Wextra -o "$TMP/link_echo" \
       firmware/test/link_echo.cpp firmware/src/link.cpp firmware/src/contract.cpp; then
    "$TMP/link_echo" < firmware/test/link-cases.txt > "$TMP/link_cpp.txt"
    python3 sim/link_echo.py < firmware/test/link-cases.txt > "$TMP/link_py.txt"
    if diff "$TMP/link_cpp.txt" "$TMP/link_py.txt" > "$TMP/linkdiff"; then
        cases=$(grep -c "" firmware/test/link-cases.txt)
        echo "  ok    both read all $cases cases identically"
    else
        echo "  FAIL  they disagree. Left is the firmware, right is the simulator:"
        sed 's/^/        /' "$TMP/linkdiff"
        fail=1
    fi
else
    echo "  FAIL  the comparison harness did not build"; fail=1
fi

echo
echo "=== firmware, builds under the configurations it ships in ==="
# Not a substitute for `pio run`, which is the only thing that proves it builds
# for the board. This catches the cheap half: that the pure layers stay free of
# hardware, and that every combination of build flags still compiles.
check() {
    desc="$1"; shift
    if c++ -Wall -Wextra -Wformat=2 -fsyntax-only "$@" $ROLES $PURE 2>/dev/null; then
        echo "  ok    $desc"
    else
        echo "  FAIL  $desc"; fail=1
    fi
}
check "defaults"                  -std=c++11
check "defaults, c++17"           -std=c++17
check "the simulator test trigger" -std=c++11 -DTREKSAFE_TRIGGER_AFTER_MS=15000
check "a tiny deduplication cache" -std=c++11 -DTREKSAFE_SEEN_CAPACITY=4
check "no relay jitter"           -std=c++11 -DTREKSAFE_RELAY_JITTER_MS=1

# The real thing: does it build for the board, with the Arduino framework and
# RadioLib underneath it. Everything above is a host compiler being asked
# whether the logic is consistent; only this answers whether it can be flashed.
#
# Skipped when PlatformIO is not installed, so the suite still runs anywhere,
# and skippable with SKIP_PIO=1 when iterating, since it takes about two minutes
# against a second and a half for everything else.
echo
echo "=== firmware, builds for the board ==="
if [ -n "$SKIP_PIO" ]; then
    echo "  skipped, SKIP_PIO is set"
elif ! command -v pio >/dev/null 2>&1; then
    echo "  skipped, PlatformIO is not installed. The firmware is untested against"
    echo "  the board on this machine: https://platformio.org/install"
else
    if (cd firmware && pio run >/dev/null 2>&1); then
        # No environment list: `pio run` with none builds every one in
        # platformio.ini, so adding an environment cannot silently escape this
        # check the way it did when the list was written out by hand here.
        echo "  ok    every environment in platformio.ini builds"
    else
        echo "  FAIL  a firmware environment did not build. Run it directly to see why:"
        echo "        cd firmware && pio run"
        fail=1
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "all suites passed"
else
  echo "SOMETHING FAILED, see above"
fi
exit "$fail"
