#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
#include "webaudio/WaBiquad.h"
#include <algorithm>
#include <cmath>

using Catch::Matchers::WithinAbs;

TEST_CASE("lowpass Q is interpreted in decibels") {
    // This is the Web Audio rule that a textbook RBJ port gets wrong: for
    // lowpass and highpass the Q parameter is in dB, and Blink applies
    // pow10(Q/20) before using it. zyn drives Q from an envelope scaled to 30,
    // so reading that as linear gets the resonance badly wrong.
    sl::WaBiquad f;
    f.prepare(48000.0);

    // Q = -3.01 dB -> resonance 0.7071 -> the classic -3 dB point at cutoff.
    f.setCoefficients(sl::FilterType::Lowpass, 1000.0, -3.0103, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(0.7071, 0.01));

    // Q = 0 dB -> resonance 1.0 -> unity at cutoff, not -3 dB.
    f.setCoefficients(sl::FilterType::Lowpass, 1000.0, 0.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 0.02));

    // Q = 20 dB -> resonance 10 -> a 10x resonant peak.
    f.setCoefficients(sl::FilterType::Lowpass, 1000.0, 20.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(10.0, 0.5));
}

TEST_CASE("lowpass still rolls off above cutoff") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Lowpass, 1000.0, -3.0103, 0.0);
    REQUIRE_THAT(f.magnitudeAt(100.0), WithinAbs(1.0, 0.01));
    REQUIRE(f.magnitudeAt(10000.0) < 0.02);
}

TEST_CASE("highpass mirrors lowpass and also takes Q in dB") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Highpass, 1000.0, -3.0103, 0.0);
    REQUIRE(f.magnitudeAt(100.0) < 0.02);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(0.7071, 0.02));
    REQUIRE_THAT(f.magnitudeAt(20000.0), WithinAbs(1.0, 0.05));
}

TEST_CASE("bandpass Q is linear, not decibels") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Bandpass, 1000.0, 4.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 0.02));   // 0 dB peak gain
    REQUIRE(f.magnitudeAt(100.0) < 0.3);
    REQUIRE(f.magnitudeAt(10000.0) < 0.3);
}

TEST_CASE("bandpass at Q zero passes through rather than silencing") {
    // zyn's Q envelope starts every note at 0, so this is hit on every note,
    // not in some corner case. Blink returns a passthrough here; clamping Q to
    // a tiny positive number instead would produce a razor-thin band and
    // effectively mute the attack.
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Bandpass, 1000.0, 0.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(100.0), WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(f.magnitudeAt(10000.0), WithinAbs(1.0, 1e-9));
}

TEST_CASE("allpass is flat, and inverts at Q zero") {
    sl::WaBiquad f;
    f.prepare(48000.0);
    f.setCoefficients(sl::FilterType::Allpass, 1000.0, 2.0, 0.0);
    for (double hz : {50.0, 500.0, 1000.0, 5000.0, 20000.0}) {
        INFO("at " << hz << " Hz");
        REQUIRE_THAT(f.magnitudeAt(hz), WithinAbs(1.0, 0.01));
    }
    f.setCoefficients(sl::FilterType::Allpass, 1000.0, 0.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 1e-9));   // -1 gain
}

TEST_CASE("shelf and peaking are flat at zero gain regardless of Q") {
    // zyn never sets a filter gain, so A == 1 for every generated instrument.
    // The shelves ignore Q entirely in Blink (S is fixed at 1), so a Q sweep
    // must not change anything here.
    for (auto t : {sl::FilterType::Lowshelf, sl::FilterType::Highshelf,
                   sl::FilterType::Peaking}) {
        for (double q : {0.5, 3.0, 30.0}) {
            sl::WaBiquad f;
            f.prepare(48000.0);
            f.setCoefficients(t, 1000.0, q, 0.0);
            for (double hz : {100.0, 1000.0, 10000.0}) {
                INFO("type " << static_cast<int>(t) << " Q " << q << " at " << hz << " Hz");
                REQUIRE_THAT(f.magnitudeAt(hz), WithinAbs(1.0, 0.01));
            }
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

TEST_CASE("edge frequencies follow Blink per filter type") {
    sl::WaBiquad f;
    f.prepare(48000.0);

    f.setCoefficients(sl::FilterType::Highpass, 0.0, 5.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 1e-9));   // passthrough at DC

    f.setCoefficients(sl::FilterType::Lowpass, 30000.0, 0.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(1.0, 1e-9));   // passthrough at nyquist

    f.setCoefficients(sl::FilterType::Highpass, 30000.0, 0.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(0.0, 1e-9));   // silence at nyquist

    f.setCoefficients(sl::FilterType::Bandpass, 0.0, 5.0, 0.0);
    REQUIRE_THAT(f.magnitudeAt(1000.0), WithinAbs(0.0, 1e-9));   // silence at DC
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
    REQUIRE(peak < 100.0);
}

TEST_CASE("every filter type survives a sweep without producing NaN") {
    for (int t = 0; t < 7; ++t) {
        sl::WaBiquad f;
        f.prepare(48000.0);
        for (int i = 0; i < 8000; ++i) {
            f.setCoefficients(static_cast<sl::FilterType>(t),
                              20000.0 * (double(i) / 8000.0),
                              30.0 * (double(i) / 8000.0), 0.0);
            const double y = f.process(std::sin(i * 0.05));
            INFO("type " << t << " sample " << i);
            REQUIRE(std::isfinite(y));
        }
    }
}

TEST_CASE("a shelf with real gain boosts its band and ignores Q") {
    // Unreachable from the plugin: zyn generates a filterType per oscillator
    // and then never assigns it, so Voice always builds a lowpass. The shelf
    // and peaking coefficients exist because this is a port of Blink's
    // BiquadFilterNode rather than only the parts zyn happens to reach -- and
    // an untested branch of a port is a branch that quietly rots.
    //
    // The existing shelf test uses zero gain, where A is 1 and the response is
    // flat whatever alpha is. That leaves the alpha expression itself -- which
    // Blink collapses to 0.5*sin(w0)*sqrt(2) by fixing the slope at S = 1 --
    // completely uncovered.
    auto responseAt = [](sl::FilterType type, double gainDb, double q, double hz) {
        sl::WaBiquad f;
        f.prepare(48000.0);
        f.setCoefficients(type, 1000.0, q, gainDb);

        // Drive a sine and measure the settled amplitude.
        double peak = 0.0;
        const int n = 24000;
        for (int i = 0; i < n; ++i) {
            const double x = std::sin(2.0 * 3.14159265358979323846 * hz * i / 48000.0);
            const double y = f.process(x);
            REQUIRE(std::isfinite(y));
            if (i > n / 2) peak = std::max(peak, std::abs(y));
        }
        return peak;
    };

    // A +12 dB lowshelf lifts 100 Hz and leaves 10 kHz alone.
    const double lowBoosted = responseAt(sl::FilterType::Lowshelf, 12.0, 1.0, 100.0);
    const double highUntouched = responseAt(sl::FilterType::Lowshelf, 12.0, 1.0, 10000.0);
    INFO("lowshelf +12 dB: 100 Hz " << lowBoosted << ", 10 kHz " << highUntouched);
    REQUIRE(lowBoosted > 3.0);          // about 4x, which is +12 dB
    REQUIRE(lowBoosted < 4.5);
    REQUIRE(highUntouched > 0.9);
    REQUIRE(highUntouched < 1.1);

    // Highshelf is the mirror image.
    REQUIRE(responseAt(sl::FilterType::Highshelf, 12.0, 1.0, 10000.0) > 3.0);
    REQUIRE(responseAt(sl::FilterType::Highshelf, 12.0, 1.0, 100.0) < 1.1);

    // At the corner a shelf sits at A, the geometric mean of its two plateaus
    // -- half the shelf gain in decibels. Derived rather than measured, so it
    // is not a golden number that drifts with the compiler.
    const double A = std::pow(10.0, 12.0 / 40.0);
    REQUIRE(responseAt(sl::FilterType::Lowshelf, 12.0, 1.0, 1000.0)
            == Catch::Approx(A).epsilon(0.01));

    // And Q does nothing to either, because Blink fixes the slope at S = 1.
    for (double q : {0.0, 1.0, 20.0, 1000.0}) {
        INFO("lowshelf at Q " << q);
        REQUIRE(responseAt(sl::FilterType::Lowshelf, 12.0, q, 100.0)
                == Catch::Approx(lowBoosted).epsilon(1e-9));
    }
}
