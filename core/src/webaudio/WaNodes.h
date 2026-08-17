#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sl {

// DelayNode with a feedback loop, matching how zyn wires it: the node's output
// is fed back through a gain into its own input, and the SAME node instance is
// shared by every voice with an identical configuration (see SharedFxRack).
class WaDelay {
public:
    void prepare(double sampleRate, double maxSeconds);
    void setDelayTime(double seconds);
    void setFeedback(double f) { feedback_ = f; }
    void reset();

    double process(double x);

private:
    std::vector<double> buffer_;
    size_t writePos_ = 0;
    double sampleRate_ = 48000.0;
    double delaySamples_ = 0.0;
    double feedback_ = 0.0;
};

// ConvolverNode with zyn's generated impulse:
//   impL[i] = impR[i] = randSample() * pow(1 - i/length, decay)
// Uniform partitioned overlap-add, because the impulse runs to 3.1 s.
//
// zyn's left and right impulse channels are identical, so this runs the same
// spectrum against two independent delay lines rather than a full 2x2 matrix.
class WaConvolver {
public:
    void prepare(double sampleRate);

    // rngSeed derives from the reverb config, so the same reverb is
    // bit-identical across runs and across processes.
    void buildImpulse(double duration, double decay, uint32_t rngSeed);
    bool ready() const { return ready_; }
    void reset();

    void process(double inL, double inR, double& outL, double& outR);

private:
    struct Channel {
        std::vector<std::vector<double>> fdlRe, fdlIm;  // input spectrum history
        std::vector<double> overlap;
        std::vector<double> inBlock;
        std::vector<double> outBlock;
        size_t fill = 0;
        size_t fdlPos = 0;
        size_t outPos = 0;
    };

    void processBlock(Channel& c);

    double sampleRate_ = 48000.0;
    bool ready_ = false;
    size_t blockSize_ = 256;
    size_t partitions_ = 0;
    std::vector<std::vector<double>> irRe_, irIm_;
    Channel left_, right_;
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
