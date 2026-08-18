#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
#include "webaudio/WaOscillator.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Power per DFT bin over exactly N samples.
std::vector<double> binPower(const std::vector<double>& x) {
    const size_t N = x.size();
    std::vector<double> p(N / 2, 0.0);
    for (size_t k = 0; k < N / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < N; ++n) {
            const double a = -2.0 * kPi * double(k) * double(n) / double(N);
            re += x[n] * std::cos(a);
            im += x[n] * std::sin(a);
        }
        p[k] = re * re + im * im;
    }
    return p;
}

std::vector<double> renderOsc(sl::Waveform w, double freq, size_t n, double sr) {
    sl::WaOscillator o;
    o.prepare(sr);
    o.setType(w);
    std::vector<double> x(n);
    for (auto& s : x) s = o.render(freq);
    return x;
}

} // namespace

TEST_CASE("sine oscillator produces a clean sine at unity peak") {
    // Bin-exact fundamental. At a non-integer bin a perfect sine still smears
    // ~6% of its power outside +/-2 bins under a rectangular window, which
    // measures the DFT rather than the oscillator.
    const size_t N = 1024;
    const double sr = 48000.0;
    const size_t kFund = 64;
    const double f0 = sr * double(kFund) / double(N);   // 3000 Hz

    const auto x = renderOsc(sl::Waveform::Sine, f0, N, sr);
    double peak = 0.0;
    for (double s : x) peak = std::max(peak, std::abs(s));
    REQUIRE_THAT(peak, WithinAbs(1.0, 0.02));

    const auto p = binPower(x);
    double total = 0.0;
    for (size_t k = 1; k < p.size(); ++k) total += p[k];
    REQUIRE(p[kFund] / total > 0.999);
}

TEST_CASE("sawtooth is band-limited: energy only at harmonic bins") {
    // A naive ramp folds every partial above Nyquist back onto NON-harmonic
    // frequencies. That, not the amount of high-frequency energy, is what
    // band-limiting prevents: a correct saw at this pitch still puts a good
    // fraction of its power above Nyquist/2, because those harmonics belong there.
    const size_t N = 1024;
    const double sr = 48000.0;
    const size_t kBin = 100;                       // bin-exact fundamental
    const double f0 = sr * double(kBin) / double(N);   // 4687.5 Hz

    const auto p = binPower(renderOsc(sl::Waveform::Sawtooth, f0, N, sr));

    double harmonic = 0.0, other = 0.0;
    for (size_t k = 1; k < p.size(); ++k) {
        if (k % kBin == 0) harmonic += p[k];
        else other += p[k];
    }
    REQUIRE(harmonic > 0.0);
    INFO("non-harmonic fraction = " << other / (harmonic + other));
    REQUIRE(other / (harmonic + other) < 0.01);
}

TEST_CASE("square and triangle are band-limited too") {
    const size_t N = 1024;
    const double sr = 48000.0;
    const size_t kBin = 100;
    const double f0 = sr * double(kBin) / double(N);

    for (auto w : {sl::Waveform::Square, sl::Waveform::Triangle}) {
        const auto p = binPower(renderOsc(w, f0, N, sr));
        double harmonic = 0.0, other = 0.0;
        for (size_t k = 1; k < p.size(); ++k) {
            if (k % kBin == 0) harmonic += p[k];
            else other += p[k];
        }
        REQUIRE(harmonic > 0.0);
        REQUIRE(other / (harmonic + other) < 0.01);
    }
}

TEST_CASE("waveform partial structure matches the Fourier series") {
    // Square has odd partials only; sawtooth has both. This distinguishes a
    // real Fourier construction from a lookup of the wrong shape.
    const size_t N = 1024;
    const double sr = 48000.0;
    const size_t kBin = 64;
    const double f0 = sr * double(kBin) / double(N);   // 3000 Hz, 8 partials fit

    const auto sq = binPower(renderOsc(sl::Waveform::Square, f0, N, sr));
    const auto sw = binPower(renderOsc(sl::Waveform::Sawtooth, f0, N, sr));

    // Second harmonic: absent for square, present for sawtooth.
    REQUIRE(sq[2 * kBin] / sq[kBin] < 1e-6);
    REQUIRE(sw[2 * kBin] / sw[kBin] > 0.1);
}

TEST_CASE("oscillator peak stays near unity across the range") {
    for (double f : {50.0, 220.0, 1000.0, 5000.0, 12000.0}) {
        const auto x = renderOsc(sl::Waveform::Sawtooth, f, 4096, 48000.0);
        double peak = 0.0;
        for (double s : x) peak = std::max(peak, std::abs(s));
        INFO("freq " << f << " peak " << peak);
        REQUIRE(peak > 0.4);
        REQUIRE(peak < 1.6);
    }
}

TEST_CASE("noise buffer is deterministic, shared and full-scale") {
    const auto& a = sl::globalNoiseBuffer(48000.0);
    const auto& b = sl::globalNoiseBuffer(48000.0);
    REQUIRE(&a == &b);                 // one global buffer, as in zyn
    REQUIRE(a.size() == 96000u);       // 2 seconds
    double peak = 0.0;
    for (float s : a) peak = std::max(peak, std::abs(double(s)));
    REQUIRE(peak <= 1.0);
    REQUIRE(peak > 0.9);
}

TEST_CASE("noise source loops and ignores frequency") {
    sl::NoiseSource n1, n2;
    n1.prepare(48000.0);
    n2.prepare(48000.0);
    for (int i = 0; i < 100; ++i) REQUIRE(n1.render() == n2.render());

    // Wraps at the 2 s buffer boundary rather than running off the end.
    sl::NoiseSource w;
    w.prepare(48000.0);
    const double first = w.render();
    for (int i = 0; i < 96000 - 1; ++i) w.render();
    REQUIRE(w.render() == first);
}

TEST_CASE("the wavetable read wraps cleanly at the end of the table") {
    // readTable interpolates between index i and i+1, and at the last sample
    // i+1 has to wrap to 0. Without the mask it reads one float past the end of
    // the vector -- which does not crash and does not produce a NaN, so nothing
    // else here would notice. What it does produce is a discontinuity once per
    // cycle, so continuity is the thing to assert.
    sl::WaOscillator osc;
    osc.prepare(48000.0);
    osc.setType(sl::Waveform::Sine);

    // A frequency that lands on the wrap often, and low enough that a real sine
    // moves only a little between samples.
    const double hz = 220.0;
    double prev = osc.render(hz);
    double biggestStep = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const double y = osc.render(hz);
        REQUIRE(std::isfinite(y));
        biggestStep = std::max(biggestStep, std::abs(y - prev));
        prev = y;
    }

    // A 220 Hz sine at 48 kHz moves at most about 0.029 per sample. Anything an
    // order of magnitude beyond that is a read off the end of the table.
    INFO("largest sample-to-sample step: " << biggestStep);
    REQUIRE(biggestStep < 0.1);
}

TEST_CASE("an oscillator stays clean over a long render") {
    // The phase accumulator is wrapped back into [0, 1) every sample. Letting
    // it grow instead costs precision immediately and overflows the table index
    // eventually, but neither shows up in a render of a few thousand samples --
    // which is what every other oscillator test here does.
    sl::WaOscillator osc;
    osc.prepare(48000.0);
    osc.setType(sl::Waveform::Sine);

    const double hz = 440.0;
    double peakEarly = 0.0, peakLate = 0.0;
    double biggestStep = 0.0, prev = osc.render(hz);

    // Thirty seconds. Thirteen million samples is enough for an unwrapped
    // accumulator to have lost most of its mantissa.
    const int total = 48000 * 30;
    for (int i = 0; i < total; ++i) {
        const double y = osc.render(hz);
        REQUIRE(std::isfinite(y));
        biggestStep = std::max(biggestStep, std::abs(y - prev));
        prev = y;
        if (i < 48000) peakEarly = std::max(peakEarly, std::abs(y));
        if (i > total - 48000) peakLate = std::max(peakLate, std::abs(y));
    }

    INFO("peak early " << peakEarly << ", late " << peakLate
         << ", largest step " << biggestStep);
    REQUIRE(peakEarly > 0.9);
    // Still the same waveform half a minute later, not a decayed or aliased one.
    REQUIRE(peakLate == Catch::Approx(peakEarly).epsilon(0.02));
    REQUIRE(biggestStep < 0.1);
}

TEST_CASE("the top wavetable range is not silent") {
    // Each range keeps only the partials that fit below Nyquist, and the
    // highest range can work out to none at all. A floor of one keeps a
    // fundamental there; without it the top table is all zeros and the
    // oscillator goes silent at the top of its range rather than thinning out.
    for (auto type : {sl::Waveform::Sine, sl::Waveform::Sawtooth,
                      sl::Waveform::Square, sl::Waveform::Triangle}) {
        sl::WaOscillator osc;
        osc.prepare(48000.0);
        osc.setType(type);

        // Well into the top range, but still audible.
        double peak = 0.0;
        for (int i = 0; i < 4800; ++i)
            peak = std::max(peak, std::abs(osc.render(15000.0)));

        INFO("waveform " << static_cast<int>(type) << " peak at 15 kHz: " << peak);
        REQUIRE(peak > 0.1);
    }
}

TEST_CASE("two sample rates can render at once") {
    // Reported crash: load a sample, start a search, pick a result, play a note
    // while the search is still running.
    //
    // The sample-match search renders every candidate offline at 22.05 kHz on a
    // worker pool. The audio thread renders at the host rate. Both reach the
    // same process-wide wavetable and noise caches, which used to hold exactly
    // one sample rate and rebuild when asked for another -- so each thread
    // freed the tables the other was reading through. An oscillator holds that
    // reference for the life of a note, so this is a use-after-free rather than
    // a torn read, and it crashed rather than sounding wrong.
    //
    // A mutex was already there and did not help: it made the rebuild atomic
    // while leaving the returned reference dangling.
    std::atomic<bool> stop{false};
    std::atomic<int> renders{0};

    // Stand-ins for the search pool, at the rate it actually uses.
    std::vector<std::thread> searchers;
    for (int t = 0; t < 3; ++t) {
        searchers.emplace_back([&stop, &renders, t] {
            while (!stop.load(std::memory_order_acquire)) {
                sl::WaOscillator osc;
                osc.prepare(22050.0);
                osc.setType(static_cast<sl::Waveform>(t % 4));
                sl::NoiseSource noise;
                noise.prepare(22050.0);
                for (int i = 0; i < 512; ++i) {
                    REQUIRE(std::isfinite(osc.render(330.0)));
                    REQUIRE(std::isfinite(noise.render()));
                }
                renders.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // The audio thread: one long note at the host rate, holding its reference
    // across everything the searchers do.
    sl::WaOscillator voice;
    voice.prepare(48000.0);
    voice.setType(sl::Waveform::Sawtooth);
    sl::NoiseSource voiceNoise;
    voiceNoise.prepare(48000.0);

    double peak = 0.0;
    for (int i = 0; i < 48000 * 2; ++i) {
        const double y = voice.render(220.0);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::isfinite(voiceNoise.render()));
        peak = std::max(peak, std::abs(y));
    }

    stop.store(true, std::memory_order_release);
    for (auto& t : searchers) t.join();

    INFO("searcher renders during the note: " << renders.load());
    REQUIRE(renders.load() > 0);   // the other rate really was in play
    REQUIRE(peak > 0.5);           // and the note came through intact
}
