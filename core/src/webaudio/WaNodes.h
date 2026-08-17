#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "webaudio/WaConvolver.h"

namespace sl {

// DelayNode with a feedback loop, matching how zyn wires it: the node's output
// is fed back through a gain into its own input, and the SAME node instance is
// shared by every voice with an identical configuration (see SharedFxRack).
// Stereo, and driven by accumulate-then-advance rather than a per-caller
// process() call. That is not a style choice: in Web Audio every voice
// connects to the SAME node input and their signals sum before the node runs
// once per sample. Letting each voice call process() would advance the delay
// line once per voice and smear every note across different tap positions.
class WaDelay {
public:
    void prepare(double sampleRate, double maxSeconds);
    void setDelayTime(double seconds);
    void setFeedback(double f) { feedback_ = f; }
    void reset();

    void addInput(double l, double r) { accL_ += l; accR_ += r; }
    void advance();
    double outL() const { return outL_; }
    double outR() const { return outR_; }

    // Mono convenience for tests: one input, one advance, one output.
    double process(double x) {
        addInput(x, x);
        advance();
        return outL_;
    }

private:
    double tap(std::vector<double>& buf, double in);

    std::vector<double> bufferL_, bufferR_;
    size_t writePos_ = 0;
    double sampleRate_ = 48000.0;
    double delaySamples_ = 0.0;
    double feedback_ = 0.0;
    double accL_ = 0.0, accR_ = 0.0;
    double outL_ = 0.0, outR_ = 0.0;
};

// StereoPannerNode's equal-power law for a mono input.
class WaPanner {
public:
    static void pan(double in, double p, double& l, double& r);
};

// WaveShaperNode plus zyn's getDistCurve.
class WaShaper {
public:
    void setCurve(double amount, double sampleRate);
    void setOversample(int factor);
    double process(double x);

private:
    std::vector<float> curve_;
    int oversample_ = 1;
    // Halfband state for 2x/4x, one biquad-ish stage per direction.
    double up1_ = 0.0, up2_ = 0.0, dn1_ = 0.0, dn2_ = 0.0;
};

// zyn's distortion transfer curve, with the sampleRate fix applied upstream:
//   curve[i] = ((3 + k) * x * 20 * deg) / (pi + k * |x|),  x = 2i/n - 1
std::vector<float> getDistCurve(double k, double sampleRate);

} // namespace sl
