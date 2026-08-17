#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaBiquad.h"
#include <algorithm>
#include <cmath>

using Catch::Matchers::WithinAbs;

TEST_CASE("lowpass magnitude matches the analytic RBJ response") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Lowpass, 1000.0, 0.7071, 0.0);
    REQUIRE_THAT(f.magnitudeAt(100.0),  WithinAbs(1.0, 0.01));
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(0.7071, 0.02));   // -3 dB at cutoff
    REQUIRE(f.magnitudeAt(10000.0) < 0.02);
}

TEST_CASE("highpass mirrors lowpass") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Highpass, 1000.0, 0.7071, 0.0);
    REQUIRE(f.magnitudeAt(100.0) < 0.02);
    REQUIRE_THAT(f.magnitudeAt(1000.0),  WithinAbs(0.7071, 0.02));
    REQUIRE_THAT(f.magnitudeAt(20000.0), WithinAbs(1.0, 0.05));
}

TEST_CASE("bandpass peaks at the centre frequency") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Bandpass, 1000.0, 4.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 0.02));
    REQUIRE(f.magnitudeAt(100.0) < 0.3);
    REQUIRE(f.magnitudeAt(10000.0) < 0.3);
}

TEST_CASE("allpass is flat in magnitude") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Allpass, 1000.0, 2.0, 0.0);
    for (double hz : {50.0, 500.0, 1000.0, 5000.0, 20000.0}) {
        INFO("at " << hz << " Hz");
        REQUIRE_THAT(f.magnitudeAt(hz), WithinAbs(1.0, 0.01));
    }
}

TEST_CASE("shelf and peaking are flat at zero gain") {
    // zyn never sets a filter gain, so A == 1 for every generated instrument.
    // These types are still selected by the generator, so they must reduce to
    // a passthrough rather than being skipped.
    for (auto t : {sl::FilterType::Lowshelf, sl::FilterType::Highshelf,
                   sl::FilterType::Peaking}) {
        sl::WaBiquad f;
        f.prepare(48000.0);
        f.setCoefficients(t, 1000.0, 3.0, 0.0);
        for (double hz : {100.0, 1000.0, 10000.0}) {
            INFO("type " << static_cast<int>(t) << " at " << hz << " Hz");
            REQUIRE_THAT(f.magnitudeAt(hz), WithinAbs(1.0, 0.01));
        }
    }
}

TEST_CASE("cutoff of zero silences a lowpass instead of exploding") {
    // zyn's filter envelope starts every note at 0 Hz, so this path runs on
    // every single note of every seed.
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Lowpass, 0.0, 5.0, 0.0);
    double peak = 0.0;
    for (int i = 0; i < 4800; ++i)
        peak = std::max(peak, std::abs(f.process(std::sin(i * 0.1))));
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak < 1e-6);
}

TEST_CASE("cutoff of zero passes a highpass through") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Highpass, 0.0, 5.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 1e-9));
}

TEST_CASE("cutoff above nyquist passes a lowpass through and silences a highpass") {
    sl::WaBiquad lp, hp;
    lp.prepare(48000.0);
    hp.prepare(48000.0);
    lp.setCoefficients(sl::FilterType::Lowpass, 30000.0, 0.7071, 0.0);
    hp.setCoefficients(sl::FilterType::Highpass, 30000.0, 0.7071, 0.0);
    REQUIRE_THAT(lp.magnitudeAt(1000.0), WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(hp.magnitudeAt(1000.0), WithinAbs(0.0, 1e-9));
}

TEST_CASE("filter stays stable across a full envelope sweep at high Q") {
    // zyn sweeps cutoff 0 -> 20000 Hz on every note with Q driven up to 30.
    sl::WaBiquad f;
    f.prepare(48000.0);
    double peak = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const double freq = 20000.0 * (double(i) / 48000.0);
        f.setCoefficients(sl::FilterType::Lowpass, freq, 30.0, 0.0);
        const double y = f.process(std::sin(i * 0.05));
        REQUIRE(std::isfinite(y));
        peak = std::max(peak, std::abs(y));
    }
    REQUIRE(peak < 100.0);   // resonance may ring hot; it must not diverge
}

TEST_CASE("every filter type survives a sweep without producing NaN") {
    for (int t = 0; t < 7; ++t) {
        sl::WaBiquad f;
        f.prepare(48000.0);
        for (int i = 0; i < 8000; ++i) {
            f.setCoefficients(static_cast<sl::FilterType>(t),
                              20000.0 * (double(i) / 8000.0), 30.0, 0.0);
            const double y = f.process(std::sin(i * 0.05));
            INFO("type " << t << " sample " << i);
            REQUIRE(std::isfinite(y));
        }
    }
}
