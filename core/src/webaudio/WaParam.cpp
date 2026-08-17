#include "webaudio/WaParam.h"

namespace sl {

void WaParam::reset(double v) {
    static_ = v;
    count_ = 0;
    cursor_ = 0;
    lastQuery_ = 0.0;
}

void WaParam::setValueAtTime(double v, double t) {
    if (count_ < kMaxEvents) events_[count_++] = {Kind::SetValue, v, t};
}

void WaParam::linearRampToValueAtTime(double v, double t) {
    if (count_ < kMaxEvents) events_[count_++] = {Kind::LinearRamp, v, t};
}

void WaParam::cancelScheduledValues(double t) {
    while (count_ > 0 && events_[count_ - 1].time >= t) --count_;
    if (cursor_ >= count_) cursor_ = count_ ? count_ - 1 : 0;
}

double WaParam::valueAt(double t) const {
    if (count_ == 0) return static_;

    // Before the first event the parameter holds its static value. zyn always
    // schedules setValueAtTime first, so a ramp never leads.
    if (t <= events_[0].time) {
        cursor_ = 0;
        lastQuery_ = t;
        return events_[0].kind == Kind::SetValue && t == events_[0].time
                   ? events_[0].value
                   : static_;
    }

    // Advance or rewind the cursor to the last event at or before t.
    if (t < lastQuery_) cursor_ = 0;
    lastQuery_ = t;
    while (cursor_ + 1 < count_ && events_[cursor_ + 1].time <= t) ++cursor_;
    while (cursor_ > 0 && events_[cursor_].time > t) --cursor_;

    const size_t i = cursor_;
    if (i + 1 >= count_) return events_[i].value;   // holds after the last event

    const Event& prev = events_[i];
    const Event& next = events_[i + 1];

    // A setValueAtTime is a step, not a ramp: hold the previous value until it.
    if (next.kind == Kind::SetValue) return prev.value;

    const double span = next.time - prev.time;
    if (span <= 0.0) return next.value;
    return prev.value + (next.value - prev.value) * ((t - prev.time) / span);
}

} // namespace sl
