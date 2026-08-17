#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Analysis.h"
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {
std::vector<float> tone(double hz, double amp = 0.5, size_t n = 48000, double sr = 48000.0) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i)
        x[i] = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979 * hz * double(i) / sr));
    return x;
}
} // namespace

TEST_CASE("identical signals have zero mel distance") {
    const auto a = sl::melSpectrogram(tone(440.0), 48000.0);
    REQUIRE(a.frames > 0);
    REQUIRE(a.bands == 64);
    REQUIRE_THAT(sl::melDistanceDb(a, a), WithinAbs(0.0, 1e-9));
}

TEST_CASE("mel distance grows with pitch difference") {
    const auto a = sl::melSpectrogram(tone(440.0), 48000.0);
    const auto near = sl::melSpectrogram(tone(466.0), 48000.0);
    const auto far = sl::melSpectrogram(tone(880.0), 48000.0);
    const double dNear = sl::melDistanceDb(a, near);
    const double dFar = sl::melDistanceDb(a, far);
    INFO("near " << dNear << " far " << dFar);
    REQUIRE(dNear > 0.0);
    REQUIRE(dFar > dNear);
}

TEST_CASE("mel distance detects a level change") {
    const auto loud = sl::melSpectrogram(tone(440.0, 0.5), 48000.0);
    const auto quiet = sl::melSpectrogram(tone(440.0, 0.25), 48000.0);
    // Halving amplitude is -6 dB of power everywhere the tone has energy.
    REQUIRE(sl::melDistanceDb(loud, quiet) > 1.0);
}

TEST_CASE("rms envelope distance measures a level offset") {
    std::vector<float> a(48000, 0.5f), b(48000, 0.25f);
    REQUIRE_THAT(sl::rmsEnvelopeDistanceDb(a, b, 48000.0), WithinAbs(6.02, 0.1));
}

TEST_CASE("rms envelope distance ignores frames silent in both signals") {
    // A long shared silent tail must not dilute a real mismatch toward zero.
    std::vector<float> a(48000, 0.0f), b(48000, 0.0f);
    for (size_t i = 0; i < 4800; ++i) { a[i] = 0.5f; b[i] = 0.25f; }
    REQUIRE_THAT(sl::rmsEnvelopeDistanceDb(a, b, 48000.0), WithinAbs(6.02, 0.2));
}

TEST_CASE("rms envelope distance is zero for identical signals") {
    const auto x = tone(440.0);
    REQUIRE_THAT(sl::rmsEnvelopeDistanceDb(x, x, 48000.0), WithinAbs(0.0, 1e-9));
}
