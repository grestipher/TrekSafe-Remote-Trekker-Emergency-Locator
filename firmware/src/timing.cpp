#include "timing.h"

namespace treksafe {

RepeatSchedule::RepeatSchedule(const uint32_t *stepsMs, size_t count)
    : steps_(stepsMs), count_(count), step_(0), lastMs_(0), waitMs_(0), sentAnything_(false)
{
    restart();
}

void RepeatSchedule::restart()
{
    step_ = 0;
    lastMs_ = 0;
    sentAnything_ = false;
    waitMs_ = count_ ? steps_[0] : 0;
}

void RepeatSchedule::sent(uint32_t nowMs)
{
    if (!count_)
        return;
    // The wait after this send comes from the step this send belongs to, then
    // the schedule advances. Advancing first would skip the first entry.
    waitMs_ = steps_[step_];
    if (step_ + 1 < count_)
        step_++;
    lastMs_ = nowMs;
    sentAnything_ = true;
}

bool RepeatSchedule::due(uint32_t nowMs) const
{
    if (!sentAnything_ || !count_)
        return false;

    // Signed difference, deliberately.
    //
    // Unsigned subtraction survives the millis() wrap at about 49 days, which a
    // beacon left on all season needs. What it does not survive is a timestamp
    // slightly in the future: nowMs - lastMs_ underflows to about four billion,
    // which reads as an enormous elapsed time and fires a repeat instantly.
    // That is not hypothetical. It happened on the first run against real
    // Meshtastic: a tick captured now, the send inside that same tick recorded
    // a millis() a millisecond later, and the beacon transmitted the distress
    // call twice two milliseconds apart.
    //
    // A signed difference is correct for both, as long as intervals stay well
    // under 24 days. The longest wait here is five minutes.
    return elapsedAtLeast(nowMs, lastMs_, waitMs_);
}

HoldToFire::HoldToFire(uint32_t holdMs) : holdMs_(holdMs), down_(false), fired_(false), downAt_(0) {}

void HoldToFire::reset()
{
    down_ = false;
    fired_ = false;
    downAt_ = 0;
}

bool HoldToFire::update(bool pressed, uint32_t nowMs)
{
    if (!pressed) {
        down_ = false;
        fired_ = false;
        return false;
    }
    if (!down_) {
        down_ = true;
        fired_ = false;
        downAt_ = nowMs;
        return false;
    }
    if (fired_)
        return false;
    if (nowMs - downAt_ >= holdMs_) {
        fired_ = true;
        return true;
    }
    return false;
}

uint32_t HoldToFire::heldMs(uint32_t nowMs) const
{
    if (!down_ || fired_)
        return 0;
    return nowMs - downAt_;
}

} // namespace treksafe
