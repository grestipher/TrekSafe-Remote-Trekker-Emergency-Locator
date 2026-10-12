// Host tests for the timing and gesture logic. No board, no dependency.
//
//   c++ -std=c++11 -Wall -Wextra -o /tmp/test_timing \
//       firmware/test/test_timing.cpp firmware/src/timing.cpp && /tmp/test_timing
//
// Split from the contract tests because these answer a different question. The
// contract tests ask what goes on the wire; these ask when, and every one of
// them is here because getting "when" wrong is silent. A repeat that never
// fires, or fires twice, looks identical to working code from the outside.

#include "../src/timing.h"

#include <stdio.h>

static int failures = 0;
static int checks = 0;

static void ok(const char *what, bool cond)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL %s\n", what);
    }
}

int main()
{
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
