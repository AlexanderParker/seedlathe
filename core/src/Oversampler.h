#pragma once
#include <cstddef>
#include <vector>

namespace sl {

// Decimates an oversampled render back down to the host's rate.
//
// The synth has no audio input, so only the downward half of oversampling is
// needed: the engine is prepared at N times the host rate, renders N times as
// many frames, and this filters and throws away the extras.
//
// Half-band FIR stages rather than a biquad cascade. An 8th-order Butterworth
// placed low enough to reject the images is already audibly dull at the top,
// and placed high enough to keep the top it rejects almost nothing -- around
// 8 dB at Nyquist. A 63-tap Kaiser-windowed sinc gets past 70 dB of stopband
// for a few million multiplies a second, which against the cost of rendering
// the extra samples in the first place is not worth optimising.
//
// Note that oversampling is a DEVIATION from zyn: the browser runs the graph at
// the context rate, so band limiting, filter coefficients and the compressor's
// timing all differ at 2x and 4x. It is off by default for exactly that reason.
class Decimator {
public:
    // factor must be 1, 2 or 4. 1 makes process() a straight copy.
    void prepare(int factor);
    void reset();

    int factor() const { return factor_; }

    // `in` holds frames * factor samples; `out` receives frames.
    void process(const float* in, float* out, int frames);

    // Samples of group delay introduced, at the OUTPUT rate. Reported to the
    // host so it can compensate.
    int latencySamples() const { return latency_; }

private:
    struct Stage {
        std::vector<float> taps;
        std::vector<float> hist;   // circular, length taps.size()
        size_t pos = 0;
    };

    std::vector<Stage> stages_;
    std::vector<float> scratch_;
    int factor_ = 1;
    int latency_ = 0;
};

} // namespace sl
