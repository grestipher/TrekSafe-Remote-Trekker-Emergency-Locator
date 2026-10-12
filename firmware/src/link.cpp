#include "link.h"
#include "timing.h"

#include <stdio.h>
#include <string.h>

namespace treksafe {

namespace {

char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// Strict unsigned parse, bounded by max. Returns -1 for anything that is not
// entirely digits or does not fit.
//
// Same reasoning as the contract's atoiSafe and the console's strictInt: a
// sequence number read out of "41x" as 41 would put two different messages
// under the same deduplication key, and one of them would then vanish. Anything
// we cannot read exactly is something we do not have.
long parseUnsigned(const char *s, long max)
{
    if (!s || !*s)
        return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
        if (v > max)
            return -1;
    }
    return v;
}

} // namespace

size_t buildLink(char *out, size_t cap, const char *origin, uint16_t seq, uint8_t hops,
                 const char *payload)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';

    // See the header: an unattributable frame cannot be deduplicated, and a
    // frame that cannot be deduplicated is one a flood mesh will rebroadcast
    // forever. Refusing to build it is the only place that can be stopped.
    if (!isValidDevId(origin))
        return 0;
    if (!payload || !*payload)
        return 0;

    int n = snprintf(out, cap, "%s|%s|%u|%u|%s", kLinkTag, origin, (unsigned)seq, (unsigned)hops,
                     payload);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool parseLink(char *line, LinkFrame &out)
{
    if (!line)
        return false;

    // Only the first four fields belong to the link layer. The payload is
    // everything after the fourth pipe, kept whole and untouched, because it is
    // itself pipe delimited and splitting it here would be this layer reading a
    // payload it has no business understanding.
    char *field[4];
    char *p = line;
    for (int i = 0; i < 4; i++) {
        field[i] = p;
        char *bar = strchr(p, '|');
        if (!bar)
            return false; // fewer than five fields, so there is no payload
        *bar = '\0';
        p = bar + 1;
    }
    if (*p == '\0')
        return false; // an empty payload is not a frame worth relaying

    for (char *c = field[0]; *c; c++)
        *c = upper(*c);
    if (strcmp(field[0], kLinkTag) != 0)
        return false;

    for (char *c = field[1]; *c; c++)
        *c = upper(*c);
    if (!isValidDevId(field[1]))
        return false;

    long seq = parseUnsigned(field[2], 0xFFFF);
    if (seq < 0)
        return false;

    // A hop count above the limit is not a frame to carry further. It is also
    // not necessarily an attack or a bug: a frame can legitimately arrive at
    // exactly kMaxHops, and the caller decides what to do about that. Anything
    // beyond what the field can hold is refused outright.
    long hops = parseUnsigned(field[3], 0xFF);
    if (hops < 0)
        return false;

    memset(&out, 0, sizeof(out));
    snprintf(out.origin, sizeof(out.origin), "%s", field[1]);
    out.seq = (uint16_t)seq;
    out.hops = (uint8_t)hops;
    out.payload = p;
    return true;
}

SeenCache::SeenCache(uint32_t forgetAfterMs) : next_(0), forgetAfterMs_(forgetAfterMs)
{
    memset(entries_, 0, sizeof(entries_));
}

bool SeenCache::isNew(const char *origin, uint16_t seq, uint32_t nowMs)
{
    // Nothing to remember it under. Treated as new so it is never dropped; the
    // caller is responsible for not rebroadcasting it. Counting one call twice
    // is a nuisance, losing one is not, which is the same trade the bridge
    // makes for a packet with no id.
    if (!origin || !*origin)
        return true;

    for (size_t i = 0; i < TREKSAFE_SEEN_CAPACITY; i++) {
        Entry &e = entries_[i];
        if (!e.used)
            continue;

        // An entry old enough to forget is freed rather than merely ignored, so
        // a quiet mesh does not hold a full ring of stale keys and evict live
        // ones to make room.
        if (forgetAfterMs_ && elapsedAtLeast(nowMs, e.at, forgetAfterMs_)) {
            e.used = false;
            continue;
        }

        if (e.seq == seq && strcmp(e.origin, origin) == 0) {
            // Deliberately not refreshed. The entry ages from when the message
            // was first heard, so a beacon repeating the same sequence number
            // cannot hold a key alive indefinitely and shadow a later reboot
            // that lands on it.
            return false;
        }
    }

    // Prefer a free slot, and only overwrite the oldest when the ring is full.
    // Without the first pass, a cache with holes punched in it by forgetting
    // would still evict a live entry on every insert.
    Entry *slot = 0;
    for (size_t i = 0; i < TREKSAFE_SEEN_CAPACITY; i++) {
        if (!entries_[i].used) {
            slot = &entries_[i];
            break;
        }
    }
    if (!slot) {
        slot = &entries_[next_];
        next_ = (next_ + 1) % TREKSAFE_SEEN_CAPACITY;
    }

    snprintf(slot->origin, sizeof(slot->origin), "%s", origin);
    slot->seq = seq;
    slot->at = nowMs;
    slot->used = true;
    return true;
}

size_t SeenCache::remembered() const
{
    size_t n = 0;
    for (size_t i = 0; i < TREKSAFE_SEEN_CAPACITY; i++)
        if (entries_[i].used)
            n++;
    return n;
}

} // namespace treksafe
