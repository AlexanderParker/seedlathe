#pragma once
#include "SharedFxRack.h"
#include "sl/Instrument.h"
#include "webaudio/WaBiquad.h"
#include "webaudio/WaNodes.h"
#include "webaudio/WaOscillator.h"
#include "webaudio/WaParam.h"
#include <array>
#include <cstdint>
#include <vector>

namespace sl {

// zyn's Z.freq: middle C is note 0 at 261.63 Hz.
double noteFrequency(int rootNote, int noteOffset);

// One sounding note. Holds its own copy of the instrument, so editing the
// design mid-chord cannot mutate a ringing voice.
//
// Rendering is block-based, one oscillator at a time. Interleaving voices and
// oscillators sample by sample meant a voice's state was evicted from L1 by
// every other voice on every sample; at eight voices that made the path
// memory-bound rather than arithmetic-bound, and further micro-optimising the
// arithmetic stopped helping at all.
class Voice {
public:
    void prepare(double sampleRate, SharedFxRack* rack);

    void noteOn(const Instrument& inst, int note, double gain, bool sustained);
    void noteOff();
    void kill();                    // immediate, for voice stealing

    // Renders `frames` samples into the shared FX graph's block buffers.
    void processBlock(int frames);

    bool active() const { return active_; }
    int note() const { return note_; }
    bool released() const { return released_; }
    double currentLevel() const { return level_; }
    uint64_t age() const { return startStamp_; }

private:
    struct OscState {
        WaOscillator osc;
        NoiseSource noise;
        WaBiquad filter;
        WaShaper shaper;
        bool useShaper = false;

        WaParam gainEnv, filterEnv, qEnv, pitchEnv;
        bool hasPitchEnv = false;

        WaOscillator gLfo, fLfo, pLfo, fmOsc;
        bool hasGLfo = false, hasFLfo = false, hasPLfo = false, hasFm = false;
        double gLfoDepth = 0.0, fLfoDepth = 0.0, pLfoDepth = 0.0;
        double fmDepth = 0.0, fmFreq = 0.0;

        bool isNoise = false;
        double baseFreq = 0.0;
        double releaseTime = 0.0;
        SharedFxRack::Route route;

        // Mono output for the current block; panned as it is pushed.
        std::vector<double> out;
        int coeffCounter = 0;
    };

    // An oscillator can be rendered a whole sub-block ahead of the ones that
    // modulate it only while the modulation delay is at least that long. zyn's
    // generated FM delay is 1 ms -- 48 samples at 48 kHz -- so 32 is safe; a
    // shorter designed delay drops this voice to single-sample steps.
    static constexpr int kSubBlock = 32;
    static constexpr int kMaxFmDelaySamples = 512;

    struct FmEdge {
        int src = 0, tgt = 0;
        double gain = 0.0;
        int len = 1;
    };

    void renderOscillator(int oscIndex, int frames, int outOffset,
                          const double* releaseMul);

    double sampleRate_ = 48000.0;
    SharedFxRack* rack_ = nullptr;

    Instrument inst_{};
    std::array<OscState, kMaxOscs> oscs_{};
    std::vector<FmEdge> fmEdges_;
    int subBlock_ = kSubBlock;

    // Per-oscillator output history, read by every FM matrix edge. One ring per
    // oscillator rather than one per edge: 25 edges would otherwise each carry
    // their own copy of the same signal.
    std::array<std::array<double, kMaxFmDelaySamples>, kMaxOscs> fmHistory_{};
    int fmHistPos_ = 0;

    std::array<double, kSubBlock> releaseMul_{};

    bool active_ = false;
    bool sustained_ = false;
    bool released_ = false;
    int note_ = 0;
    double level_ = 0.0;
    double t_ = 0.0;
    double endTime_ = 0.0;
    double releaseStart_ = 0.0;
    double releaseLen_ = 0.0;
    uint64_t startStamp_ = 0;

    static constexpr int kCoeffInterval = 8;

    // Pan is fixed for the life of a note, so its gains are resolved once.
    double panL_ = 0.7071067811865476, panR_ = 0.7071067811865476;
};

// Fixed pool with stealing. zyn creates nodes without bound, which is fine in
// a browser tab you can close and not fine in a host.
class VoicePool {
public:
    void prepare(double sampleRate, int maxVoices, SharedFxRack* rack);

    void noteOn(const Instrument& inst, int note, double gain, bool sustained);
    void noteOff(int note);
    void allNotesOff();

    void render(float* left, float* right, int frames);

    int activeCount() const;

private:
    std::vector<Voice> voices_;
    std::vector<Voice*> active_;   // rebuilt per block, never resized in render
    SharedFxRack* rack_ = nullptr;
    double sampleRate_ = 48000.0;
};

} // namespace sl
