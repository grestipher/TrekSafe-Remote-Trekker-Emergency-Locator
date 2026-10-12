// Host tests for the link layer. No board, no radio, no dependency.
//
//   c++ -std=c++11 -Wall -Wextra -o /tmp/test_link \
//       firmware/test/test_link.cpp firmware/src/link.cpp firmware/src/contract.cpp \
//       && /tmp/test_link
//
// The link layer is where a mistake costs the whole mesh rather than one
// message: a frame that cannot be deduplicated is rebroadcast by every repeater
// that hears it, forever, and takes the airtime every other distress call needs
// with it. So the deduplication cases are tested harder than the parsing ones.

#include "../src/link.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void eq(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL %s\n       got  %s\n       want %s\n", what, got, want);
    }
}

static void ok(const char *what, bool cond)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

// parseLink splits in place, so every test needs its own writable copy.
static char *scratch(const char *s)
{
    static char buf[8][treksafe::kLinkFrameMax];
    static int turn = 0;
    char *out = buf[turn++ % 8];
    snprintf(out, treksafe::kLinkFrameMax, "%s", s);
    return out;
}

static const char *const SOS = "SOS|1|A4F2|52.8734|-118.0814|12|1755000000|I|86";

int main()
{
    char buf[treksafe::kLinkFrameMax];

    printf("building\n");
    {
        size_t n = treksafe::buildLink(buf, sizeof(buf), "A4F2", 41, 0, SOS);
        eq("a distress call, freshly on the air", buf,
           "TS1|A4F2|41|0|SOS|1|A4F2|52.8734|-118.0814|12|1755000000|I|86");
        ok("reports the length it wrote", n == strlen(buf));
        ok("an SOS with its envelope stays well under the buffer", n < treksafe::kLinkFrameMax);

        treksafe::buildLink(buf, sizeof(buf), "A4F2", 65535, 3, "OK|1|A4F2|0|0||0");
        eq("the extremes of both numeric fields", buf, "TS1|A4F2|65535|3|OK|1|A4F2|0|0||0");

        // See link.h: an unattributable frame cannot be deduplicated, and a
        // flood mesh rebroadcasts such a frame forever.
        ok("refuses to send a frame nobody could deduplicate",
           treksafe::buildLink(buf, sizeof(buf), "ZZ!Z", 1, 0, SOS) == 0);
        eq("and leaves nothing behind to send by accident", buf, "");
        ok("refuses an empty origin", treksafe::buildLink(buf, sizeof(buf), "", 1, 0, SOS) == 0);
        ok("refuses a null origin", treksafe::buildLink(buf, sizeof(buf), 0, 1, 0, SOS) == 0);
        ok("refuses an empty payload", treksafe::buildLink(buf, sizeof(buf), "A4F2", 1, 0, "") == 0);

        ok("a buffer too small returns 0 rather than a truncated frame",
           treksafe::buildLink(buf, 12, "A4F2", 1, 0, SOS) == 0);
        eq("and leaves an empty string", buf, "");
    }

    printf("parsing\n");
    {
        treksafe::LinkFrame f;
        ok("a frame straight off the air",
           treksafe::parseLink(scratch("TS1|A4F2|41|2|SOS|1|A4F2|52.87|-118.08|12|0|I|86"), f));
        eq("origin", f.origin, "A4F2");
        ok("sequence", f.seq == 41);
        ok("hops taken", f.hops == 2);
        eq("the payload comes back whole, pipes and all", f.payload,
           "SOS|1|A4F2|52.87|-118.08|12|0|I|86");

        ok("heard directly is zero hops, not missing",
           treksafe::parseLink(scratch("TS1|A4F2|1|0|OK|1|A4F2|0|0||0"), f) && f.hops == 0);

        ok("a lowercase tag and origin still parse, for a hand typed test",
           treksafe::parseLink(scratch("ts1|a4f2|1|0|OK|1|A4F2|0|0||0"), f));
        eq("and the origin is normalised to what the contract requires", f.origin, "A4F2");

        ok("a payload containing no pipes at all is still a payload",
           treksafe::parseLink(scratch("TS1|A4F2|1|0|hello"), f));
        eq("carried verbatim", f.payload, "hello");

        printf("parsing, things that are not our frames\n");
        ok("a bare contract frame with no envelope is not a link frame",
           !treksafe::parseLink(scratch(SOS), f));
        ok("a link version we do not speak is dropped rather than guessed at",
           !treksafe::parseLink(scratch("TS2|A4F2|1|0|SOS|1|A4F2"), f));
        ok("an unreadable origin is dropped, it could not be deduplicated",
           !treksafe::parseLink(scratch("TS1|ZZ!Z|1|0|SOS|1|A4F2"), f));
        ok("a sequence that is not a number is dropped",
           !treksafe::parseLink(scratch("TS1|A4F2|4x|0|SOS|1|A4F2"), f));
        ok("a sequence too large for the field is dropped",
           !treksafe::parseLink(scratch("TS1|A4F2|70000|0|SOS|1|A4F2"), f));
        ok("a hop count that is not a number is dropped",
           !treksafe::parseLink(scratch("TS1|A4F2|1|x|SOS|1|A4F2"), f));
        ok("a frame with no payload is not worth relaying",
           !treksafe::parseLink(scratch("TS1|A4F2|1|0|"), f));
        ok("too few fields", !treksafe::parseLink(scratch("TS1|A4F2|1"), f));
        ok("an empty line", !treksafe::parseLink(scratch(""), f));
        ok("noise", !treksafe::parseLink(scratch("hello there"), f));
        ok("a null line", !treksafe::parseLink(0, f));

        // Round trip, because the two halves have to agree and nothing else
        // checks that they do.
        treksafe::buildLink(buf, sizeof(buf), "B117", 1234, 1, SOS);
        ok("what we build, we can read back", treksafe::parseLink(buf, f));
        eq("same origin", f.origin, "B117");
        ok("same sequence", f.seq == 1234);
        ok("same hops", f.hops == 1);
        eq("same payload", f.payload, SOS);
    }

    printf("deduplication\n");
    {
        treksafe::SeenCache seen;
        ok("the first time a message is offered it is new", seen.isNew("A4F2", 1, 1000));
        ok("the second time it is not", !seen.isNew("A4F2", 1, 1100));
        ok("and it stays not new however many relays repeat it",
           !seen.isNew("A4F2", 1, 1200) && !seen.isNew("A4F2", 1, 5000));

        ok("the beacon's next call is a different message", seen.isNew("A4F2", 2, 2000));
        ok("a different beacon with the same sequence is a different message",
           seen.isNew("B117", 1, 2000));

        // The failure this exists to prevent, stated as a test: two repeaters
        // in range of each other must not rebroadcast each other's
        // rebroadcasts.
        ok("a frame coming back from the other repeater is not rebroadcast again",
           !seen.isNew("A4F2", 2, 2100));
    }

    printf("deduplication, a beacon that rebooted\n");
    {
        // The subtle one. A beacon that reboots starts counting again, and a
        // repeater that remembered the old numbers forever would silently drop
        // the first frames of a real new distress call. Nothing anywhere would
        // log it. Forgetting is what bounds that.
        treksafe::SeenCache seen(60000); // forget after a minute, for the test

        ok("heard during the first run", seen.isNew("A4F2", 1, 1000));
        ok("still remembered a moment later", !seen.isNew("A4F2", 1, 2000));
        ok("still remembered just before the window closes", !seen.isNew("A4F2", 1, 60999));

        ok("after the window, the same numbers are a new distress call again",
           seen.isNew("A4F2", 1, 62000));

        // And the entry must age from when the message was first heard, not
        // from the last time something asked about it, or a beacon repeating
        // one sequence number would hold that key alive forever.
        treksafe::SeenCache ageing(60000);
        ok("first heard", ageing.isNew("C0DE", 7, 0));
        for (uint32_t t = 1000; t < 60000; t += 1000)
            ageing.isNew("C0DE", 7, t); // repeats, all suppressed
        ok("the window still closes on schedule despite the repeats",
           ageing.isNew("C0DE", 7, 61000));
    }

    printf("deduplication, staying bounded\n");
    {
        // A repeater runs for a season. The cache must not grow, and it must
        // not stop working when it fills.
        treksafe::SeenCache seen(0); // never forget, so only the ring bounds it
        bool allNew = true;
        for (uint16_t i = 0; i < 500; i++)
            allNew = seen.isNew("A4F2", i, 1000) && allNew;
        ok("five hundred distinct messages are each seen as new", allNew);
        ok("stays inside its fixed size", seen.remembered() <= TREKSAFE_SEEN_CAPACITY);
        ok("the most recent message is still remembered", !seen.isNew("A4F2", 499, 1000));
        ok("one long since evicted is new again, which is the cost of a fixed size",
           seen.isNew("A4F2", 0, 1000));

        treksafe::SeenCache noKey;
        ok("a frame with no origin is never dropped, there is no key for it",
           noKey.isNew("", 1, 1000) && noKey.isNew("", 1, 1000));
        ok("and nothing is remembered under an empty key", noKey.remembered() == 0);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
