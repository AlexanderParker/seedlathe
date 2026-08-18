#include "SampleMatch.h"

#include "Analysis.h"
#include "OfflineRender.h"
#include "WavIO.h"
#include "sl/InstrumentGen.h"
#include "sl/Mulberry32.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace sl {
namespace {

constexpr size_t kMatchSamples =
    static_cast<size_t>(kMatchRate * kMatchSeconds);

// zyn's middle C, the pitch renderOffline produces for note 0.
constexpr double kMiddleC = 261.625565300598634;

std::vector<float> fitLength(std::vector<float> x) {
    x.resize(kMatchSamples, 0.f);
    return x;
}

void l2Normalise(std::vector<float>& v) {
    double sum = 0.0;
    for (float x : v) sum += double(x) * double(x);
    const double norm = std::sqrt(sum);
    if (norm <= 1e-12) return;
    const float inv = static_cast<float>(1.0 / norm);
    for (float& x : v) x *= inv;
}

// Mean dB per band across time, then mean-removed. Removing the mean is what
// makes the comparison about spectral SHAPE rather than overall level, so a
// quiet recording still matches a loud render of the same timbre.
//
// This is where the metric's real limit lives, and it is worth stating rather
// than rediscovering. Broadband noise on the target flattens this profile,
// while every candidate is a clean render with a strongly shaped one -- so a
// noisy target ends up closest to whichever candidate is flattest rather than
// to its own seed. Measured on a lead at 20 dB SNR, which is an ordinary
// recording, its own seed fell from rank 1 of 115 to rank 86, and the top score
// went UP, because a noisy target resembles everything a little.
//
// A floor relative to the peak was tried as a fix and is not one: at 40 dB and
// 25 dB it changes nothing, and at 15 dB it only lifts that rank to 46 while
// costing real separation on clean targets. The fix would be to estimate and
// subtract the noise floor, or to compare only spectral peaks. Until then the
// feature is for clean material -- rendered stems, sample-library one-shots --
// and says so in the UI.
std::vector<float> melProfile(const MelSpectrogram& spec) {
    std::vector<float> out(static_cast<size_t>(std::max(spec.bands, 0)), 0.f);
    if (spec.frames <= 0 || spec.bands <= 0) return out;

    for (int b = 0; b < spec.bands; ++b) {
        double sum = 0.0;
        for (int f = 0; f < spec.frames; ++f) sum += spec.at(f, b);
        out[static_cast<size_t>(b)] = static_cast<float>(sum / spec.frames);
    }
    const double mean =
        std::accumulate(out.begin(), out.end(), 0.0) / double(out.size());
    for (float& x : out) x -= static_cast<float>(mean);
    l2Normalise(out);
    return out;
}

std::vector<float> rmsEnvelope(const std::vector<float>& x) {
    std::vector<float> out(kMatchFrames, 0.f);
    if (x.empty()) return out;

    const size_t hop = std::max<size_t>(1, x.size() / kMatchFrames);
    float peak = 0.f;
    for (int i = 0; i < kMatchFrames; ++i) {
        const size_t from = static_cast<size_t>(i) * hop;
        const size_t to = std::min(from + hop, x.size());
        double sum = 0.0;
        for (size_t j = from; j < to; ++j) sum += double(x[j]) * double(x[j]);
        const float v = (to > from)
                            ? static_cast<float>(std::sqrt(sum / double(to - from)))
                            : 0.f;
        out[static_cast<size_t>(i)] = v;
        peak = std::max(peak, v);
    }
    if (peak > 1e-9f)
        for (float& v : out) v /= peak;
    return out;
}

// Per-frame spectral centroid over Nyquist, from the mel spectrogram's band
// energies. Coarse on purpose: this is a brightness trajectory, not a
// measurement.
std::vector<float> centroidTrack(const MelSpectrogram& spec) {
    std::vector<float> out(kMatchFrames, 0.f);
    if (spec.frames <= 0 || spec.bands <= 0) return out;

    for (int i = 0; i < kMatchFrames; ++i) {
        const int f = std::min(spec.frames - 1,
                               static_cast<int>(int64_t(i) * spec.frames / kMatchFrames));
        double num = 0.0, den = 0.0;
        for (int b = 0; b < spec.bands; ++b) {
            // Mel values are dB with a -100 floor; shift to a positive weight.
            const double w = std::max(0.0, double(spec.at(f, b)) + 100.0);
            num += w * double(b);
            den += w;
        }
        out[static_cast<size_t>(i)] =
            den > 1e-9 ? static_cast<float>(num / den / double(spec.bands)) : 0.f;
    }
    return out;
}

double meanAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n == 0) return 1.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += std::fabs(double(a[i]) - double(b[i]));
    return sum / double(n);
}

} // namespace

int detectRootNote(const std::vector<float>& mono, double sampleRate) {
    if (mono.empty() || sampleRate <= 0.0) return 0;

    // Autocorrelate the loudest window rather than the first: a recording often
    // opens with room tone, and a pitch read off silence is noise.
    constexpr size_t kWindow = 4096;
    const size_t limit = std::min(mono.size(), static_cast<size_t>(sampleRate * 2.0));
    size_t bestStart = 0;
    double bestEnergy = -1.0;
    for (size_t s = 0; s + kWindow <= limit; s += kWindow / 2) {
        double e = 0.0;
        for (size_t i = 0; i < kWindow; ++i) e += double(mono[s + i]) * double(mono[s + i]);
        if (e > bestEnergy) { bestEnergy = e; bestStart = s; }
    }
    if (bestEnergy <= 1e-12) return 0;

    const size_t n = std::min(kWindow, mono.size() - bestStart);
    const float* w = mono.data() + bestStart;

    // 27 Hz to 1050 Hz covers every pitch a seed can usefully be rendered at.
    const size_t minLag = std::max<size_t>(2, static_cast<size_t>(sampleRate / 1050.0));
    const size_t maxLag = std::min(n - 1, static_cast<size_t>(sampleRate / 27.0));
    if (maxLag <= minLag) return 0;

    // Zero-lag energy once, over the whole window.
    double e0 = 0.0;
    for (size_t i = 0; i < n; ++i) e0 += double(w[i]) * double(w[i]);
    if (e0 <= 1e-12) return 0;

    // The BIASED estimator: the sum runs over the shrinking overlap but the
    // divisor stays the full-window energy, so correlation tapers with lag.
    // Normalising by the overlap instead -- which looks more correct -- makes a
    // two-sample overlap correlate perfectly, and the answer is then always the
    // longest lag on offer. That is not a subtlety to rediscover: it returned
    // -36 semitones, the clamp floor, for a pure middle C.
    double bestScore = 0.0;
    size_t bestLag = 0;
    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        double num = 0.0;
        for (size_t i = 0; i + lag < n; ++i) num += double(w[i]) * double(w[i + lag]);
        const double score = num / e0;
        if (score > bestScore) { bestScore = score; bestLag = lag; }
    }
    if (bestLag == 0) return 0;

    const double f0 = sampleRate / double(bestLag);
    const int note = static_cast<int>(std::lround(12.0 * std::log2(f0 / kMiddleC)));
    return std::clamp(note, -36, 36);
}

SoundFeatures featuresOf(const std::vector<float>& mono, double sampleRate,
                         bool detectPitch) {
    SoundFeatures f;
    if (mono.empty() || sampleRate <= 0.0) return f;

    if (detectPitch) f.rootNote = detectRootNote(mono, sampleRate);

    std::vector<float> x = resampleLinear(mono, sampleRate, kMatchRate);
    if (x.empty()) return f;
    x = fitLength(std::move(x));

    // Normalise before analysis, not after. The mel spectrogram floors at
    // -100 dB, so a quiet signal loses detail to clamping that removing the
    // mean afterwards cannot recover -- a render and a recording of the same
    // sound 34 dB apart scored 96.7 rather than 99.9 until this moved here.
    float peak = 0.f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-9f)
        for (float& v : x) v /= peak;

    const MelSpectrogram spec = melSpectrogram(x, kMatchRate);
    f.mel = melProfile(spec);
    f.env = rmsEnvelope(x);
    f.centroid = centroidTrack(spec);
    f.ok = !f.mel.empty();
    return f;
}

int instrumentTranspose(const Instrument& inst) {
    if (inst.oscCount <= 0) return 0;
    const Osc& o = inst.oscs[0];
    return o.oct * 12 + static_cast<int>(o.detune);
}

SoundFeatures featuresOfInstrument(const Instrument& inst, int soundingNote) {
    const int note = soundingNote - instrumentTranspose(inst);
    const RenderResult r = renderOffline(inst, note, 1.0, kMatchSeconds, kMatchRate);
    if (r.left.empty()) return {};

    std::vector<float> mono(r.left.size());
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = 0.5f * (r.left[i] + r.right[i]);

    // Already at kMatchRate, so featuresOf only trims and analyses.
    return featuresOf(mono, kMatchRate, false);
}

double sampleSimilarity(const SoundFeatures& a, const SoundFeatures& b) {
    if (a.empty() || b.empty()) return 0.0;

    // Cosine distance on unit vectors: scale-free, and 0 for identical shapes.
    double dot = 0.0;
    const size_t n = std::min(a.mel.size(), b.mel.size());
    for (size_t i = 0; i < n; ++i) dot += double(a.mel[i]) * double(b.mel[i]);
    const double melDist = std::clamp(1.0 - dot, 0.0, 1.0);

    const double envDist = std::clamp(meanAbsDiff(a.env, b.env), 0.0, 1.0);
    const double centDist = std::clamp(meanAbsDiff(a.centroid, b.centroid) * 4.0, 0.0, 1.0);

    // Timbre dominates: two sounds with the same spectrum and different attacks
    // are far more alike than the reverse. Centroid is last because the mel
    // profile already carries most of the brightness information; it is here to
    // separate sounds whose brightness MOVES differently over the note.
    const double d = 0.60 * melDist + 0.25 * envDist + 0.15 * centDist;
    return 100.0 * (1.0 - std::clamp(d, 0.0, 1.0));
}

SampleSearch::Result SampleSearch::run(const SoundFeatures& target,
                                       int typeFilter,
                                       uint32_t rngSeed,
                                       double threshold,
                                       const std::function<bool()>& shouldStop,
                                       const std::function<void(const Result&)>& onProgress) {
    Result best;
    if (target.empty()) return best;

    Mulberry32 rng(rngSeed);
    const int note = target.rootNote;

    while (!(shouldStop && shouldStop())) {
        uint32_t seed = static_cast<uint32_t>(rng() * 4294967296.0);
        if (typeFilter > 0)
            seed = (seed / 10u) * 10u + static_cast<uint32_t>(typeFilter - 1);

        const Instrument cand = generateInstrument(seed);
        const double score = sampleSimilarity(target, featuresOfInstrument(cand, note));

        ++best.tested;
        best.lastSeed = seed;
        best.lastScore = score;
        const bool improved = score > best.score;
        if (improved) {
            best.seed = seed;
            best.score = score;
            best.found = true;
        }
        // Every candidate, not only the improvements: the caller counts these
        // to report how many seeds have been heard.
        if (onProgress) onProgress(best);
        if (improved && threshold > 0.0 && score >= threshold) break;
    }
    return best;
}

SampleSearchRunner::~SampleSearchRunner() { cancel(); join(); }

bool SampleSearchRunner::loadTarget(const std::string& path) {
    const WavData wav = readWavMono(path);
    if (!wav.ok) {
        error_ = wav.error;
        target_ = {};
        return false;
    }
    setTarget(wav.mono, wav.sampleRate);
    if (!target_.ok) {
        error_ = "the file decoded but produced no usable audio";
        return false;
    }
    error_.clear();
    return true;
}

void SampleSearchRunner::setTarget(const std::vector<float>& mono, double sampleRate) {
    cancel();
    join();
    target_ = featuresOf(mono, sampleRate);
}

void SampleSearchRunner::start(int typeFilter, double threshold) {
    cancel();
    join();
    if (!target_.ok) return;

    cancel_.store(false, std::memory_order_release);
    bestScore_.store(-1.0, std::memory_order_release);
    bestSeed_.store(0, std::memory_order_release);
    tested_.store(0, std::memory_order_release);
    found_.store(false, std::memory_order_release);
    top_.clear();

    // Leave a core for the audio thread and the UI. Rendering is the whole cost
    // here, so saturating every core makes the plugin stutter for no gain.
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const int workers = static_cast<int>(std::min(hw - 1u, 8u));

    running_.store(true, std::memory_order_release);
    live_.store(workers, std::memory_order_release);

    for (int w = 0; w < workers; ++w) {
        threads_.emplace_back([this, typeFilter, threshold, w] {
            SampleSearch::run(
                target_, typeFilter,
                // Distinct streams per worker, or every thread searches the
                // same candidates and the pool buys nothing.
                0x9E3779B9u * static_cast<uint32_t>(w + 1) + 12345u,
                threshold,
                [this] { return cancel_.load(std::memory_order_acquire); },
                [this, threshold](const SampleSearch::Result& r) {
                    tested_.fetch_add(1, std::memory_order_relaxed);
                    if (r.lastScore >= 0.0) top_.offer(r.lastSeed, r.lastScore);
                    if (!r.found) return;

                    double current = bestScore_.load(std::memory_order_acquire);
                    while (r.score > current &&
                           !bestScore_.compare_exchange_weak(current, r.score,
                                                             std::memory_order_acq_rel))
                        ;
                    if (r.score >= current) {
                        bestSeed_.store(r.seed, std::memory_order_release);
                        found_.store(true, std::memory_order_release);
                    }
                    // One worker reaching the threshold stops all of them.
                    if (threshold > 0.0 && r.score >= threshold)
                        cancel_.store(true, std::memory_order_release);
                });

            if (live_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                running_.store(false, std::memory_order_release);
        });
    }
}

void SampleSearchRunner::cancel() { cancel_.store(true, std::memory_order_release); }

void SampleSearchRunner::join() {
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    running_.store(false, std::memory_order_release);
}

SampleSearch::Result SampleSearchRunner::best() const {
    SampleSearch::Result r;
    r.seed = bestSeed_.load(std::memory_order_acquire);
    r.score = bestScore_.load(std::memory_order_acquire);
    r.tested = tested_.load(std::memory_order_relaxed);
    r.found = found_.load(std::memory_order_acquire);
    return r;
}

} // namespace sl
