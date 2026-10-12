#!/bin/sh
# Double-click this on a Mac to see TrekSafe working.
#
# It works out for itself what you have plugged in:
#
#   a board running the beacon firmware   the real firmware drives the demo.
#                                         Hold the button and watch the console.
#   nothing plugged in                    everything is simulated, including the
#                                         beacon, and a scripted incident plays.
#
# Either way it starts the bridge, opens the console in your browser, and every
# frame crosses the same link layer and the same mesh rules the firmware uses.
#
# What is real and what is not is stated on screen and in this window, and the
# console marks every simulated signal reading as simulated rather than showing
# it as something that was measured. Nothing invented here is ever written into
# a real incident record: the store is a throwaway file, created fresh each run.
#
# To flash a board first:
#     cd firmware && pio run -e beacon-serial -t upload
#
# Press Ctrl+C in this window to stop. Closing the window takes the console down.

cd "$(dirname "$0")" || exit 1

DEMO_STORE="$(mktemp -d)/demo.db"

# Ask the serial code itself what it can see, rather than guessing at /dev here.
# One place knows what a plugged-in board looks like, and it is the same place
# that has to open it.
PORTS=$(python3 - <<'PY' 2>/dev/null
import sys
sys.path.insert(0, "sim")
try:
    from serial_link import list_candidate_ports
    print("\n".join(list_candidate_ports()))
except Exception:
    pass
PY
)
COUNT=$(printf '%s' "$PORTS" | grep -c . 2>/dev/null || echo 0)

echo "TrekSafe demonstration"
echo

if [ "$COUNT" -eq 1 ]; then
    echo "Found a board on $PORTS"
    echo "Running the real beacon firmware over the cable. The repeaters, the"
    echo "gateway and every signal reading are simulated on this Mac."
    echo
    echo "Hold the button on the board for three seconds to raise a distress call."
    SOURCE="node"
elif [ "$COUNT" -gt 1 ]; then
    echo "More than one serial device is plugged in, so this will not guess which"
    echo "is the board:"
    printf '%s\n' "$PORTS" | sed 's/^/    /'
    echo
    echo "Falling back to the fully simulated demonstration. To use the board, run:"
    echo "    python3 bridge/treksafe_bridge.py --source node:/dev/cu.YOURS --open"
    echo
    SOURCE="sim:4"
else
    echo "No board plugged in, so everything is simulated: the beacon, the"
    echo "repeaters, the gateway and every signal reading."
    echo
    echo "A scripted incident starts in a few seconds. A party checks in, the"
    echo "button goes under canopy with no GPS lock, the lock arrives, and the"
    echo "beacon repeats until you acknowledge it in the console."
    echo
    echo "To use a real board instead:  cd firmware && pio run -e beacon-serial -t upload"
    SOURCE="sim:4"
fi

echo
echo "Keep this window open. Press Ctrl+C to stop."
echo

exec python3 bridge/treksafe_bridge.py \
    --source "$SOURCE" \
    --store "$DEMO_STORE" \
    --open
