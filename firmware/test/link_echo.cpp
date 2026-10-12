// Prints how the firmware's link layer reads each line of standard input.
//
// Not a test on its own. It is one half of a differential test: the other half
// is sim/link_echo.py, which prints the same thing for the simulator's link
// layer, and run-tests.sh feeds both the same cases and diffs the output.
//
// That test exists because link.cpp and sim/mesh.py are two implementations of
// one specification, which is a standing invitation for them to drift. When it
// was first run they disagreed on five inputs, and every disagreement was the
// simulator being more permissive than the firmware: Python's int() quietly
// accepts a leading plus, surrounding whitespace and underscore separators, so
// the simulated mesh carried frames a real repeater would have dropped. A
// simulator that is kinder than reality is worse than no simulator, because the
// demonstration passes and the ridge does not.
//
// One line in, one line out, so the two can be compared with diff:
//
//   OK <origin> <seq> <hops> <payload>
//   REJECT

#include "../src/link.h"

#include <stdio.h>
#include <string.h>

int main()
{
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        // Strip only the line ending the file format added. Anything else,
        // including stray spaces and tabs, is part of the case being tested and
        // must reach the parser untouched.
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        // parseLink splits in place, so it gets a copy and the original is left
        // alone. Nothing here should depend on that, but relying on it would
        // make this harness part of what is being tested.
        char work[512];
        snprintf(work, sizeof(work), "%s", line);

        treksafe::LinkFrame f;
        if (treksafe::parseLink(work, f))
            printf("OK %s %u %u %s\n", f.origin, (unsigned)f.seq, (unsigned)f.hops, f.payload);
        else
            printf("REJECT\n");
    }
    return 0;
}
