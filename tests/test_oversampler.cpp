#include <catch2/catch_test_macros.hpp>

#include "Oversampler.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> tone(double freq, double rate, int n) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t>(i)] =
            static_cast<float>(std::sin(2.0 * kPi * freq * double(i) / rate));
    return x;
}

// Peak of the second half, so the filter's start-up transient is excluded.
float settledPeak(const std::vector<float>& x) {
    float peak = 0.f;
    for (size_t i = x.size() / 2; i < x.size(); ++i)
        peak = std::max(peak, std::fabs(x[i]));
    return peak;
}

} // namespace

TEST_CASE("a decimator at 1x copies its input") {
    sl::Decimator d;
    d.prepare(1);
    REQUIRE(d.factor() == 1);
    REQUIRE(d.latencySamples() == 0);

    const auto in = tone(1000.0, 48000.0, 256);
    std::vector<float> out(256);
    d.process(in.data(), out.data(), 256);
    for (size_t i = 0; i < out.size(); ++i) REQUIRE(out[i] == in[i]);
}

TEST_CASE("decimation preserves level at DC") {
    for (int factor : {2, 4}) {
        sl::Decimator d;
        d.prepare(factor);
        REQUIRE(d.factor() == factor);

        const int frames = 512;
        std::vector<float> in(static_cast<size_t>(frames * factor), 1.f);
        std::vector<float> out(frames);
        d.process(in.data(), out.data(), frames);

        INFO("factor " << factor << " settled DC " << out[frames - 1]);
        REQUIRE(std::fabs(out[frames - 1] - 1.f) < 0.01f);
    }
}

TEST_CASE("decimation keeps audio below the output Nyquist") {
    // 1 kHz at 96 kHz decimating to 48 kHz: comfortably in the passband.
    sl::Decimator d;
    d.prepare(2);

    const int frames = 4096;
    const auto in = tone(1000.0, 96000.0, frames * 2);
    std::vector<float> out(frames);
    d.process(in.data(), out.data(), frames);

    const float peak = settledPeak(out);
    INFO("passband peak: " << peak);
    REQUIRE(peak > 0.97f);
    REQUIRE(peak < 1.03f);
}

TEST_CASE("decimation rejects what would otherwise alias") {
    // 34 kHz at 96 kHz would fold to 14 kHz at 48 kHz -- audible, and exactly
    // what oversampling exists to prevent.
    sl::Decimator d;
    d.prepare(2);

    const int frames = 4096;
    const auto in = tone(34000.0, 96000.0, frames * 2);
    std::vector<float> out(frames);
    d.process(in.data(), out.data(), frames);

    const float peak = settledPeak(out);
    INFO("stopband peak: " << peak);
    REQUIRE(peak < 0.001f);       // better than 60 dB down
}

TEST_CASE("4x decimation rejects across both halvings") {
    // 70 kHz at 192 kHz, down to 48 kHz.
    sl::Decimator d;
    d.prepare(4);

    const int frames = 2048;
    const auto in = tone(70000.0, 192000.0, frames * 4);
    std::vector<float> out(frames);
    d.process(in.data(), out.data(), frames);

    INFO("stopband peak: " << settledPeak(out));
    REQUIRE(settledPeak(out) < 0.001f);
    REQUIRE(d.latencySamples() > 0);
}

TEST_CASE("processing in blocks matches processing in one go") {
    // The delay line has to carry across calls, or every block boundary is a
    // discontinuity the user hears as a click.
    const int frames = 1024;
    const auto in = tone(3000.0, 96000.0, frames * 2);

    sl::Decimator whole;
    whole.prepare(2);
    std::vector<float> a(frames);
    whole.process(in.data(), a.data(), frames);

    sl::Decimator split;
    split.prepare(2);
    std::vector<float> b(frames);
    const int half = frames / 2;
    split.process(in.data(), b.data(), half);
    split.process(in.data() + half * 2, b.data() + half, half);

    for (int i = 0; i < frames; ++i)
        REQUIRE(std::fabs(a[static_cast<size_t>(i)] - b[static_cast<size_t>(i)]) < 1e-6f);
}
