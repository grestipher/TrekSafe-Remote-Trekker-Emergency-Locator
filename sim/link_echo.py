"""Prints how the simulator's link layer reads each line of standard input.

The Python half of the differential test described in
firmware/test/link_echo.cpp. Output format is identical so the two can be
compared with diff, and run-tests.sh does exactly that.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mesh  # noqa: E402


def main():
    for line in sys.stdin:
        # Only the line ending is stripped. Anything else is part of the case.
        line = line.rstrip("\n").rstrip("\r")
        f = mesh.parse_link(line)
        if f:
            print("OK %s %d %d %s" % (f["origin"], f["seq"], f["hops"], f["payload"]))
        else:
            print("REJECT")


if __name__ == "__main__":
    main()
