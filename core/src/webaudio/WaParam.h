#pragma once
#include <array>
#include <cstddef>

namespace sl {

// Web Audio AudioParam timeline, restricted to the two methods zyn uses:
// setValueAtTime and linearRampToValueAtTime.
//
// Fixed capacity so it never allocates on the audio thread. zyn schedules at
// most five events per envelope -- one setValueAtTime plus four ramps.
//
// Events are assumed to be scheduled in non-decreasing time order, which is
// what Z.adsr and Z.adsrSustain always do.
class WaParam {
public:
    static constexpr size_t kMaxEvents = 8;

    void reset(double v);
    void setValueAtTime(double v, double t);
    void linearRampToValueAtTime(double v, double t);
    void cancelScheduledValues(double t);

    double valueAt(double t) const;

    // Incremental evaluation for the render loop. beginStepping() resolves the
    // first segment; nextValue() then returns one sample and advances. This
    // replaces a timeline search plus a division per call with an add -- a
    // voice reads three or four envelopes per oscillator per sample, so at
    // polyphony that was about a third of the whole voice path.
    void beginStepping(double sampleRate);
    double nextValue();
    double staticValue() const { return static_; }
    bool hasEvents() const { return count_ > 0; }

    // Time of the last scheduled event, or 0 when there are none.
    double endTime() const { return count_ ? events_[count_ - 1].time : 0.0; }

private:
    enum class Kind { SetValue, LinearRamp };
    struct Event { Kind kind; double value; double time; };

    double static_ = 0.0;
    std::array<Event, kMaxEvents> events_{};
    size_t count_ = 0;

    // Cursor for the common case of monotonically advancing render time.
    // Mutable because valueAt is logically const.
    mutable size_t cursor_ = 0;
    mutable double lastQuery_ = 0.0;

    // Stepping state.
    double sr_ = 48000.0;
    size_t stepSeg_ = 0;
    long long stepRemaining_ = -1;   // negative means "hold forever"
    double stepValue_ = 0.0;
    double stepDelta_ = 0.0;
    void enterSegment(size_t i);
};

} // namespace sl
