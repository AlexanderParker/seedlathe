#pragma once
#include "sl/Instrument.h"

namespace sl {

// Web Audio BiquadFilterNode: RBJ cookbook coefficients with Blink's clamping.
//
// Coefficients are recomputed per sample (a-rate) because zyn drives both
// cutoff and Q from envelopes on every note, sweeping cutoff across the entire
// 0 -> 20000 Hz span. The degenerate endpoints are therefore not edge cases,
// they run constantly, and matching Blink's behaviour there is what keeps the
// port sounding right rather than merely plausible.
class WaBiquad {
public:
    void prepare(double sampleRate);
    void reset();

    void setCoefficients(FilterType type, double freqHz, double q, double gainDb);
    double process(double x);

    // |H(e^jw)| from the stored coefficients, so tests can check the response
    // analytically instead of by ear.
    double magnitudeAt(double freqHz) const;

private:
    void setPassthrough();
    void setSilence();

    double sampleRate_ = 48000.0;
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

} // namespace sl
