#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaOscillator.h"
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
