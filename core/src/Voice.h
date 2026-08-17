#pragma once
#include "SharedFxRack.h"
#include "sl/Instrument.h"
#include "webaudio/WaBiquad.h"
#include "webaudio/WaNodes.h"
#include "webaudio/WaOscillator.h"
#include "webaudio/WaParam.h"
#include <array>
#include <vector>

namespace sl {

// zyn's Z.freq: middle C is note 0 at 261.63 Hz.
double noteFrequency(int rootNote, int noteOffset);

// One sounding note. Holds its own copy of the instrument, so editing the
// design mid-chord cannot mutate a ringing voice.
//
// A voice does not return audio. It pushes into the shared FX graph, which the
// pool advances once per sample -- see SharedFxRack for why that matters.
class Voice {
public:
    void prepare(double sampleRate, SharedFxRack* rack);

    void noteOn(const Instrument& inst, int note, double gain, bool sustained);
    void noteOff();
    void kill();                    // immediate, for voice stealing

    void process();                 // one sample into the rack

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
        double out = 0.0;           // last oscillator output, for the FM matrix
    };

    // Modulation-path delay for FM matrix edges. zyn inserts one to break
    // feedback loops; generated instruments always use its 1 ms default.
    static constexpr int kMaxFmDelaySamples = 512;
    struct FmEdge {
        int src = 0, tgt = 0;
        double gain = 0.0;
        std::array<double, kMaxFmDelaySamples> line{};
        int len = 1;
        int pos = 0;
    };

    double sampleRate_ = 48000.0;
    SharedFxRack* rack_ = nullptr;

    Instrument inst_{};
    std::array<OscState, kMaxOscs> oscs_{};
    std::vector<FmEdge> fmEdges_;

    bool active_ = false;
    bool sustained_ = false;
    bool released_ = false;
    int note_ = 0;
    double level_ = 0.0;
    double t_ = 0.0;
    double endTime_ = 0.0;
    double releaseStart_ = 0.0;
    double releaseLen_ = 0.0;
    double releaseFrom_ = 1.0;
    uint64_t startStamp_ = 0;

    // Filter coefficients are refreshed on this cadence, not every sample.
    static constexpr int kCoeffInterval = 8;
    int coeffCounter_ = 0;

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
