// Host tests for the payload contract. No board, no toolchain beyond a C++
// compiler, no dependency.
//
//   c++ -std=c++11 -Wall -Wextra -o /tmp/test_frames \
//       firmware/test/test_frames.cpp firmware/src/TrekSafeFrames.cpp && /tmp/test_frames
//
// The parser and the frame builder are the one place where a bug means a real
// distress call is malformed or an acknowledgement is missed, so they are
// tested away from the radio where the failures are visible.

#include "../src/TrekSafeFrames.h"

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

static treksafe::Distress base()
{
    treksafe::Distress d;
    d.devid = "A4F2";
    d.hasFix = true;
    d.lat = 52.8734;
    d.lon = -118.0814;
    d.accM = 12;
    d.epoch = 1755000000u;
    d.cat = treksafe::CAT_MEDICAL;
    d.battPct = 87;
    return d;
}

int main()
{
    char buf[treksafe::kFrameMax];

    printf("device id\n");
    {
        char id[treksafe::kDevIdMax];
        treksafe::deviceIdFromNodeNum(0xDEADA4F2u, id);
        eq("low half of the node number, uppercase hex", id, "A4F2");
        treksafe::deviceIdFromNodeNum(0x00000001u, id);
        eq("leading zeroes are kept, the contract wants fixed width", id, "0001");
        ok("four hex characters is a valid id", treksafe::isValidDevId("A4F2"));
        ok("two is the minimum", treksafe::isValidDevId("A4"));
        ok("one is too short", !treksafe::isValidDevId("A"));
        ok("nine is too long", !treksafe::isValidDevId("A4F2A4F2A"));
        ok("lowercase is not valid, the contract says uppercase", !treksafe::isValidDevId("a4f2"));
        ok("non hex is not valid", !treksafe::isValidDevId("ZZZZ"));
    }

    printf("distress frames\n");
    {
        treksafe::Distress d = base();
        size_t n = treksafe::buildSos(buf, sizeof(buf), d);
        eq("a complete distress call", buf, "SOS|1|A4F2|52.8734|-118.0814|12|1755000000|M|87");
        ok("stays near the 45 bytes the contract budgets for", n <= 50);

        // The rule this product turns on: a beacon under canopy with no lock
        // still raises the alarm, and the position follows later.
        d = base();
        d.hasFix = false;
        treksafe::buildSos(buf, sizeof(buf), d);
        eq("no fix is sent as 0|0, not withheld", buf, "SOS|1|A4F2|0|0|12|1755000000|M|87");

        d = base();
        d.epoch = treksafe::kClockNotSet;
        treksafe::buildSos(buf, sizeof(buf), d);
        eq("no clock leaves the field empty rather than inventing a time", buf,
           "SOS|1|A4F2|52.8734|-118.0814|12||M|87");

        d = base();
        d.accM = treksafe::kUnknownInt;
        d.battPct = treksafe::kUnknownInt;
        treksafe::buildSos(buf, sizeof(buf), d);
        eq("unknown accuracy and battery are admitted, not fabricated", buf,
           "SOS|1|A4F2|52.8734|-118.0814||1755000000|M|");

        d = base();
        d.cat = 'Q';
        treksafe::buildSos(buf, sizeof(buf), d);
        eq("an unrecognised category degrades to U and still sends", buf,
           "SOS|1|A4F2|52.8734|-118.0814|12|1755000000|U|87");

        d = base();
        d.devid = "ZZZZ";
        treksafe::buildSos(buf, sizeof(buf), d);
        eq("an unusable device id still produces a distress call", buf,
           "SOS|1||52.8734|-118.0814|12|1755000000|M|87");

        // Everything absent at once. This is the worst case the hardware can
        // present and it must still put a distress call on the air.
        treksafe::Distress bare;
        bare.devid = 0;
        bare.hasFix = false;
        bare.lat = 0;
        bare.lon = 0;
        bare.accM = treksafe::kUnknownInt;
        bare.epoch = treksafe::kClockNotSet;
        bare.cat = 0;
        bare.battPct = treksafe::kUnknownInt;
        treksafe::buildSos(buf, sizeof(buf), bare);
        eq("nothing known at all still sends", buf, "SOS|1||0|0|||U|");

        d = base();
        ok("a buffer that cannot hold the frame returns 0 rather than truncating",
           treksafe::buildSos(buf, 10, d) == 0);
        ok("and leaves an empty string behind", buf[0] == '\0');
    }

    printf("check-in and telemetry\n");
    {
        treksafe::Distress d = base();
        treksafe::buildOk(buf, sizeof(buf), d);
        eq("check-in carries no category or battery", buf, "OK|1|A4F2|52.8734|-118.0814|12|1755000000");

        treksafe::buildTel(buf, sizeof(buf), "A4F2", 64, 4.02, -3.54);
        eq("telemetry", buf, "TEL|1|A4F2|64|4.0|-3.5");

        treksafe::buildTel(buf, sizeof(buf), "A4F2", treksafe::kUnknownInt, 4.0, 0.0);
        eq("unknown battery on a repeater is left empty", buf, "TEL|1|A4F2||4.0|0.0");
    }

    printf("acknowledgement\n");
    {
        treksafe::Ack a;
        ok("the frame the console actually sends", treksafe::parseAck("ACK|1|A4F2|INC-002", "A4F2", a));
        eq("names our device", a.devid, "A4F2");
        eq("carries the incident", a.incident, "INC-002");
        ok("and says so", a.hasIncident);

        ok("a missing incident id is still an acknowledgement, someone looked",
           treksafe::parseAck("ACK|1|A4F2", "A4F2", a));
        ok("recorded as having no incident", !a.hasIncident);

        ok("lowercase from a hand typed test still reaches us",
           treksafe::parseAck("ack|1|a4f2|INC-002", "A4F2", a));
        ok("whitespace around the frame is tolerated",
           treksafe::parseAck("  ACK|1|A4F2|INC-002\r\n", "A4F2", a));

        ok("an acknowledgement for another beacon is not ours",
           !treksafe::parseAck("ACK|1|B001|INC-002", "A4F2", a));
        ok("a schema we do not speak is not acted on",
           !treksafe::parseAck("ACK|2|A4F2|INC-002", "A4F2", a));
        ok("a schema field that is not a number is not acted on",
           !treksafe::parseAck("ACK|1x|A4F2|INC-002", "A4F2", a));
        ok("a distress frame is not an acknowledgement",
           !treksafe::parseAck("SOS|1|A4F2|52.8|-118.0|12|1755000000|M|87", "A4F2", a));
        ok("noise is not an acknowledgement", !treksafe::parseAck("hello", "A4F2", a));
        ok("an empty line is not an acknowledgement", !treksafe::parseAck("", "A4F2", a));
        ok("a beacon with no id of its own accepts nothing",
           !treksafe::parseAck("ACK|1|A4F2|INC-002", "", a));
    }

    printf("repeat schedule\n");
    {
        // The schedule the firmware ships with.
        static const uint32_t steps[] = {30000, 60000, 120000, 300000};
        treksafe::RepeatSchedule r(steps, 4);

        ok("nothing is due before anything has been sent", !r.due(0));

        r.sent(1000);
        ok("the first wait is the first step, not the second", r.waitMs() == 30000);
        ok("not due a second early", !r.due(1000 + 29999));
        ok("due at thirty seconds", r.due(1000 + 30000));

        r.sent(31000);
        ok("then a minute", r.waitMs() == 60000);
        ok("not due early", !r.due(31000 + 59999));
        ok("due at a minute", r.due(31000 + 60000));

        r.sent(91000);
        ok("then two minutes", r.waitMs() == 120000);

        r.sent(211000);
        ok("then five", r.waitMs() == 300000);

        r.sent(511000);
        ok("and it holds at five, it never gives up on someone", r.waitMs() == 300000);
        r.sent(811000);
        ok("still five", r.waitMs() == 300000);

        r.restart();
        ok("a new call starts the schedule over", !r.due(811000));
        r.sent(900000);
        ok("back to thirty seconds", r.waitMs() == 30000);

        // A beacon left switched on for a season must not go quiet when the
        // millisecond counter wraps, which it does at about 49 days.
        treksafe::RepeatSchedule w(steps, 4);
        const uint32_t nearMax = 0xFFFFFFFFu - 10000;
        w.sent(nearMax);
        ok("not due before the wait, across the wrap", !w.due(nearMax + 29999));
        ok("due after the wait, across the wrap", w.due(nearMax + 30000));

        // The bug that shipped: a send recorded a millisecond after the tick
        // that is checking it. Unsigned subtraction turned that into about
        // four billion milliseconds elapsed and fired a repeat at once, so the
        // beacon transmitted the same distress call twice, two milliseconds
        // apart, on real Meshtastic.
        treksafe::RepeatSchedule f(steps, 4);
        f.sent(20000);
        ok("a send timestamped just after the check is not instantly due", !f.due(19999));
        ok("nor a whole second before", !f.due(19000));
        ok("and the real wait still applies afterwards", !f.due(20000 + 29999));
        ok("due once the wait has actually passed", f.due(20000 + 30000));

        treksafe::RepeatSchedule empty(steps, 0);
        empty.sent(1000);
        ok("an empty schedule never claims something is due", !empty.due(999999));
    }

    printf("periodic timing\n");
    {
        // The same underflow the repeat schedule was fixed for, pulled out so
        // every periodic check in the module uses it. Two of them did not, and
        // wrote `now - since >= interval` inline: the acknowledged beacon's
        // check-in and the repeater's telemetry would each have gone out twice
        // whenever the stamp landed a millisecond after the tick that was about
        // to compare against it, which is the ordinary case and not a rare one.
        using treksafe::elapsedAtLeast;

        ok("nothing has elapsed at the same instant", !elapsedAtLeast(1000, 1000, 1));
        ok("not before the interval", !elapsedAtLeast(1000 + 9999, 1000, 10000));
        ok("exactly at the interval", elapsedAtLeast(1000 + 10000, 1000, 10000));
        ok("and after it", elapsedAtLeast(1000 + 10001, 1000, 10000));

        // A stamp taken inside the tick sits after the tick's own clock.
        ok("a stamp one millisecond in the future is not instantly elapsed",
           !elapsedAtLeast(20000, 20001, 600000));
        ok("nor a whole second in the future", !elapsedAtLeast(20000, 21000, 600000));

        // 49 days, for a beacon left switched on all season.
        const uint32_t nearWrap = 0xFFFFFFFFu - 5000;
        ok("not elapsed just before the interval, across the wrap",
           !elapsedAtLeast(nearWrap + 9999, nearWrap, 10000));
        ok("elapsed at the interval, across the wrap",
           elapsedAtLeast(nearWrap + 10000, nearWrap, 10000));

        ok("an interval of zero is always elapsed", elapsedAtLeast(5, 5, 0));
    }

    printf("press and hold\n");
    {
        treksafe::HoldToFire h(3000);
        ok("a knock does not fire", !h.update(true, 0));
        ok("still not at two seconds", !h.update(true, 2000));
        ok("released early, nothing happens", !h.update(false, 2500));

        h.reset();
        ok("hold starts", !h.update(true, 10000));
        ok("part way through", !h.update(true, 12000));
        ok("the hold shows its progress so it can be abandoned", h.heldMs(12000) == 2000);
        ok("fires on the tick the hold completes", h.update(true, 13000));
        ok("and only once, holding longer does not send a second call", !h.update(true, 14000));
        ok("no progress shown once fired", h.heldMs(14000) == 0);

        ok("releasing after a fire does not fire again", !h.update(false, 15000));
        ok("a second deliberate hold is allowed", !h.update(true, 16000));
        ok("and fires on its own merit", h.update(true, 19000));
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
