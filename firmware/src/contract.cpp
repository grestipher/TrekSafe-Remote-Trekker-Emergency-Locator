#include "contract.h"

#include <stdio.h>
#include <string.h>

namespace treksafe {

namespace {

const char *kHex = "0123456789ABCDEF";

// Optional integer field. Anything unknown becomes an empty field rather than
// a made up number, because a fabricated battery level or accuracy reading is
// worse than an admitted gap.
void optInt(char *buf, size_t cap, int v)
{
    if (v < 0) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, cap, "%d", v);
}

void optU32(char *buf, size_t cap, uint32_t v)
{
    if (v == kClockNotSet) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, cap, "%lu", (unsigned long)v);
}

// Four decimal places is about eleven metres, which is finer than any fix this
// hardware will produce under canopy, and it keeps the frame near 45 bytes.
void coord(char *latBuf, size_t latCap, char *lonBuf, size_t lonCap, const Distress &d)
{
    if (!d.hasFix) {
        // The contract defines 0|0 as no fix yet. This is a valid distress
        // message, not an error, and the console alarms on it.
        snprintf(latBuf, latCap, "0");
        snprintf(lonBuf, lonCap, "0");
        return;
    }
    snprintf(latBuf, latCap, "%.4f", d.lat);
    snprintf(lonBuf, lonCap, "%.4f", d.lon);
}

char safeCategory(char c)
{
    switch (c) {
    case CAT_MEDICAL:
    case CAT_INJURY:
    case CAT_LOST:
    case CAT_UNSPECIFIED:
        return c;
    default:
        // An unrecognised category degrades to unspecified. It never stops the
        // frame going out.
        return CAT_UNSPECIFIED;
    }
}

const char *safeDevId(const char *s)
{
    return (s && isValidDevId(s)) ? s : "";
}

char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Splits on the pipe, in place, into at most max fields. Returns the count.
int split(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p && n < max) {
        if (*p == '|') {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }
    return n;
}

void trim(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            s[--len] = '\0';
        else
            break;
    }
}

// Strict small integer parse. Returns -1 for anything that is not entirely
// digits, so a schema field of "1x" is not quietly read as 1.
int atoiSafe(const char *s)
{
    if (!s || !*s)
        return -1;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
        if (v > 9999)
            return -1;
    }
    return v;
}

size_t finish(char *out, size_t cap, int written)
{
    if (written < 0 || (size_t)written >= cap) {
        if (cap > 0)
            out[0] = '\0';
        return 0;
    }
    return (size_t)written;
}

} // namespace

void deviceIdFromSerial(uint32_t serial, char *out)
{
    uint16_t low = (uint16_t)(serial & 0xFFFF);
    out[0] = kHex[(low >> 12) & 0xF];
    out[1] = kHex[(low >> 8) & 0xF];
    out[2] = kHex[(low >> 4) & 0xF];
    out[3] = kHex[low & 0xF];
    out[4] = '\0';
}

bool isValidDevId(const char *s)
{
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n < 2 || n > 8)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool digit = (c >= '0' && c <= '9');
        bool hex = (c >= 'A' && c <= 'F');
        if (!digit && !hex)
            return false;
    }
    return true;
}

size_t buildSos(char *out, size_t cap, const Distress &d)
{
    char latBuf[16], lonBuf[16], accBuf[12], epochBuf[16], battBuf[8];
    coord(latBuf, sizeof(latBuf), lonBuf, sizeof(lonBuf), d);
    optInt(accBuf, sizeof(accBuf), d.accM);
    optU32(epochBuf, sizeof(epochBuf), d.epoch);
    optInt(battBuf, sizeof(battBuf), d.battPct);

    int n = snprintf(out, cap, "SOS|%d|%s|%s|%s|%s|%s|%c|%s", kSchema, safeDevId(d.devid), latBuf, lonBuf,
                     accBuf, epochBuf, safeCategory(d.cat), battBuf);
    return finish(out, cap, n);
}

size_t buildOk(char *out, size_t cap, const Distress &d)
{
    char latBuf[16], lonBuf[16], accBuf[12], epochBuf[16];
    coord(latBuf, sizeof(latBuf), lonBuf, sizeof(lonBuf), d);
    optInt(accBuf, sizeof(accBuf), d.accM);
    optU32(epochBuf, sizeof(epochBuf), d.epoch);

    int n = snprintf(out, cap, "OK|%d|%s|%s|%s|%s|%s", kSchema, safeDevId(d.devid), latBuf, lonBuf, accBuf,
                     epochBuf);
    return finish(out, cap, n);
}

size_t buildTel(char *out, size_t cap, const char *devid, int battPct, double volts, double snr)
{
    char battBuf[8];
    optInt(battBuf, sizeof(battBuf), battPct);

    int n = snprintf(out, cap, "TEL|%d|%s|%s|%.1f|%.1f", kSchema, safeDevId(devid), battBuf, volts, snr);
    return finish(out, cap, n);
}

bool parseAck(const char *line, const char *ourDevId, Ack &out)
{
    if (!line || !ourDevId)
        return false;

    char buf[kFrameMax];
    size_t len = strlen(line);
    if (len >= sizeof(buf))
        return false;
    memcpy(buf, line, len + 1);
    trim(buf);

    char *fields[8] = {0};
    int n = split(buf, fields, 8);
    if (n < 3)
        return false;

    for (int i = 0; i < n; i++)
        trim(fields[i]);

    for (char *p = fields[0]; *p; p++)
        *p = upper(*p);
    if (strcmp(fields[0], "ACK") != 0)
        return false;

    // A schema we do not speak is not ours to act on. Unlike a distress frame,
    // discarding this is safe: the base station will keep trying, and telling
    // someone they were heard on the strength of a frame we cannot read would
    // be worse than staying quiet.
    if (atoiSafe(fields[1]) != kSchema)
        return false;

    for (char *p = fields[2]; *p; p++)
        *p = upper(*p);

    char ours[kDevIdMax];
    size_t ourLen = strlen(ourDevId);
    if (ourLen == 0 || ourLen >= sizeof(ours))
        return false;
    memcpy(ours, ourDevId, ourLen + 1);
    for (char *p = ours; *p; p++)
        *p = upper(*p);

    if (strcmp(fields[2], ours) != 0)
        return false;

    memset(&out, 0, sizeof(out));
    snprintf(out.devid, sizeof(out.devid), "%s", ours);

    // An acknowledgement with no incident id still means a human has seen the
    // call, which is the whole point of the return path. Take it.
    if (n >= 4 && fields[3][0] != '\0') {
        snprintf(out.incident, sizeof(out.incident), "%s", fields[3]);
        out.hasIncident = true;
    }
    return true;
}
} // namespace treksafe
