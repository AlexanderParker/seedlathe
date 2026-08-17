#include "Voice.h"
#include <algorithm>
#include <cmath>

namespace sl {
namespace {

uint64_t g_stamp = 0;

// zyn's Z.adsr: setValueAtTime(0), then four sequential linear ramps.
// Attack is clamped to a 5 ms minimum to avoid a click.
double scheduleAdsr(WaParam& p, double t, const Adsr& env, double max) {
    p.reset(0.0);
    p.setValueAtTime(0.0, t);
    const double aEnd = t + std::max(env.aT, 0.005);
    p.linearRampToValueAtTime(env.aV * max, aEnd);
    p.linearRampToValueAtTime(env.dV * max, t + env.aT + env.dT);
    p.linearRampToValueAtTime(env.sV * max, t + env.aT + env.dT + env.sT);
    const double end = t + env.aT + env.dT + env.sT + env.rT;
    p.linearRampToValueAtTime(env.rV * max, end);
    return end;
}

// zyn's Z.adsrSustain: attack, decay, sustain, then hold. No release is
// scheduled -- noteOff supplies it.
double scheduleAdsrSustain(WaParam& p, double t, const Adsr& env, double max) {
    p.reset(0.0);
    p.setValueAtTime(0.0, t);
    p.linearRampToValueAtTime(env.aV * max, t + std::max(env.aT, 0.005));
    p.linearRampToValueAtTime(env.dV * max, t + env.aT + env.dT);
    p.linearRampToValueAtTime(env.sV * max, t + env.aT + env.dT + env.sT);
    return t + env.aT + env.dT + env.sT;
}

} // namespace

double noteFrequency(int rootNote, int noteOffset) {
    return 261.63 * std::pow(2.0, double(rootNote + noteOffset) / 12.0);
}

void Voice::prepare(double sampleRate, SharedFxRack* rack) {
    sampleRate_ = sampleRate;
    rack_ = rack;
    for (auto& o : oscs_) {
        o.osc.prepare(sampleRate);
        o.noise.prepare(sampleRate);
        o.filter.prepare(sampleRate);
        o.gLfo.prepare(sampleRate);
        o.fLfo.prepare(sampleRate);
        o.pLfo.prepare(sampleRate);
        o.fmOsc.prepare(sampleRate);
    }
    fmEdges_.reserve(kMaxOscs * kMaxOscs);
    active_ = false;
}

void Voice::noteOn(const Instrument& inst, int note, double gain, bool sustained) {
    // Copy the instrument: a later edit to the caller's tree must not reach a
    // ringing voice.
    inst_ = inst;
    note_ = note;
    sustained_ = sustained;
    released_ = false;
    active_ = true;
    t_ = 0.0;
    level_ = 0.0;
    endTime_ = 0.0;
    startStamp_ = ++g_stamp;
    coeffCounter_ = 0;

    const int n = inst_.oscCount;
    // zyn: voiceGain = 1 / (notes * oscs), and layer.gain = 0.5 * gain.
    const double voiceGain = 1.0 / double(std::max(1, n));
    const double maxGain = 0.5 * gain * voiceGain;

    for (int i = 0; i < n; ++i) {
        const Osc& c = inst_.oscs[static_cast<size_t>(i)];
        OscState& s = oscs_[static_cast<size_t>(i)];

        s.isNoise = (c.waveform == Waveform::Noise);
        // detune is SEMITONES in zyn, not cents.
        s.baseFreq = noteFrequency(0, note + c.oct * 12 + static_cast<int>(c.detune));

        if (s.isNoise) s.noise.reset();
        else { s.osc.setType(c.waveform); s.osc.resetPhase(); }

        s.filter.reset();

        s.useShaper = c.dist.on;
        if (s.useShaper) {
            s.shaper.setCurve(c.dist.amount, sampleRate_);
            s.shaper.setOversample(c.dist.oversample);
        }

        if (sustained) {
            endTime_ = std::max(endTime_,
                                scheduleAdsrSustain(s.gainEnv, 0.0, c.adsrGain, maxGain));
            scheduleAdsrSustain(s.filterEnv, 0.0, c.adsrFilter, 20000.0);
            scheduleAdsrSustain(s.qEnv, 0.0, c.adsrFilterQ, 30.0);
        } else {
            endTime_ = std::max(endTime_,
                                scheduleAdsr(s.gainEnv, 0.0, c.adsrGain, maxGain));
            endTime_ = std::max(endTime_,
                                scheduleAdsr(s.filterEnv, 0.0, c.adsrFilter, 20000.0));
            endTime_ = std::max(endTime_,
                                scheduleAdsr(s.qEnv, 0.0, c.adsrFilterQ, 30.0));
        }
        s.releaseTime = c.adsrGain.rT;

        // The pitch envelope REPLACES the oscillator frequency rather than
        // offsetting it: zyn schedules setValueAtTime(0) then ramps to
        // oFreq * amount, so the oscillator starts at 0 Hz.
        s.hasPitchEnv = c.pEnv.on && !s.isNoise;
        if (s.hasPitchEnv) {
            const double target = s.baseFreq * c.pEnv.amount;
            if (sustained) scheduleAdsrSustain(s.pitchEnv, 0.0, c.pEnv.env, target);
            else scheduleAdsr(s.pitchEnv, 0.0, c.pEnv.env, target);
        }

        s.hasGLfo = c.gLfo.on;
        if (s.hasGLfo) {
            s.gLfo.setType(c.gLfo.type);
            s.gLfo.resetPhase();
            s.gLfoDepth = c.gLfo.depth;
        }
        s.hasFLfo = c.fLfo.on && !s.isNoise;
        if (s.hasFLfo) {
            s.fLfo.setType(c.fLfo.type);
            s.fLfo.resetPhase();
            s.fLfoDepth = c.fLfo.depth;
        }
        s.hasPLfo = c.pLfo.on && !s.isNoise;
        if (s.hasPLfo) {
            s.pLfo.setType(c.pLfo.type);
            s.pLfo.resetPhase();
            s.pLfoDepth = c.pLfo.depth * s.baseFreq;
        }
        s.hasFm = c.fm.on && !s.isNoise;
        if (s.hasFm) {
            s.fmOsc.setType(c.fm.type);
            s.fmOsc.resetPhase();
            s.fmFreq = std::clamp(c.fm.frequency * s.baseFreq, -22050.0, 22050.0);
            s.fmDepth = c.fm.depth;
        }

        s.route = rack_->acquireRoute(c);
        s.out = 0.0;
    }

    // FM matrix. zyn skips any edge touching a noise oscillator and routes the
    // modulator through a short delay to break feedback loops.
    fmEdges_.clear();
    if (inst_.hasFmMatrix) {
        for (int src = 0; src < n; ++src) {
            for (int tgt = 0; tgt < n; ++tgt) {
                const double amt = inst_.fmMatrix[static_cast<size_t>(src)]
                                                 [static_cast<size_t>(tgt)];
                if (amt == 0.0) continue;
                if (oscs_[static_cast<size_t>(src)].isNoise) continue;
                if (oscs_[static_cast<size_t>(tgt)].isNoise) continue;

                FmEdge e;
                e.src = src;
                e.tgt = tgt;
                // zyn: gain = amt * targetFreq * 0.2, frequency-relative so FM
                // depth stays consistent across octaves.
                e.gain = amt * oscs_[static_cast<size_t>(tgt)].baseFreq * 0.2;
                double d = inst_.fmDelays[static_cast<size_t>(src)]
                                         [static_cast<size_t>(tgt)];
                if (d <= 0.0) d = 0.001;   // zyn's default
                e.len = std::clamp(static_cast<int>(d * sampleRate_), 1,
                                   kMaxFmDelaySamples - 1);
                e.line.fill(0.0);
                e.pos = 0;
                fmEdges_.push_back(e);
            }
        }
    }
}

void Voice::noteOff() {
    if (!active_ || released_) return;
    released_ = true;
    releaseStart_ = t_;
    // zyn's Z.noteOff: ramp from the current value to zero over the
    // instrument's release, with a small minimum to prevent a click.
    double r = 0.015;
    for (int i = 0; i < inst_.oscCount; ++i)
        r = std::max(r, std::max(oscs_[static_cast<size_t>(i)].releaseTime, 0.015));
    releaseLen_ = r;
    releaseFrom_ = 1.0;
}

void Voice::kill() {
    active_ = false;
    level_ = 0.0;
}

void Voice::process() {
    if (!active_) return;

    double peak = 0.0;

    // Snapshot last-sample oscillator outputs so every FM matrix edge reads
    // the same generation, rather than seeing partially updated neighbours.
    double prevOut[kMaxOscs];
    for (int i = 0; i < inst_.oscCount; ++i)
        prevOut[i] = oscs_[static_cast<size_t>(i)].out;

    // Release multiplier, applied on top of the sustain envelope.
    double releaseMul = 1.0;
    if (released_) {
        const double elapsed = t_ - releaseStart_;
        releaseMul = releaseLen_ > 0.0 ? 1.0 - elapsed / releaseLen_ : 0.0;
        if (releaseMul <= 0.0) { active_ = false; level_ = 0.0; return; }
    }

    for (int i = 0; i < inst_.oscCount; ++i) {
        OscState& s = oscs_[static_cast<size_t>(i)];
        const Osc& c = inst_.oscs[static_cast<size_t>(i)];

        double sample;
        if (s.isNoise) {
            sample = s.noise.render();
        } else {
            // Base frequency, or the pitch envelope's value when it replaces it.
            double freq = s.hasPitchEnv ? s.pitchEnv.valueAt(t_) : s.baseFreq;

            // Connected AudioParam inputs sum on top of the automation value.
            if (s.hasPLfo) freq += s.pLfo.render(inst_.oscs[static_cast<size_t>(i)]
                                                     .pLfo.frequency) * s.pLfoDepth;
            if (s.hasFm) freq += s.fmOsc.render(s.fmFreq) * s.fmDepth;

            for (auto& e : fmEdges_) {
                if (e.tgt != i) continue;
                const double delayed = e.line[static_cast<size_t>(e.pos)];
                freq += delayed * e.gain;
            }

            sample = s.osc.render(freq);
        }

        s.out = sample;

        if (s.useShaper) sample = s.shaper.process(sample);

        double cutoff = s.filterEnv.valueAt(t_);
        if (s.hasFLfo) cutoff += s.fLfo.render(c.fLfo.frequency) * s.fLfoDepth;
        const double q = s.qEnv.valueAt(t_);
        // Coefficients are refreshed every kCoeffInterval samples rather than
        // every sample. Recomputing them costs about seven times the filtering
        // itself -- two transcendentals plus a pow -- and at polyphony that
        // alone blew the audio callback's deadline. The envelopes driving
        // cutoff and Q are linear ramps, so at 48 kHz this quantises them to
        // 0.17 ms. The cost to fidelity is measured, not assumed: see
        // vectors/fidelity-thresholds.json.
        if (coeffCounter_ == 0)
        // ALWAYS lowpass, regardless of osc.filterType. zyn generates a
        // filterType into every oscillator and then never assigns it to the
        // node -- render() creates the BiquadFilterNode and sets only .Q and
        // .frequency, so every filter in zyn is the Web Audio default, which is
        // lowpass. Honouring filterType here would mis-filter the ~86% of
        // oscillators whose generated type is something else. The field is
        // still carried in the data model because the search scorer reads it,
        // exactly like filterQ.
        s.filter.setCoefficients(FilterType::Lowpass, cutoff, q, 0.0);
        sample = s.filter.process(sample);

        double g = s.gainEnv.valueAt(t_);
        if (s.hasGLfo) g += s.gLfo.render(c.gLfo.frequency) * s.gLfoDepth;
        g *= releaseMul;
        sample *= g;

        peak = std::max(peak, std::abs(sample));

        // zyn's Z.play uses pan 0, so the panner is centred equal-power.
        double l, r;
        WaPanner::pan(sample, 0.0, l, r);
        rack_->push(s.route, l, r);
    }

    // Advance the FM matrix modulation delays with this sample's outputs.
    for (auto& e : fmEdges_) {
        e.line[static_cast<size_t>(e.pos)] = prevOut[e.src];
        e.pos = (e.pos + 1) % e.len;
    }

    if (++coeffCounter_ >= kCoeffInterval) coeffCounter_ = 0;
    level_ = peak;
    t_ += 1.0 / sampleRate_;

    if (!sustained_ && t_ > endTime_) { active_ = false; level_ = 0.0; }
}

// ------------------------------------------------------------- VoicePool

void VoicePool::prepare(double sampleRate, int maxVoices, SharedFxRack* rack) {
    sampleRate_ = sampleRate;
    rack_ = rack;
    voices_.resize(static_cast<size_t>(std::max(1, maxVoices)));
    for (auto& v : voices_) v.prepare(sampleRate, rack);
}

void VoicePool::noteOn(const Instrument& inst, int note, double gain, bool sustained) {
    for (auto& v : voices_) {
        if (!v.active()) { v.noteOn(inst, note, gain, sustained); return; }
    }
    // Steal the quietest, breaking ties by age.
    Voice* victim = &voices_[0];
    for (auto& v : voices_) {
        if (v.currentLevel() < victim->currentLevel() ||
            (v.currentLevel() == victim->currentLevel() && v.age() < victim->age()))
            victim = &v;
    }
    victim->kill();
    victim->noteOn(inst, note, gain, sustained);
}

void VoicePool::noteOff(int note) {
    for (auto& v : voices_)
        if (v.active() && !v.released() && v.note() == note) v.noteOff();
}

void VoicePool::allNotesOff() {
    for (auto& v : voices_) v.kill();
}

void VoicePool::render(float* left, float* right, int frames) {
    for (int i = 0; i < frames; ++i) {
        for (auto& v : voices_) v.process();
        double l = 0.0, r = 0.0;
        rack_->mixAndAdvance(l, r);
        left[i] = static_cast<float>(l);
        right[i] = static_cast<float>(r);
    }
}

int VoicePool::activeCount() const {
    int n = 0;
    for (const auto& v : voices_) if (v.active()) ++n;
    return n;
}

} // namespace sl
