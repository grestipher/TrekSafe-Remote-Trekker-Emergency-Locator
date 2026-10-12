// Host tests for the payload contract. No board, no toolchain beyond a C++
// compiler, no dependency.
//
//   c++ -std=c++11 -Wall -Wextra -o /tmp/test_contract \
//       firmware/test/test_contract.cpp firmware/src/contract.cpp && /tmp/test_contract
//
// The parser and the frame builder are the one place where a bug means a real
// distress call is malformed or an acknowledgement is missed, so they are
// tested away from the radio where the failures are visible.

#include "../src/contract.h"

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
        treksafe::deviceIdFromSerial(0xDEADA4F2u, id);
        eq("low half of the hardware serial, uppercase hex", id, "A4F2");
        treksafe::deviceIdFromSerial(0x00000001u, id);
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

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
