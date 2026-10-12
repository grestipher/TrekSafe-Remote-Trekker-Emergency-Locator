#include "role_gateway.h"

#include <stdio.h>
#include <string.h>

namespace treksafe {

namespace {

// A signal reading, or "-" for no reading at all.
//
// The distinction is the whole reason this helper exists rather than a printf
// with a %.1f in it. Zero decibels of signal to noise is a real, marginal link.
// No measurement is silence about the link. The coverage survey treats them
// completely differently and it can only do that if the gateway keeps them
// apart on the wire.
void writeSnr(char *out, size_t cap, const LinkQuality &q)
{
    if (q.hasSnr)
        snprintf(out, cap, "%.1f", (double)q.snr);
    else
        snprintf(out, cap, "-");
}

void writeRssi(char *out, size_t cap, const LinkQuality &q)
{
    if (q.hasRssi)
        snprintf(out, cap, "%d", q.rssi);
    else
        snprintf(out, cap, "-");
}

// Longest host line: the verb, three readings, and a payload.
const size_t kHostLineMax = kFrameMax + 48;

} // namespace

GatewayRole::GatewayRole(Transport &radio, HostPort &host, const char *devid, uint16_t startSeq)
    : radio_(radio), host_(host), seq_(startSeq), indication_(IND_LISTENING), lastBeatMs_(0),
      beatenOnce_(false), heard_(0), sent_(0), dropped_(0), failed_(0)
{
    snprintf(devid_, sizeof(devid_), "%s", devid ? devid : "");
}

void GatewayRole::pollRadio(uint32_t nowMs)
{
    char line[kLinkFrameMax];
    LinkQuality quality;

    while (radio_.receive(line, sizeof(line), quality)) {
        LinkFrame f;
        if (!parseLink(line, f)) {
            // Unreadable, or somebody else's protocol on the same frequency.
            // Reported rather than swallowed: the base station's rule is that
            // no packet disappears without a stated reason, and this is the
            // only place that can state this one.
            char note[kHostLineMax];
            snprintf(note, sizeof(note), "LOG heard a frame this gateway could not read");
            host_.writeLine(note);
            dropped_++;
            continue;
        }

        // Our own transmission heard back, or a repeater's copy of it.
        if (strcmp(f.origin, devid_) == 0) {
            dropped_++;
            continue;
        }

        if (!seen_.isNew(f.origin, f.seq, nowMs)) {
            // The same call arriving by a second path through the mesh. See the
            // note on seen_ in the header: without this the console counts the
            // copies as a beacon calling again because nobody answered.
            dropped_++;
            continue;
        }

        char snr[16], rssi[16];
        writeSnr(snr, sizeof(snr), quality);
        writeRssi(rssi, sizeof(rssi), quality);

        char out[kHostLineMax];
        int n = snprintf(out, sizeof(out), "RX %s %s %u %s", snr, rssi, (unsigned)f.hops,
                         f.payload);
        if (n < 0 || (size_t)n >= sizeof(out)) {
            // A payload too long to report. Said out loud rather than
            // truncated, because a truncated distress call reaching the console
            // would be parsed as a malformed one and the reason would be lost.
            host_.writeLine("LOG heard a frame too long to pass to the host");
            dropped_++;
            continue;
        }

        host_.writeLine(out);
        heard_++;
    }
}

void GatewayRole::pollHost(uint32_t nowMs)
{
    (void)nowMs;

    char line[kHostLineMax];
    while (host_.readLine(line, sizeof(line))) {
        // The only thing the base station ever asks for is a transmission.
        // Anything else is noise on the cable and is ignored rather than
        // guessed at.
        if (strncmp(line, "TX ", 3) != 0)
            continue;

        const char *payload = line + 3;
        if (*payload == '\0')
            continue;

        char frame[kLinkFrameMax];
        if (buildLink(frame, sizeof(frame), devid_, seq_, 0, payload) == 0) {
            host_.writeLine("LOG could not build a frame for that payload, nothing was sent");
            failed_++;
            continue;
        }

        if (radio_.send(frame)) {
            seq_++;
            sent_++;
        } else {
            // The base station has to know. It is holding an incident it
            // believes it has replied to, and D3 says a beacon pressed into
            // silence is the failure this whole product exists to prevent.
            host_.writeLine("LOG the radio refused to transmit, nothing went out");
            failed_++;
        }
    }
}

void GatewayRole::beat(uint32_t nowMs)
{
    if (beatenOnce_ && !elapsedAtLeast(nowMs, lastBeatMs_, TREKSAFE_GATEWAY_BEAT_MS))
        return;

    char out[48];
    snprintf(out, sizeof(out), "BEAT %lu", (unsigned long)nowMs);
    host_.writeLine(out);
    lastBeatMs_ = nowMs;
    beatenOnce_ = true;
}

void GatewayRole::tick(uint32_t nowMs, const DeviceState &state)
{
    // A gateway has mains power and a cable. It reports nothing about itself
    // beyond being alive, which is what BEAT is for.
    (void)state;

    pollRadio(nowMs);
    pollHost(nowMs);
    beat(nowMs);

    indication_ = IND_LISTENING;
}

} // namespace treksafe
