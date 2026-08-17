#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaNodes.h"
#include "sl/Mulberry32.h"
#include <algorithm>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE("delay returns an impulse after the delay time") {
    sl::WaDelay d;
    d.prepare(48000.0, 0.5);
    d.setDelayTime(0.01);   // 480 samples
    d.setFeedback(0.0);

    int first = -1;
    for (int i = 0; i < 1000; ++i) {
        const double out = d.process(i == 0 ? 1.0 : 0.0);
        if (out > 0.5 && first < 0) first = i;
    }
    REQUIRE(first >= 479);
    REQUIRE(first <= 481);
}

TEST_CASE("delay feedback decays geometrically") {
    sl::WaDelay d;
    d.prepare(48000.0, 0.5);
    d.setDelayTime(0.01);
    d.setFeedback(0.5);

    double taps[3] = {0, 0, 0};
    int found = 0;
    for (int i = 0; i < 2000 && found < 3; ++i) {
        const double out = d.process(i == 0 ? 1.0 : 0.0);
        if (out > 1e-3) taps[found++] = out;
    }
    REQUIRE(found == 3);
    REQUIRE_THAT(taps[1] / taps[0], WithinAbs(0.5, 0.05));
    REQUIRE_THAT(taps[2] / taps[1], WithinAbs(0.5, 0.05));
}

TEST_CASE("delay with maximum feedback stays bounded") {
    // zyn draws feedback up to 0.8 and shares one delay node across every
    // voice, so a runaway loop here would take the whole master bus with it.
    sl::WaDelay d;
    d.prepare(48000.0, 0.5);
    d.setDelayTime(0.05);
    d.setFeedback(0.8);
    double peak = 0.0;
    for (int i = 0; i < 48000 * 4; ++i) {
        const double out = d.process(i < 100 ? 1.0 : 0.0);
        REQUIRE(std::isfinite(out));
        peak = std::max(peak, std::abs(out));
    }
    REQUIRE(peak < 10.0);
}

TEST_CASE("convolver impulse is deterministic for a given seed") {
    sl::WaConvolver a, b;
    a.prepare(48000.0);
    b.prepare(48000.0);
    a.buildImpulse(0.5, 0.8, 12345u);
    b.buildImpulse(0.5, 0.8, 12345u);
    REQUIRE(a.ready());

    for (int i = 0; i < 2000; ++i) {
        double al, ar, bl, br;
        a.process(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0, al, ar);
        b.process(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0, bl, br);
        REQUIRE(al == bl);
        REQUIRE(ar == br);
    }
}

TEST_CASE("convolver differs between seeds and produces a decaying tail") {
    sl::WaConvolver a, b;
    a.prepare(48000.0);
    b.prepare(48000.0);
    a.buildImpulse(0.5, 0.8, 1u);
    b.buildImpulse(0.5, 0.8, 2u);

    double diff = 0.0, earlyEnergy = 0.0, lateEnergy = 0.0;
    for (int i = 0; i < 24000; ++i) {
        double al, ar, bl, br;
        const double x = (i == 0) ? 1.0 : 0.0;
        a.process(x, x, al, ar);
        b.process(x, x, bl, br);
        REQUIRE(std::isfinite(al));
        diff += std::abs(al - bl);
        if (i < 4000) earlyEnergy += al * al;
        else if (i > 16000) lateEnergy += al * al;
    }
    REQUIRE(diff > 0.0);
    REQUIRE(earlyEnergy > lateEnergy);   // the tail decays
}

TEST_CASE("convolver is silent before an impulse is built") {
    sl::WaConvolver c;
    c.prepare(48000.0);
    REQUIRE_FALSE(c.ready());
    double l, r;
    c.process(1.0, 1.0, l, r);
    REQUIRE(l == 0.0);
    REQUIRE(r == 0.0);
}

TEST_CASE("panner is equal-power and centred at unity") {
    double l, r;
    sl::WaPanner::pan(1.0, 0.0, l, r);
    REQUIRE_THAT(l, WithinAbs(0.7071, 0.001));
    REQUIRE_THAT(r, WithinAbs(0.7071, 0.001));

    sl::WaPanner::pan(1.0, -1.0, l, r);
    REQUIRE_THAT(l, WithinAbs(1.0, 0.001));
    REQUIRE_THAT(r, WithinAbs(0.0, 0.001));

    sl::WaPanner::pan(1.0, 1.0, l, r);
    REQUIRE_THAT(l, WithinAbs(0.0, 0.001));
    REQUIRE_THAT(r, WithinAbs(1.0, 0.001));
}

TEST_CASE("getDistCurve is finite, monotonic and odd-symmetric") {
    const auto c = sl::getDistCurve(100.0, 48000.0);
    REQUIRE(c.size() == 48000u);
    for (float v : c) REQUIRE(std::isfinite(v));
    for (size_t i = 1; i < c.size(); ++i) REQUIRE(c[i] >= c[i - 1] - 1e-6f);
    REQUIRE_THAT(double(c.front() + c.back()), WithinAbs(0.0, 1e-4));
}

TEST_CASE("shaper maps the curve and stays bounded") {
    sl::WaShaper s;
    s.setCurve(100.0, 48000.0);
    s.setOversample(1);
    // Not exactly zero, and correctly so: WaveShaperNode maps input to
    // index (x+1)/2*(n-1), and with an even-length curve x=0 lands on index
    // 23999.5 -- between two points either side of zero. Chrome behaves the
    // same way. The offset is ~2e-4, so the tolerance reflects that.
    REQUIRE_THAT(s.process(0.0), WithinAbs(0.0, 1e-3));
    REQUIRE(s.process(1.0) > s.process(0.5));
    REQUIRE(s.process(-1.0) < s.process(-0.5));
    // Out-of-range input clamps instead of reading past the curve.
    REQUIRE(std::isfinite(s.process(5.0)));
    REQUIRE(std::isfinite(s.process(-5.0)));
}

TEST_CASE("shaper with no curve set is transparent") {
    sl::WaShaper s;
    REQUIRE(s.process(0.42) == 0.42);
}

TEST_CASE("convolver matches brute-force direct convolution") {
    // The correctness proof for non-uniform partitioning, and it must convolve
    // against an INDEPENDENTLY generated impulse. An earlier version recovered
    // the impulse from the convolver itself, which only proved the engine
    // agreed with itself -- it passed while a whole segment of the tail was
    // arriving 24576 samples early.
    const double sr = 48000.0;
    const uint32_t seed = 4242u;

    // Long enough to span the direct head and several FFT segments, including
    // one past the block-size cap where the output delay compensation applies.
    for (double duration : {0.005, 0.05, 0.5, 1.2}) {
        const std::vector<float> h = sl::makeReverbImpulse(duration, 0.7, seed, sr);

        sl::WaConvolver c;
        c.prepare(sr);
        c.buildImpulse(duration, 0.7, seed);
        REQUIRE(c.ready());

        std::vector<double> x(4000);
        sl::Mulberry32 rng(99u);
        for (auto& v : x) v = rng(2.0) - 1.0;

        std::vector<double> got(x.size());
        for (size_t i = 0; i < x.size(); ++i) {
            double l, r;
            c.process(x[i], x[i], l, r);
            got[i] = l;
        }

        double worst = 0.0, peak = 0.0;
        for (size_t i = 0; i < x.size(); ++i) {
            double want = 0.0;
            const size_t kMax = std::min(i + 1, h.size());
            for (size_t k = 0; k < kMax; ++k) want += double(h[k]) * x[i - k];
            peak = std::max(peak, std::abs(want));
            worst = std::max(worst, std::abs(want - got[i]));
        }
        INFO("duration " << duration << " worst " << worst << " peak " << peak);
        REQUIRE(peak > 1e-6);
        REQUIRE(worst < peak * 1e-3);
    }
}
