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

// The live controls: everything that reaches a note which has already
// started. A seed schedules its envelopes at note-on and plays them back, so
// without these the instrument can be triggered but not played.
//
// EVERY field is exactly neutral at its default. x1.0 and +0.0 are exact in
// IEEE arithmetic, so an untouched voice renders sample for sample what it
// rendered before any of this existed -- which the fidelity suite, comparing
// against a zyn.js that has none of it, depends on.
struct VoiceMacros {
    // Multiplies the filter cutoff, matching Web Audio's
    // BiquadFilterNode.detune: the computed frequency there is
    // frequency * 2^(detune/1200), so a ratio is the exact analogue of a
    // detune in cents. An additive offset in Hz would be the easier thing to
    // write and the wrong thing to play -- inaudible on a cutoff sitting at
    // 15 kHz, catastrophic on one at 200 Hz.
    double cutoffRatio = 1.0;

    // Added to the Q envelope, because Web Audio's Q for a lowpass is already
    // in decibels (see WaBiquad::setCoefficients).
    double resonanceDb = 0.0;

    // How far the filter envelope SWINGS, about the level it settles at.
    // Zero holds the filter still at its sustain value; one is the envelope
    // as generated; two exaggerates it. Deliberately not a second cutoff
    // control: scaling the whole envelope would just be cutoffRatio again.
    double filterEnvAmount = 1.0;

    // Scale every LFO's rate and depth, and the FM depth, across all three
    // LFOs and both FM paths. One control each rather than one per source:
    // an instrument has up to fifteen LFOs, and nobody automates fifteen.
    double lfoRate = 1.0;
    double lfoDepth = 1.0;
    double fmDepth = 1.0;

    // Scales every oscillator's release. Read once at note-off, so a note
    // already fading keeps the length it started with.
    double release = 1.0;
};

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
    void prepare(double sampleRate);

    // The rack is bound per NOTE, not per voice. A voice keeps the rack it was
    // born with for its whole life, which is what makes it safe to build a
    // replacement on the message thread: the builder only ever touches a rack
    // no sounding voice references.
    void noteOn(SharedFxRack* rack, const Instrument& inst, int note,
                double gain, bool sustained);

    const SharedFxRack* rack() const { return rack_; }
    void noteOff();

    // The key was let go while the sustain pedal was down: the note keeps
    // sounding, and releases when the pedal does.
    void holdForSustain() { sustainHeld_ = true; }
    bool sustainHeld() const { return sustainHeld_; }

    // Pitch bend as a frequency ratio, applied after every modulation source
    // rather than to the base pitch: bending a note bends its vibrato and its
    // FM sidebands with it, which is what a detune input does in Web Audio.
    void setBendRatio(double ratio) { bendRatio_ = ratio; }

    void setMacros(const VoiceMacros& m) { target_ = m; }
    const VoiceMacros& macros() const { return target_; }

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

        // Where the filter envelope settles, which is the pivot the envelope
        // amount swings about. Captured at note-on because it comes from the
        // instrument this voice copied, not from the one on screen now.
        double filterSustain = 0.0;
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

    // One ramp per oscillator, not one per voice. zyn's noteOff ramps every
    // gain node with its OWN env.R[0], so an instrument whose oscillators have
    // different release times releases them at different rates; a single
    // voice-wide ramp at the slowest of them is audibly wrong on release.
    std::array<std::array<double, kSubBlock>, kMaxOscs> releaseMul_{};

    bool active_ = false;
    bool sustained_ = false;
    bool released_ = false;
    bool sustainHeld_ = false;
    double bendRatio_ = 1.0;

    // The macros, and the smoother that keeps a knob sweep from stepping the
    // filter coefficients or a modulation depth audibly. Targets are written
    // from outside at block boundaries; the smoothed values advance once per
    // sub-block, so the granularity is 32 samples rather than a host block.
    //
    // Release is not smoothed: it is read once, at note-off, and gliding it
    // would mean a note whose fade changes length while it is fading. LFO
    // rate is not smoothed either -- the oscillators are phase-continuous, so
    // a rate change is inaudible as a discontinuity.
    VoiceMacros target_{};
    VoiceMacros now_{};
    double releaseScale_ = 1.0;      // captured at note-off
    double modCoefPerSample_ = 0.0;
    static constexpr double kModSmoothSeconds = 0.012;

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
    void prepare(double sampleRate, int maxVoices);

    void noteOn(SharedFxRack* rack, const Instrument& inst, int note,
                double gain, bool sustained);
    void noteOff(int note);
    void allNotesOff();

    // MIDI CC 64. Note-offs arriving while this is on are deferred until it
    // goes off, which is what every player expects a pedal to do and what a
    // synth that ignores it is immediately noticed for.
    void setSustainPedal(bool on);
    bool sustainPedal() const { return sustainPedal_; }

    // MIDI pitch wheel, in semitones. Applies to sounding notes and to any
    // started afterwards, until it is set again.
    void setPitchBend(double semitones);
    double pitchBend() const { return bendSemitones_; }

    // The live controls, for every voice, sounding or not yet started. Cheap
    // enough to call every block: it early-outs when nothing moved, which is
    // how the plugin keeps them in step with host automation.
    void setMacros(const VoiceMacros& m);
    const VoiceMacros& macros() const { return macros_; }

    // Mixes every rack that sounding voices reference, and -- when the caller
    // supplies the full set -- every rack still ringing. A reverb tail
    // outlives the note that caused it, so dropping a rack the moment its last
    // voice ends chops the tail off the instant a key is released.
    void render(float* left, float* right, int frames,
                SharedFxRack* const* allRacks = nullptr, int rackCount = 0);

    int activeCount() const;

    // True while any sounding voice still references this rack, so the caller
    // knows it must not be rebuilt.
    bool rackInUse(const SharedFxRack* rack) const;

    // Releases every voice bound to a rack so it can be rebuilt. Audio thread
    // only: a voice's active flag is read every block.
    void killVoicesUsing(const SharedFxRack* rack);

private:
    std::vector<Voice> voices_;
    std::vector<Voice*> active_;         // rebuilt per block, never resized here
    std::vector<SharedFxRack*> racks_;   // distinct racks among active voices
    std::vector<float> mixL_, mixR_;
    double sampleRate_ = 48000.0;
    bool sustainPedal_ = false;
    double bendSemitones_ = 0.0;
    VoiceMacros macros_{};
};

} // namespace sl
