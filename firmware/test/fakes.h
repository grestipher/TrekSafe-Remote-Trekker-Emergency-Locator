// Test doubles for the transport seam.
//
// These are the reason the roles are worth the interface. With them, the whole
// of a beacon's life, button held to acknowledgement received, runs inside a
// compiler in about a microsecond, and the assertions are on exactly what went
// on the air. The firmware this replaced could not do any of that; its own
// header said that none of the glue could be tested without hardware, and so
// none of it was.

#pragma once

#include "../src/transport.h"

#include <stdio.h>
#include <string.h>

namespace fake {

// Enough for any test here, and a fixed size so a runaway role that transmits
// on every tick is caught by an assertion rather than by the machine running
// out of memory.
const size_t kMaxFrames = 64;

// A radio that goes nowhere. Records what was sent, replays what you queue.
class Transport : public treksafe::Transport {
  public:
    Transport() : sentCount(0), inbox_(0), inboxCount_(0), working_(true) {}

    bool send(const char *frame) override
    {
        if (!working_)
            return false;
        if (sentCount >= kMaxFrames) {
            printf("  FAIL fake transport overflowed: a role is transmitting far more "
                   "than the test expected\n");
            return false;
        }
        snprintf(sent[sentCount], sizeof(sent[0]), "%s", frame);
        sentCount++;
        return true;
    }

    bool receive(char *out, size_t cap, treksafe::LinkQuality &quality) override
    {
        if (inbox_ >= inboxCount_)
            return false;
        snprintf(out, cap, "%s", inbox[inbox_]);
        quality = inboxQuality[inbox_];
        inbox_++;
        return true;
    }

    // Queue a frame for the role to receive on its next tick.
    void deliver(const char *frame, float snr = 0, bool hasSnr = false)
    {
        if (inboxCount_ >= kMaxFrames)
            return;
        snprintf(inbox[inboxCount_], sizeof(inbox[0]), "%s", frame);
        treksafe::LinkQuality q;
        q.hasSnr = hasSnr;
        q.snr = snr;
        inboxQuality[inboxCount_] = q;
        inboxCount_++;
    }

    // A radio that has failed, so the send path's refusal can be exercised.
    void breakRadio() { working_ = false; }
    void fixRadio() { working_ = true; }

    void forget()
    {
        sentCount = 0;
        inbox_ = inboxCount_ = 0;
    }

    // The most recent frame sent, or "" if nothing has been.
    const char *last() const { return sentCount ? sent[sentCount - 1] : ""; }

    char sent[kMaxFrames][treksafe::kLinkFrameMax];
    size_t sentCount;

  private:
    char inbox[kMaxFrames][treksafe::kLinkFrameMax];
    treksafe::LinkQuality inboxQuality[kMaxFrames];
    size_t inbox_;
    size_t inboxCount_;
    bool working_;
};

// A cable to a base station that is not there.
class HostPort : public treksafe::HostPort {
  public:
    HostPort() : writtenCount(0), inbox_(0), inboxCount_(0) {}

    void writeLine(const char *line) override
    {
        if (writtenCount >= kMaxFrames)
            return;
        snprintf(written[writtenCount], sizeof(written[0]), "%s", line);
        writtenCount++;
    }

    bool readLine(char *out, size_t cap) override
    {
        if (inbox_ >= inboxCount_)
            return false;
        snprintf(out, cap, "%s", inbox[inbox_]);
        inbox_++;
        return true;
    }

    void deliver(const char *line)
    {
        if (inboxCount_ >= kMaxFrames)
            return;
        snprintf(inbox[inboxCount_], sizeof(inbox[0]), "%s", line);
        inboxCount_++;
    }

    // How many written lines begin with this verb. Written lines are checked by
    // verb rather than by index because the heartbeat is interleaved with
    // everything else and its exact position is not what any test means.
    size_t countStarting(const char *prefix) const
    {
        size_t n = 0;
        for (size_t i = 0; i < writtenCount; i++)
            if (strncmp(written[i], prefix, strlen(prefix)) == 0)
                n++;
        return n;
    }

    // The first written line beginning with this verb, or "" if there is none.
    const char *firstStarting(const char *prefix) const
    {
        for (size_t i = 0; i < writtenCount; i++)
            if (strncmp(written[i], prefix, strlen(prefix)) == 0)
                return written[i];
        return "";
    }

    char written[kMaxFrames][treksafe::kFrameMax + 48];
    size_t writtenCount;

  private:
    char inbox[kMaxFrames][treksafe::kFrameMax + 48];
    size_t inbox_;
    size_t inboxCount_;
};

} // namespace fake
