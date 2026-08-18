#include "webaudio/WaOscillator.h"
#include "sl/Mulberry32.h"
#include <cmath>
#include <map>
#include <mutex>

namespace sl {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Blink PeriodicWave constants (third_party/blink/renderer/modules/webaudio/
// periodic_wave.cc).
constexpr unsigned kNumberOfOctaveBands = 3;
constexpr double kCentsPerRange = 1200.0 / kNumberOfOctaveBands;   // 400

unsigned periodicWaveSize(double sampleRate) {
    if (sampleRate <= 24000.0) return 2048;
    if (sampleRate <= 88200.0) return 4096;
    return 16384;
}

// Fourier coefficient for partial n. Blink writes all built-in waveforms into
// the imaginary array only -- they are odd functions, so pure sines.
double partialAmplitude(Waveform w, unsigned n) {
    const double piFactor = 2.0 / (double(n) * kPi);
    switch (w) {
    case Waveform::Sine:
        return n == 1 ? 1.0 : 0.0;
    case Waveform::Square:
        return (n & 1u) ? 2.0 * piFactor : 0.0;               // 4/(n*pi), odd n
    case Waveform::Sawtooth:
        return piFactor * ((n & 1u) ? 1.0 : -1.0);            // (2/(n*pi))*(-1)^(n+1)
    case Waveform::Triangle: {
        if (!(n & 1u)) return 0.0;
        const double sign = (((n - 1u) / 2u) & 1u) ? -1.0 : 1.0;
        return 2.0 * piFactor * piFactor * sign;              // (8/(pi^2 n^2))*(-1)^((n-1)/2)
    }
    case Waveform::Noise:
    default:
        return 0.0;
    }
}

} // namespace

// At namespace scope rather than in the anonymous namespace above: the header
// forward-declares sl::WaveTables so WaOscillator can cache a pointer to its
// table set instead of re-resolving it under a mutex on every sample.
struct WaveTables {
    unsigned size = 0;
    unsigned ranges = 0;
    double lowestFundamental = 0.0;
    std::vector<std::vector<float>> table;   // [range][sample]
};

namespace {

WaveTables buildTables(Waveform w, double sampleRate) {
    WaveTables t;
    t.size = periodicWaveSize(sampleRate);
    t.ranges = static_cast<unsigned>(0.5 + kNumberOfOctaveBands * std::log2(double(t.size)));
    const unsigned maxPartials = t.size / 2;
    t.lowestFundamental = (0.5 * sampleRate) / double(maxPartials);   // == sr / size

    // Sine lookup indexed by (n*k) mod size, so building the tables costs
    // table reads rather than millions of std::sin calls.
    std::vector<double> sinLut(t.size);
    for (unsigned k = 0; k < t.size; ++k)
        sinLut[k] = std::sin(2.0 * kPi * double(k) / double(t.size));

    t.table.resize(t.ranges);
    for (unsigned r = 0; r < t.ranges; ++r) {
        const double cullingScale = std::pow(2.0, -(double(r) * kCentsPerRange) / 1200.0);
        unsigned partials = static_cast<unsigned>(cullingScale * double(maxPartials));
        if (partials < 1) partials = 1;

        auto& out = t.table[r];
        out.assign(t.size, 0.0f);
        for (unsigned n = 1; n <= partials; ++n) {
            const double b = partialAmplitude(w, n);
            if (b == 0.0) continue;
            for (unsigned k = 0; k < t.size; ++k)
                out[k] += static_cast<float>(b * sinLut[(n * k) % t.size]);
        }
    }

    // Blink normalises every range by the peak of range 0, so relative level
    // stays consistent as ranges change under a glide.
    double maxValue = 0.0;
    for (float v : t.table[0]) maxValue = std::max(maxValue, std::abs(double(v)));
    if (maxValue > 0.0) {
        const float scale = static_cast<float>(1.0 / maxValue);
        for (auto& tab : t.table)
            for (float& v : tab) v *= scale;
    }
    return t;
}

const WaveTables& tablesFor(Waveform w, double sampleRate) {
    // Keyed by sample rate, and never evicted. That is the whole point: an
    // oscillator holds the returned reference for the life of a note and reads
    // through it every sample, so an entry can never be freed or moved while
    // anything might still be looking at it.
    //
    // This used to keep one rate and rebuild when asked for another, which was
    // safe only while the assumption in the old comment held -- that rate
    // changes happen off the audio thread during prepare. The sample-match
    // search broke it: its workers render at 22.05 kHz on a thread pool while
    // the audio thread renders at 48 kHz, so each cleared the cache the other
    // was reading from. Crash, reported as loading a sample, searching, then
    // playing a note.
    //
    // The mutex was never the missing piece. It made the mutation atomic; what
    // escaped it was the lifetime of the reference handed back.
    //
    // std::map rather than a vector because its nodes are stable: inserting a
    // new rate cannot move the entries already handed out.
    static std::mutex mtx;
    static std::map<double, std::vector<WaveTables>> cache;

    std::lock_guard<std::mutex> lock(mtx);
    auto it = cache.find(sampleRate);
    if (it == cache.end()) {
        std::vector<WaveTables> built;
        built.reserve(4);
        for (int i = 0; i < 4; ++i)
            built.push_back(buildTables(static_cast<Waveform>(i), sampleRate));
        it = cache.emplace(sampleRate, std::move(built)).first;
    }
    const int idx = static_cast<int>(w);
    return it->second[static_cast<size_t>(idx >= 0 && idx < 4 ? idx : 0)];
}

// size is always a power of two (2048, 4096 or 16384), so the wrap is a mask
// rather than two integer divisions, and pos is non-negative so the fractional
// part needs no floor.
float readTable(const std::vector<float>& tab, double phase, unsigned size) {
    const double pos = phase * double(size);
    const unsigned ip = static_cast<unsigned>(pos);
    const unsigned mask = size - 1u;
    const unsigned i0 = ip & mask;
    const unsigned i1 = (ip + 1u) & mask;
    const double frac = pos - double(ip);
    return static_cast<float>(tab[i0] + (tab[i1] - tab[i0]) * frac);
}

} // namespace

// Defined at namespace scope, not in the anonymous namespace: the header
// forward-declares sl::WaveTables so WaOscillator can cache a pointer.

void WaOscillator::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    phase_ = 0.0;
    tablesFor(Waveform::Sine, sampleRate);   // build once, off the audio thread
    tables_ = &tablesFor(type_, sampleRate);
}

void WaOscillator::setType(Waveform w) {
    type_ = w;
    // Resolve the table set here, not in render(). tablesFor takes a mutex, and
    // taking one per oscillator per sample -- main oscillators, LFOs and FM
    // modulators alike -- was a large share of the voice path's cost.
    tables_ = &tablesFor(w, sampleRate_);
}

double WaOscillator::render(double freqHz) {
    if (!tables_) tables_ = &tablesFor(type_, sampleRate_);
    const WaveTables& t = *tables_;

    // Blink's range selection, verbatim in spirit:
    //   ratio  = f / lowestFundamental   (0.5 when f <= 0)
    //   cents  = log2(ratio) * 1200
    //   range  = 1 + cents / centsPerRange
    //
    // log2 is a libm call, and for any oscillator that is not being modulated
    // the frequency is identical sample after sample, so the result is cached
    // and only recomputed when the frequency actually moves.
    if (freqHz != lastFreq_) {
        lastFreq_ = freqHz;
        const double absFreq = std::abs(freqHz);
        const double ratio = absFreq > 0.0 ? absFreq / t.lowestFundamental : 0.5;
        const double pitchRange = 1.0 + (std::log2(ratio) * 1200.0) / kCentsPerRange;

        if (pitchRange <= 0.0) {
            rangeI1_ = 0;
            rangeInterp_ = 0.0;
        } else if (pitchRange >= double(t.ranges - 1)) {
            rangeI1_ = t.ranges - 1;
            rangeInterp_ = 0.0;
        } else {
            rangeI1_ = static_cast<unsigned>(pitchRange);
            rangeInterp_ = pitchRange - double(rangeI1_);
        }
        rangeI2_ = rangeI1_ + 1u < t.ranges ? rangeI1_ + 1u : rangeI1_;
    }
    const unsigned i1 = rangeI1_;
    const unsigned i2 = rangeI2_;
    const double interp = rangeInterp_;

    const double a = readTable(t.table[i1], phase_, t.size);
    const double b = readTable(t.table[i2], phase_, t.size);
    const double out = a + (b - a) * interp;

    phase_ += freqHz / sampleRate_;
    phase_ -= std::floor(phase_);   // handles negative frequencies too

    return out;
}

const std::vector<float>& globalNoiseBuffer(double sampleRate) {
    // Per rate and never evicted, for the same reason as the wavetables above:
    // NoiseSource keeps the returned reference and reads through it every
    // sample, so resizing a shared buffer underneath it is a use-after-free the
    // moment two sample rates are in play at once.
    static std::mutex mtx;
    static std::map<double, std::vector<float>> buffers;

    std::lock_guard<std::mutex> lock(mtx);
    auto it = buffers.find(sampleRate);
    if (it == buffers.end()) {
        std::vector<float>& buffer = buffers[sampleRate];
        // Fixed seed, not the instrument seed: zyn keeps ONE global noise
        // buffer shared by every voice and instrument, and
        // tools/export-reference-audio.mjs overrides Z.randSample with this
        // identical stream so the reference audio matches.
        Mulberry32 r(0x5EED1A7Eu);
        const size_t n = static_cast<size_t>(2.0 * sampleRate);
        buffer.resize(n);
        for (size_t i = 0; i < n; ++i)
            buffer[i] = static_cast<float>(r(2.0) - 1.0);
        it = buffers.find(sampleRate);
    }
    return it->second;
}

void NoiseSource::prepare(double sampleRate) {
    buffer_ = &globalNoiseBuffer(sampleRate);
    pos_ = 0;
}

double NoiseSource::render() {
    if (!buffer_ || buffer_->empty()) return 0.0;
    const double v = (*buffer_)[pos_];
    if (++pos_ >= buffer_->size()) pos_ = 0;
    return v;
}

} // namespace sl
