#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

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

    // Same accumulate-then-advance contract as WaDelay, for the same reason.
    void addInput(double l, double r) { accL_ += l; accR_ += r; }
    void advance();
    double outL() const { return outL_; }
    double outR() const { return outR_; }

    void process(double inL, double inR, double& outL, double& outR) {
        addInput(inL, inR);
        advance();
        outL = outL_;
        outR = outR_;
    }

private:
    struct Channel {
        std::vector<std::vector<float>> fdlRe, fdlIm;  // input spectrum history
        std::vector<float> overlap;
        std::vector<float> inBlock;
        std::vector<float> outBlock;
        size_t fill = 0;
        size_t fdlPos = 0;
        size_t outPos = 0;
    };

    void processBlock(Channel& c);

    double sampleRate_ = 48000.0;
    bool ready_ = false;
    double accL_ = 0.0, accR_ = 0.0;
    double outL_ = 0.0, outR_ = 0.0;
    size_t blockSize_ = 256;
    size_t partitions_ = 0;
    std::vector<std::vector<float>> irRe_, irIm_;
    Channel left_, right_;

    // Once the input has been silent for longer than the tail, the output is
    // zero and there is nothing to compute. Without this every reverb an
    // instrument owns keeps convolving forever after the last note ends.
    size_t irLength_ = 0;
    size_t silentBlocks_ = 0;

    // Scratch for processBlock, allocated once in buildImpulse. Creating these
    // per call cost 16 allocations per audio callback -- see
    // tests/test_realtime.cpp.
    std::vector<float> scratchRe_, scratchIm_, accRe_, accIm_;
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
