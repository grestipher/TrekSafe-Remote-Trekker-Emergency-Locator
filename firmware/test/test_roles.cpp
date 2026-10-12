// Host tests for the three roles. No board, no radio, no dependency.
//
//   c++ -std=c++11 -Wall -Wextra -o /tmp/test_roles \
//       firmware/test/test_roles.cpp firmware/src/role_beacon.cpp \
//       firmware/src/role_repeater.cpp firmware/src/role_gateway.cpp \
//       firmware/src/link.cpp firmware/src/contract.cpp firmware/src/timing.cpp \
//       && /tmp/test_roles
//
// This file is the argument for the transport interface. Everything below runs
// a role's real logic against a fake radio: the whole of a beacon's life, from
// the button going down to an acknowledgement arriving, in a compiler, with the
// assertions on exactly what went on the air.
//
// The behaviours tested here are the ones where being wrong is silent. A beacon
// that stops repeating one call too early, a repeater that drops a frame it
// should have carried, a gateway that reports a signal reading it never took:
// none of those look like failures from outside. They look like a quiet mesh.

#include "../src/role_beacon.h"
#include "../src/role_gateway.h"
#include "../src/role_repeater.h"
#include "fakes.h"

#include <stdio.h>
#include <string.h>

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

static void eq(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL %s\n       got  %s\n       want %s\n", what, got, want);
    }
}

// True if haystack contains needle. Used to assert on the payload inside a link
// frame without restating the envelope, whose sequence numbers are seeded
// unpredictably in real use and are not what these tests are about.
static bool has(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != 0;
}

// The tick loop, as the firmware runs it. Roles are driven at a fixed cadence
// and every test that involves time advancing has to go through here, because
// "did the repeat fire" depends on the role being asked at all.
static void run(treksafe::BeaconRole &b, const treksafe::DeviceState &s, uint32_t &now,
                uint32_t forMs, uint32_t stepMs = 50)
{
    uint32_t until = now + forMs;
    while (now < until) {
        b.tick(now, s);
        now += stepMs;
    }
}

static treksafe::DeviceState located()
{
    treksafe::DeviceState s;
    s.hasFix = true;
    s.lat = 52.8734;
    s.lon = -118.0814;
    s.accM = 12;
    s.epoch = 1755000000u;
    s.battPct = 86;
    s.battVolts = 3.9;
    return s;
}

int main()
{
    printf("beacon, the gesture\n");
    {
        fake::Transport radio;
        treksafe::BeaconRole b(radio, "A4F2", 40);
        treksafe::DeviceState s = located();
        uint32_t now = 0;

        run(b, s, now, 5000);
        ok("nothing goes out while nobody touches it", radio.sentCount == 0);
        ok("and it says nothing is happening", b.indication() == treksafe::IND_IDLE);

        // A knock in a backpack. Held for well under the three seconds.
        s.buttonPressed = true;
        run(b, s, now, 1000);
        ok("a short press sends nothing", radio.sentCount == 0);
        ok("but it does show the hold building, so it can be abandoned",
           b.indication() == treksafe::IND_HOLDING);
        s.buttonPressed = false;
        run(b, s, now, 1000);
        ok("released early, still nothing", radio.sentCount == 0);
        ok("and back to resting", b.indication() == treksafe::IND_IDLE);

        // A deliberate hold.
        s.buttonPressed = true;
        run(b, s, now, TREKSAFE_HOLD_MS + 100);
        ok("holding it sends exactly one distress call", radio.sentCount == 1);
        ok("which is a distress call", has(radio.last(), "|SOS|1|A4F2|"));
        ok("carrying the position", has(radio.last(), "52.8734|-118.0814"));
        ok("wrapped for the mesh, having taken no hops yet", has(radio.last(), "TS1|A4F2|40|0|"));
        ok("and the beacon is now waiting to be heard",
           b.state() == treksafe::BeaconRole::STATE_AWAITING_ACK);
        ok("which it shows", b.indication() == treksafe::IND_AWAITING_ACK);

        // The one that matters: keeping the button down must not machine-gun
        // the mesh with the same call.
        run(b, s, now, 10000);
        ok("holding it down longer does not send a second call", radio.sentCount == 1);
    }

    printf("beacon, repeating until somebody answers\n");
    {
        fake::Transport radio;
        treksafe::BeaconRole b(radio, "A4F2", 0);
        treksafe::DeviceState s = located();
        uint32_t now = 0;

        b.raiseDistress(treksafe::CAT_INJURY, now, s);
        ok("the call goes out at once", radio.sentCount == 1);

        run(b, s, now, 29000);
        ok("no repeat before the first interval is up", radio.sentCount == 1);
        run(b, s, now, 2000);
        ok("a repeat at thirty seconds", radio.sentCount == 2);

        // Each repeat is a new message with its own sequence number, or a
        // repeater would deduplicate the second one and the mesh would carry
        // the call exactly once however many times the beacon sent it.
        ok("the first call", has(radio.sent[0], "TS1|A4F2|0|0|"));
        ok("the repeat is a distinct message", has(radio.sent[1], "TS1|A4F2|1|0|"));

        run(b, s, now, 59000);
        ok("the interval widens, so no repeat yet", radio.sentCount == 2);
        run(b, s, now, 2000);
        ok("a repeat at a minute", radio.sentCount == 3);

        // It must never give up. A beacon that stops calling before anyone
        // answers leaves a person believing nobody heard them.
        run(b, s, now, 60u * 60u * 1000u);
        ok("it is still calling an hour later", radio.sentCount > 3);
        ok("and still waiting", b.state() == treksafe::BeaconRole::STATE_AWAITING_ACK);
    }

    printf("beacon, the acknowledgement\n");
    {
        fake::Transport radio;
        treksafe::BeaconRole b(radio, "A4F2", 0);
        treksafe::DeviceState s = located();
        uint32_t now = 0;

        b.raiseDistress(treksafe::CAT_INJURY, now, s);
        run(b, s, now, 31000);
        ok("calling, and repeating", radio.sentCount == 2);

        radio.deliver("TS1|BA5E|5|1|ACK|1|A4F2|INC-001");
        run(b, s, now, 100);
        ok("an acknowledgement stops it", b.state() == treksafe::BeaconRole::STATE_ACKNOWLEDGED);
        ok("and it shows that help is coming", b.indication() == treksafe::IND_ACKNOWLEDGED);

        size_t afterAck = radio.sentCount;
        run(b, s, now, 5u * 60u * 1000u);
        ok("no more distress calls once somebody has answered", radio.sentCount == afterAck);

        // It keeps the base station's picture fresh without raising anything.
        run(b, s, now, TREKSAFE_CHECKIN_MS);
        ok("a check-in follows later", radio.sentCount > afterAck);
        ok("and it is a check-in, not another alarm", has(radio.last(), "|OK|1|A4F2|"));
    }

    printf("beacon, acknowledgements that are not ours\n");
    {
        fake::Transport radio;
        treksafe::BeaconRole b(radio, "A4F2", 0);
        treksafe::DeviceState s = located();
        uint32_t now = 0;

        b.raiseDistress(treksafe::CAT_INJURY, now, s);

        radio.deliver("TS1|BA5E|1|1|ACK|1|B117|INC-002");
        radio.deliver("TS1|BA5E|2|1|ACK|2|A4F2|INC-003");
        radio.deliver("TS1|B117|3|0|SOS|1|B117|52.9|-118.1|20|0|M|50");
        radio.deliver("TS2|BA5E|4|1|ACK|1|A4F2|INC-004");
        radio.deliver("not a frame at all");
        run(b, s, now, 200);

        ok("somebody else's acknowledgement does not stop us",
           b.state() == treksafe::BeaconRole::STATE_AWAITING_ACK);

        // And after all that noise, the real one still works.
        radio.deliver("TS1|BA5E|6|1|ACK|1|A4F2|INC-001");
        run(b, s, now, 100);
        ok("ours still does", b.state() == treksafe::BeaconRole::STATE_ACKNOWLEDGED);
    }

    printf("beacon, no fix under canopy\n");
    {
        // The rule this whole product turns on. A beacon that has not acquired
        // a lock still raises the alarm, and the position follows.
        fake::Transport radio;
        treksafe::BeaconRole b(radio, "A4F2", 0);
        treksafe::DeviceState s; // no fix, no clock, no battery
        uint32_t now = 0;

        s.buttonPressed = true;
        run(b, s, now, TREKSAFE_HOLD_MS + 100);
        ok("it still calls for help with nothing to report", radio.sentCount == 1);
        ok("as a distress call with no position, which the contract allows",
           has(radio.last(), "|SOS|1|A4F2|0|0|"));

        // The lock arrives. It must not wait for the next scheduled repeat: the
        // base station is holding an incident marked NO FIX and somebody is
        // deciding where to send people.
        s.hasFix = true;
        s.lat = 52.8734;
        s.lon = -118.0814;
        run(b, s, now, 200);
        ok("a lock arriving sends the position immediately", radio.sentCount == 2);
        ok("carrying it", has(radio.last(), "52.8734|-118.0814"));

        size_t afterFix = radio.sentCount;
        run(b, s, now, 5000);
        ok("and only once, not on every tick from then on", radio.sentCount == afterFix);
    }

    printf("beacon, a radio that will not transmit\n");
    {
        fake::Transport radio;
        treksafe::BeaconRole b(radio, "A4F2", 0);
        treksafe::DeviceState s = located();
        uint32_t now = 0;

        radio.breakRadio();
        b.raiseDistress(treksafe::CAT_INJURY, now, s);
        ok("nothing went out", radio.sentCount == 0);
        ok("it is counted as a failure rather than as a send", b.failedCount() == 1);
        ok("and the beacon says so rather than looking like it is waiting",
           b.indication() == treksafe::IND_SEND_FAILED);
        ok("but it is still in distress, not idle",
           b.state() == treksafe::BeaconRole::STATE_AWAITING_ACK);

        // The critical part: a refused send must not count as a send, or the
        // repeat schedule advances and the person waits on a call nobody heard.
        radio.fixRadio();
        run(b, s, now, 1000);
        ok("once the radio works, the call goes out on the next attempt",
           radio.sentCount >= 1);
    }

    printf("repeater, carrying a call through\n");
    {
        fake::Transport radio;
        treksafe::RepeaterRole r(radio, "FA01", 0, 0xC0FFEE);
        treksafe::DeviceState s;
        s.battPct = 91;
        s.battVolts = 4.1;
        uint32_t now = 1000;

        radio.deliver("TS1|A4F2|7|0|SOS|1|A4F2|52.87|-118.08|12|0|I|86");
        for (int i = 0; i < 10; i++) {
            r.tick(now, s);
            now += 50;
        }
        ok("the call is carried on", r.relayedCount() == 1);
        ok("with the hop count raised, so the survey knows how far it came",
           has(radio.sent[radio.sentCount - 1], "TS1|A4F2|7|1|"));
        ok("and the payload untouched",
           has(radio.sent[radio.sentCount - 1], "SOS|1|A4F2|52.87|-118.08|12|0|I|86"));

        // The one that stops a flood mesh eating itself.
        //
        // Asserted on both counters, not just the relay one. "Did not relay"
        // and "dropped it deliberately" look identical from the outside, and a
        // repeater that had stopped hearing anything at all would pass a test
        // that only checked the first.
        size_t before = r.relayedCount();
        size_t droppedBefore = r.droppedCount();
        radio.deliver("TS1|A4F2|7|0|SOS|1|A4F2|52.87|-118.08|12|0|I|86");
        radio.deliver("TS1|A4F2|7|1|SOS|1|A4F2|52.87|-118.08|12|0|I|86");
        for (int i = 0; i < 10; i++) {
            r.tick(now, s);
            now += 50;
        }
        ok("the same message coming back round is not carried again",
           r.relayedCount() == before);
        ok("and both copies were heard and deliberately dropped, not missed",
           r.droppedCount() == droppedBefore + 2);

        // A frame that has travelled far enough.
        char spent[128];
        snprintf(spent, sizeof(spent), "TS1|B117|1|%d|SOS|1|B117|52.9|-118.1|20|0|M|50",
                 treksafe::kMaxHops);
        radio.deliver(spent);
        before = r.relayedCount();
        droppedBefore = r.droppedCount();
        for (int i = 0; i < 10; i++) {
            r.tick(now, s);
            now += 50;
        }
        ok("a frame out of hops is not carried further", r.relayedCount() == before);
        ok("and that was a decision rather than a frame going missing",
           r.droppedCount() == droppedBefore + 1);
    }

    printf("repeater, reporting its own health\n");
    {
        fake::Transport radio;
        treksafe::RepeaterRole r(radio, "FA01", 0, 0xBEEF);
        treksafe::DeviceState s;
        s.battPct = 64;
        s.battVolts = 3.9;
        uint32_t now = 0;

        r.tick(now, s);
        ok("it reports in as soon as it is up, so it is known to exist",
           radio.sentCount == 1);
        ok("as telemetry carrying its battery", has(radio.last(), "|TEL|1|FA01|64|3.9|"));

        now += 60000;
        r.tick(now, s);
        ok("and not again a minute later", radio.sentCount == 1);

        now += TREKSAFE_TELEMETRY_MS;
        r.tick(now, s);
        ok("but again on the interval", radio.sentCount == 2);

        // Its own frame heard back from a neighbouring repeater.
        radio.deliver(radio.sent[0]);
        size_t before = r.relayedCount();
        for (int i = 0; i < 6; i++) {
            r.tick(now, s);
            now += 50;
        }
        ok("a repeater does not relay its own telemetry back into the mesh",
           r.relayedCount() == before);
    }

    printf("gateway, passing the mesh up the cable\n");
    {
        fake::Transport radio;
        fake::HostPort host;
        treksafe::GatewayRole g(radio, host, "BA5E", 0);
        treksafe::DeviceState s;
        uint32_t now = 0;

        radio.deliver("TS1|A4F2|7|2|SOS|1|A4F2|52.87|-118.08|12|0|I|86", 6.25f, true);
        g.tick(now, s);

        eq("the frame reaches the host with what the radio measured",
           host.firstStarting("RX "), "RX 6.2 - 2 SOS|1|A4F2|52.87|-118.08|12|0|I|86");
        ok("counted as heard", g.heardCount() == 1);
        ok("and the host is told the gateway is alive", host.countStarting("BEAT ") == 1);

        // A reading of zero and no reading at all are different facts, and the
        // coverage survey is built on being able to tell them apart.
        host.writtenCount = 0;
        radio.deliver("TS1|B117|1|0|OK|1|B117|52.9|-118.1|20|0");
        g.tick(now, s);
        eq("no signal reading is reported as absent, never as zero",
           host.firstStarting("RX "), "RX - - 0 OK|1|B117|52.9|-118.1|20|0");

        // The same call arriving by two paths must not look like two calls.
        host.writtenCount = 0;
        size_t droppedBefore = g.droppedCount();
        radio.deliver("TS1|A4F2|7|1|SOS|1|A4F2|52.87|-118.08|12|0|I|86");
        g.tick(now, s);
        ok("a second copy by another path is not passed up again",
           host.countStarting("RX ") == 0);
        ok("and it was heard and dropped, not missed",
           g.droppedCount() == droppedBefore + 1);

        // Nothing disappears without a stated reason.
        host.writtenCount = 0;
        radio.deliver("this is not a TrekSafe frame");
        g.tick(now, s);
        ok("an unreadable frame is reported rather than silently dropped",
           host.countStarting("LOG ") == 1);
    }

    printf("gateway, sending the acknowledgement back\n");
    {
        fake::Transport radio;
        fake::HostPort host;
        treksafe::GatewayRole g(radio, host, "BA5E", 11);
        treksafe::DeviceState s;
        uint32_t now = 0;

        host.deliver("TX ACK|1|A4F2|INC-001");
        g.tick(now, s);
        ok("the acknowledgement goes out on the mesh", radio.sentCount == 1);
        eq("wrapped by the gateway, at zero hops", radio.last(),
           "TS1|BA5E|11|0|ACK|1|A4F2|INC-001");
        ok("counted", g.sentCount() == 1);

        host.deliver("something the gateway does not understand");
        size_t before = radio.sentCount;
        g.tick(now, s);
        ok("noise on the cable is ignored rather than transmitted",
           radio.sentCount == before);

        // D3: the base station has to find out that its reply did not go.
        radio.breakRadio();
        host.writtenCount = 0;
        host.deliver("TX ACK|1|B117|INC-002");
        g.tick(now, s);
        ok("a refused transmission is counted as a failure", g.failedCount() == 1);
        ok("and the base station is told, because it thinks it has replied",
           host.countStarting("LOG ") == 1);
    }

    printf("gateway, the heartbeat\n");
    {
        // The console cannot tell a calm night from a dead radio without this.
        fake::Transport radio;
        fake::HostPort host;
        treksafe::GatewayRole g(radio, host, "BA5E", 0);
        treksafe::DeviceState s;
        uint32_t now = 0;

        g.tick(now, s);
        ok("it says it is alive immediately, not after a full interval",
           host.countStarting("BEAT ") == 1);

        now += 1000;
        g.tick(now, s);
        ok("and not on every tick, which would be noise", host.countStarting("BEAT ") == 1);

        now += TREKSAFE_GATEWAY_BEAT_MS;
        g.tick(now, s);
        ok("but again on the interval, with no mesh traffic needed to prompt it",
           host.countStarting("BEAT ") == 2);
    }

    printf("a node that cannot identify itself\n");
    {
        // Found by writing these tests, which is the point of them. A gateway
        // named "GW01" transmitted nothing at all: G and W are not hex, so the
        // contract rejects the id, buildLink refuses to attribute a frame to it
        // and every send fails. In the field that is a base station whose
        // acknowledgements silently never leave, which is the precise failure
        // D3 exists to prevent.
        //
        // The behaviour below is correct and deliberate: a frame nobody can
        // attribute cannot be deduplicated, and a flood mesh rebroadcasts such
        // a frame forever. Refusing to send it is right. What was missing is
        // anybody saying so, which is why main.cpp now checks the device id at
        // startup and refuses to run the role rather than transmitting nothing
        // and looking healthy.
        fake::Transport radio;
        fake::HostPort host;
        treksafe::GatewayRole bad(radio, host, "GW01", 0);
        treksafe::DeviceState s;

        host.deliver("TX ACK|1|A4F2|INC-001");
        bad.tick(0, s);
        ok("a node with a non-hex id transmits nothing", radio.sentCount == 0);
        ok("counted as a failure rather than passing quietly", bad.failedCount() == 1);
        ok("and it says so on the cable", host.countStarting("LOG ") == 1);

        treksafe::BeaconRole badBeacon(radio, "OOPS", 0);
        badBeacon.raiseDistress(treksafe::CAT_INJURY, 0, s);
        ok("a beacon with a non-hex id cannot call for help either",
           radio.sentCount == 0 && badBeacon.failedCount() == 1);
        ok("and shows the failure rather than looking like it is waiting",
           badBeacon.indication() == treksafe::IND_SEND_FAILED);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
