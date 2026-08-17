#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sl {

// ConvolverNode with zyn's generated impulse:
//   impL[i] = impR[i] = randSample() * pow(1 - i/length, decay)
//
// Non-uniform partitioned convolution. zyn gives every oscillator its own
// reverb, so one instrument can run five independent 2-3 second convolutions;
// with uniform 256-sample partitions that alone was 85% of the plugin's CPU.
//
// Structure, which is what Blink's ReverbConvolver does too:
//
//   taps [0, 64)        direct time-domain    -> zero latency
//   taps [64, 512)      FFT, block 64
//   taps [512, 4096)    FFT, block 512
//   taps [4096, 32768)  FFT, block 4096
//   taps [32768, end)   FFT, block 8192
//
// Each FFT segment's first tap equals its block size. That is the point of the
// arrangement: a partitioned convolution with block B has exactly B samples of
// latency, so a segment whose taps begin at B lines up with no output delay
// line at all, and the direct head covers what would otherwise be latency.
//
// Cost falls because the long tail -- where nearly all the taps are -- is
// handled by a handful of large partitions instead of hundreds of small ones.
// Generates the normalised impulse response zyn's reverb config describes.
// Exposed so tests can convolve against it independently, rather than against
// an impulse recovered from the convolver itself -- which proves only that the
// engine agrees with itself.
std::vector<float> makeReverbImpulse(double duration, double decay,
                                     uint32_t rngSeed, double sampleRate);

class WaConvolver {
public:
    void prepare(double sampleRate);

    // rngSeed derives from the reverb config, so the same reverb is
    // bit-identical across runs and across processes.
    void buildImpulse(double duration, double decay, uint32_t rngSeed);
    bool ready() const { return ready_; }
    void reset();

    // Accumulate-then-advance, like WaDelay: in Web Audio every voice feeds the
    // same node input and the node runs once per sample.
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
    struct Chan {
        std::vector<std::vector<float>> fdlRe, fdlIm;   // [partition][2B]
        std::vector<float> inBlock;    // B
        std::vector<float> outBlock;   // B, the samples handed out this block
        std::vector<float> overlap;    // B
        size_t fill = 0;
        size_t fdlPos = 0;

        // A partitioned convolution with block B has exactly B samples of
        // latency. When a segment's first tap sits further out than B -- which
        // happens once the block size is capped -- the difference has to be
        // delayed explicitly, or that whole part of the tail arrives early.
        std::vector<float> outDelay;
        size_t delayPos = 0;
    };

    struct Segment {
        size_t blockSize = 0;
        size_t partitions = 0;
        std::vector<std::vector<float>> irRe, irIm;
        Chan ch[2];
        std::vector<float> scratchRe, scratchIm, accRe, accIm;
    };

    void processSegment(Segment& seg, int channel);

    double sampleRate_ = 48000.0;
    bool ready_ = false;
    double accL_ = 0.0, accR_ = 0.0;
    double outL_ = 0.0, outR_ = 0.0;

    // Direct time-domain head, held twice so the convolution loop needs no
    // wraparound test.
    static constexpr size_t kDirectTaps = 64;
    std::vector<float> directIr_;
    std::vector<float> histL_, histR_;
    size_t histPos_ = 0;

    std::vector<Segment> segments_;

    size_t irLength_ = 0;
    size_t silentSamples_ = 0;
};

} // namespace sl
