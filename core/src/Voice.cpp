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

void Voice::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    rack_ = nullptr;
    for (auto& o : oscs_) {
        o.osc.prepare(sampleRate);
        o.noise.prepare(sampleRate);
        o.filter.prepare(sampleRate);
        o.gLfo.prepare(sampleRate);
        o.fLfo.prepare(sampleRate);
        o.pLfo.prepare(sampleRate);
        o.fmOsc.prepare(sampleRate);
    }
    for (auto& o : oscs_) o.out.assign(SharedFxRack::kMaxBlock, 0.0);
    fmEdges_.reserve(kMaxOscs * kMaxOscs);
    active_ = false;
}

void Voice::noteOn(SharedFxRack* rack, const Instrument& inst, int note,
                   double gain, bool sustained) {
    // Bind the rack for the life of this note. Nothing may rebuild a rack while
    // a voice still points at it.
    rack_ = rack;

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
    WaPanner::gains(0.0, panL_, panR_);

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

        s.gainEnv.beginStepping(sampleRate_);
        s.filterEnv.beginStepping(sampleRate_);
        s.qEnv.beginStepping(sampleRate_);
        if (s.hasPitchEnv) s.pitchEnv.beginStepping(sampleRate_);

        s.route = rack_->acquireRoute(c);
        s.coeffCounter = 0;
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
                fmEdges_.push_back(e);
            }
        }
    }

    // An oscillator may only be rendered a sub-block ahead of its modulator
    // while the modulation delay is at least that long.
    subBlock_ = kSubBlock;
    for (const auto& e : fmEdges_) subBlock_ = std::min(subBlock_, e.len);
    if (subBlock_ < 1) subBlock_ = 1;

    for (auto& ring : fmHistory_) ring.fill(0.0);
    fmHistPos_ = 0;
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
}

void Voice::kill() {
    active_ = false;
    level_ = 0.0;
}

void Voice::renderOscillator(int oscIndex, int frames, int outOffset,
                             const double* releaseMul) {
    OscState& s = oscs_[static_cast<size_t>(oscIndex)];
    const Osc& c = inst_.oscs[static_cast<size_t>(oscIndex)];
    double* __restrict out = s.out.data() + outOffset;

    // Gather the FM matrix edges targeting this oscillator once, not per
    // sample. The modulator's history is at least a sub-block old, so it is
    // complete before this oscillator runs.
    const FmEdge* edges[kMaxOscs * kMaxOscs];
    int edgeCount = 0;
    for (const auto& e : fmEdges_)
        if (e.tgt == oscIndex) edges[edgeCount++] = &e;

    double peak = 0.0;

    for (int i = 0; i < frames; ++i) {
        double sample;
        if (s.isNoise) {
            sample = s.noise.render();
        } else {
            // The pitch envelope REPLACES the oscillator frequency rather than
            // offsetting it: zyn schedules setValueAtTime(0) then ramps to
            // oFreq * amount, so the oscillator starts at 0 Hz.
            double freq = s.hasPitchEnv ? s.pitchEnv.nextValue() : s.baseFreq;

            // Connected AudioParam inputs sum on top of the automation value.
            if (s.hasPLfo) freq += s.pLfo.render(c.pLfo.frequency) * s.pLfoDepth;
            if (s.hasFm) freq += s.fmOsc.render(s.fmFreq) * s.fmDepth;

            for (int e = 0; e < edgeCount; ++e) {
                const FmEdge& edge = *edges[e];
                int idx = fmHistPos_ + i - edge.len;
                while (idx < 0) idx += kMaxFmDelaySamples;
                idx %= kMaxFmDelaySamples;
                freq += fmHistory_[static_cast<size_t>(edge.src)]
                                  [static_cast<size_t>(idx)] * edge.gain;
            }

            sample = s.osc.render(freq);
        }

        // Raw oscillator output feeds the FM matrix history.
        {
            int idx = (fmHistPos_ + i) % kMaxFmDelaySamples;
            fmHistory_[static_cast<size_t>(oscIndex)][static_cast<size_t>(idx)] = sample;
        }

        if (s.useShaper) sample = s.shaper.process(sample);

        double cutoff = s.filterEnv.nextValue();
        if (s.hasFLfo) cutoff += s.fLfo.render(c.fLfo.frequency) * s.fLfoDepth;
        const double q = s.qEnv.nextValue();
        // Coefficients are refreshed every kCoeffInterval samples. Recomputing
        // them costs about seven times the filtering itself -- two
        // transcendentals plus a pow -- and the envelopes driving cutoff and Q
        // are linear ramps, so this quantises them to 0.17 ms at 48 kHz.
        //
        // ALWAYS lowpass, regardless of osc.filterType. zyn generates a
        // filterType into every oscillator and then never assigns it to the
        // node, so every filter in zyn is the Web Audio default. The field is
        // still carried in the data model because the search scorer reads it.
        if (s.coeffCounter == 0)
            s.filter.setCoefficients(FilterType::Lowpass, cutoff, q, 0.0);
        if (++s.coeffCounter >= kCoeffInterval) s.coeffCounter = 0;
        sample = s.filter.process(sample);

        double g = s.gainEnv.nextValue();
        if (s.hasGLfo) g += s.gLfo.render(c.gLfo.frequency) * s.gLfoDepth;
        g *= releaseMul[i];
        sample *= g;

        const double a = sample < 0.0 ? -sample : sample;
        if (a > peak) peak = a;
        out[i] = sample;
    }

    if (peak > level_) level_ = peak;
}

void Voice::processBlock(int frames) {
    if (!active_) return;

    for (int o = 0; o < inst_.oscCount; ++o)
        std::fill_n(oscs_[static_cast<size_t>(o)].out.begin(), frames, 0.0);

    level_ = 0.0;
    int done = 0;

    while (done < frames && active_) {
        int n = std::min(subBlock_, frames - done);

        // Release multiplier and end-of-note detection, shared by every
        // oscillator in this sub-block.
        bool endsHere = false;
        int valid = n;
        for (int i = 0; i < n; ++i) {
            const double t = t_ + double(i) / sampleRate_;
            double mul = 1.0;
            if (released_) {
                const double elapsed = t - releaseStart_;
                mul = releaseLen_ > 0.0 ? 1.0 - elapsed / releaseLen_ : 0.0;
                if (mul <= 0.0) { endsHere = true; valid = i; break; }
            }
            if (!sustained_ && t > endTime_) { endsHere = true; valid = i; break; }
            releaseMul_[static_cast<size_t>(i)] = mul;
        }
        n = valid;

        if (n > 0) {
            for (int o = 0; o < inst_.oscCount; ++o)
                renderOscillator(o, n, done, releaseMul_.data());
            fmHistPos_ = (fmHistPos_ + n) % kMaxFmDelaySamples;
            t_ += double(n) / sampleRate_;
            done += n;
        }

        if (endsHere) {
            active_ = false;
            level_ = 0.0;
            break;
        }
    }

    // zyn's Z.play pans centre, so the panner is a pair of constant gains
    // resolved at note-on rather than a cos/sin per sample.
    for (int o = 0; o < inst_.oscCount; ++o) {
        const OscState& s = oscs_[static_cast<size_t>(o)];
        rack_->pushBlockMono(s.route, s.out.data(), panL_, panR_, frames);
    }
}

// ------------------------------------------------------------- VoicePool

void VoicePool::prepare(double sampleRate, int maxVoices) {
    sampleRate_ = sampleRate;
    voices_.resize(static_cast<size_t>(std::max(1, maxVoices)));
    active_.reserve(voices_.size());
    racks_.reserve(voices_.size());
    mixL_.assign(SharedFxRack::kMaxBlock, 0.0f);
    mixR_.assign(SharedFxRack::kMaxBlock, 0.0f);
    for (auto& v : voices_) v.prepare(sampleRate);
}

void VoicePool::noteOn(SharedFxRack* rack, const Instrument& inst, int note,
                       double gain, bool sustained) {
    for (auto& v : voices_) {
        if (!v.active()) { v.noteOn(rack, inst, note, gain, sustained); return; }
    }
    // Steal the quietest, breaking ties by age.
    Voice* victim = &voices_[0];
    for (auto& v : voices_) {
        if (v.currentLevel() < victim->currentLevel() ||
            (v.currentLevel() == victim->currentLevel() && v.age() < victim->age()))
            victim = &v;
    }
    victim->kill();
    victim->noteOn(rack, inst, note, gain, sustained);
}

void VoicePool::noteOff(int note) {
    for (auto& v : voices_)
        if (v.active() && !v.released() && v.note() == note) v.noteOff();
}

void VoicePool::allNotesOff() {
    for (auto& v : voices_) v.kill();
}

void VoicePool::killVoicesUsing(const SharedFxRack* rack) {
    for (auto& v : voices_)
        if (v.active() && v.rack() == rack) v.kill();
}

bool VoicePool::rackInUse(const SharedFxRack* rack) const {
    for (const auto& v : voices_)
        if (v.active() && v.rack() == rack) return true;
    return false;
}

void VoicePool::render(float* left, float* right, int frames) {
    int done = 0;
    while (done < frames) {
        const int n = std::min(frames - done, SharedFxRack::kMaxBlock);

        // Gather the active voices, and the distinct racks they reference,
        // once per block rather than once per sample.
        active_.clear();
        racks_.clear();
        for (auto& v : voices_) {
            if (!v.active()) continue;
            active_.push_back(&v);
            SharedFxRack* r = const_cast<SharedFxRack*>(v.rack());
            if (!r) continue;
            bool seen = false;
            for (auto* known : racks_) if (known == r) { seen = true; break; }
            if (!seen) racks_.push_back(r);
        }

        for (int i = 0; i < n; ++i) {
            left[done + i] = 0.f;
            right[done + i] = 0.f;
        }

        for (auto* r : racks_) r->beginBlock(n);
        for (Voice* v : active_) v->processBlock(n);
        for (auto* r : racks_) {
            r->mixBlock(mixL_.data(), mixR_.data(), n);
            for (int i = 0; i < n; ++i) {
                left[done + i] += mixL_[static_cast<size_t>(i)];
                right[done + i] += mixR_[static_cast<size_t>(i)];
            }
        }

        done += n;
    }
}

int VoicePool::activeCount() const {
    int n = 0;
    for (const auto& v : voices_) if (v.active()) ++n;
    return n;
}

} // namespace sl
