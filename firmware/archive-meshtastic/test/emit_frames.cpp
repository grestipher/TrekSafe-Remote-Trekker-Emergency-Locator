// Prints the frames the firmware would transmit, so they can be pasted into
// the console without a radio.
//
//   c++ -std=c++11 -o /tmp/emit_frames \
//       firmware/test/emit_frames.cpp firmware/src/TrekSafeFrames.cpp && /tmp/emit_frames
//
// This exists because there is one board. Until there is a second one, the
// beacon and the base station cannot both be on the air at once, so the only
// honest way to check that the two ends agree on the contract is to take the
// exact bytes one would send and put them into the other by hand. Paste a line
// into the raw box in the console, or send it as a text message from the
// Meshtastic phone app once a bridge exists.

#include "../src/TrekSafeFrames.h"

#include <stdio.h>

static treksafe::Distress beacon()
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

static void emit(const char *what, const char *frame)
{
    printf("%-34s %s\n", what, frame);
}

int main()
{
    char buf[treksafe::kFrameMax];
    treksafe::Distress d;

    d = beacon();
    treksafe::buildSos(buf, sizeof(buf), d);
    emit("distress, everything known", buf);

    d = beacon();
    d.hasFix = false;
    d.epoch = treksafe::kClockNotSet;
    treksafe::buildSos(buf, sizeof(buf), d);
    emit("distress, under canopy, no lock", buf);

    d = beacon();
    d.cat = treksafe::CAT_INJURY;
    d.accM = treksafe::kUnknownInt;
    treksafe::buildSos(buf, sizeof(buf), d);
    emit("distress, accuracy unknown", buf);

    d = beacon();
    d.devid = 0;
    d.hasFix = false;
    d.accM = treksafe::kUnknownInt;
    d.epoch = treksafe::kClockNotSet;
    d.battPct = treksafe::kUnknownInt;
    d.cat = 0;
    treksafe::buildSos(buf, sizeof(buf), d);
    emit("distress, nothing known at all", buf);

    d = beacon();
    treksafe::buildOk(buf, sizeof(buf), d);
    emit("check-in", buf);

    treksafe::buildTel(buf, sizeof(buf), "B117", 64, 4.02, -3.54);
    emit("telemetry from a repeater", buf);

    return 0;
}
