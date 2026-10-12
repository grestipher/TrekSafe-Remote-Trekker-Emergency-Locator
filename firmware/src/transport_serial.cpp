#include "transport_serial.h"

#include <string.h>

namespace treksafe {

namespace {
const char *const kPrefix = "AIR ";
const size_t kPrefixLen = 4;
} // namespace

SerialTransport::SerialTransport(Stream &stream) : stream_(stream), len_(0), overflowed_(false)
{
    buffer_[0] = '\0';
}

bool SerialTransport::send(const char *frame)
{
    if (!frame || !*frame)
        return false;

    // print rather than println for the prefix and frame, then one newline, so
    // a frame is written as a single line even if something else is sharing
    // this port.
    stream_.print(kPrefix);
    stream_.print(frame);
    stream_.print('\n');
    return true;
}

bool SerialTransport::receive(char *out, size_t cap, LinkQuality &quality)
{
    // A cable measured nothing. Left as the default, which reports no readings
    // at all rather than zeroes: zero dB is a real marginal link and silence
    // about it is not, and the coverage survey depends on telling them apart.
    quality = LinkQuality();

    while (stream_.available() > 0) {
        int c = stream_.read();
        if (c < 0)
            break;

        if (c != '\n' && c != '\r') {
            if (len_ + 1 < sizeof(buffer_)) {
                buffer_[len_++] = (char)c;
            } else {
                // Too long to be one of ours. Remember that, and keep consuming
                // to the end of the line so its tail is not read as the start
                // of the next frame.
                overflowed_ = true;
            }
            continue;
        }

        // End of a line. Take a copy of what we have and reset for the next one
        // before deciding anything, so every path leaves the buffer clean.
        buffer_[len_] = '\0';
        size_t len = len_;
        bool bad = overflowed_;
        len_ = 0;
        overflowed_ = false;

        if (bad || len <= kPrefixLen)
            continue;
        if (strncmp(buffer_, kPrefix, kPrefixLen) != 0)
            continue; // not traffic: a log line, or noise on the cable

        snprintf(out, cap, "%s", buffer_ + kPrefixLen);
        return true;
    }

    return false;
}

} // namespace treksafe
