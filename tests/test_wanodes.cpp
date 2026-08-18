#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
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

TEST_CASE("a reverb shorter than the direct head does not read past it") {
    // The convolver splits an impulse into a 64-tap direct head and FFT
    // segments over the remainder. An impulse shorter than 64 samples has no
    // remainder, and the head has to be clamped to what exists -- copying a
    // fixed 64 taps out of a 5-tap buffer is an out-of-bounds read.
    //
    // The generator cannot produce this and the designer's slider stops at
    // 0.05 s, but a pasted patch has neither limit: duration is a double
    // straight out of the JSON.
    for (double duration : {1e-6, 1e-5, 1e-4, 0.001}) {
        sl::WaConvolver verb;
        verb.prepare(48000.0);
        verb.buildImpulse(duration, 0.8, 1234u);

        double peak = 0.0;
        for (int i = 0; i < 4096; ++i) {
            verb.addInput(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0);
            verb.advance();
            REQUIRE(std::isfinite(verb.outL()));
            REQUIRE(std::isfinite(verb.outR()));
            peak = std::max(peak, std::abs(verb.outL()));
        }
        INFO("duration " << duration << " peak " << peak);
        REQUIRE(peak < 100.0);        // finite is not enough; it must be sane
    }
}

TEST_CASE("a reverb with no energy in it does not blow up the normalisation") {
    // The impulse is normalised by its own power. A configuration that leaves
    // essentially no energy would divide by almost nothing and scale the result
    // to infinity, so the power is floored before the division.
    for (double decay : {0.0, 1e-9, 60.0}) {
        sl::WaConvolver verb;
        verb.prepare(48000.0);
        verb.buildImpulse(0.5, decay, 99u);

        double peak = 0.0;
        for (int i = 0; i < 8192; ++i) {
            verb.addInput(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0);
            verb.advance();
            REQUIRE(std::isfinite(verb.outL()));
            peak = std::max(peak, std::abs(verb.outL()));
        }
        INFO("decay " << decay << " peak " << peak);
        REQUIRE(peak < 100.0);
    }
}

TEST_CASE("the reverb normalisation carries its sample-rate term") {
    // Blink's CalculateNormalizationScale multiplies by
    // kGainCalibrationSampleRate / sampleRate, and this is a verbatim port of
    // it. At 48 kHz that factor is exactly 1, so every other test in the suite
    // would pass with the term deleted -- which is how the mutation audit found
    // it uncovered.
    //
    // Note what is NOT being asserted. The obvious expectation, equal loudness
    // across rates, is wrong and was the first version of this test: the scale
    // goes as 1/rate while the sample count goes as rate, so the energy of a
    // convolved impulse ends up proportional to 48000/rate rather than
    // constant. That is Blink's behaviour, and the point of a verbatim port is
    // to reproduce it rather than improve on it.
    auto tailEnergy = [](double rate) {
        sl::WaConvolver verb;
        verb.prepare(rate);
        verb.buildImpulse(1.0, 0.7, 7u);

        double energy = 0.0;
        const int frames = static_cast<int>(rate * 2.0);
        for (int i = 0; i < frames; ++i) {
            verb.addInput(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0);
            verb.advance();
            energy += verb.outL() * verb.outL();
        }
        return energy;
    };

    const double at48 = tailEnergy(48000.0);
    const double at441 = tailEnergy(44100.0);
    const double at96 = tailEnergy(96000.0);
    REQUIRE(at48 > 0.0);

    INFO("energy ratios: 44.1k/48k " << (at441 / at48)
         << " (expect " << (48000.0 / 44100.0) << "), 96k/48k "
         << (at96 / at48) << " (expect 0.5)");
    REQUIRE(at441 / at48 == Catch::Approx(48000.0 / 44100.0).epsilon(0.02));
    REQUIRE(at96 / at48 == Catch::Approx(0.5).epsilon(0.02));
}

TEST_CASE("the reverb tail is flushed past the end of the impulse") {
    // The convolver stops working once the input has been silent for longer
    // than the impulse -- but not the moment it passes that, because the FFT
    // delay lines still hold partially accumulated blocks. Cutting at the
    // impulse length instead throws away the last few thousand samples of every
    // tail, which is the quietest and most noticeable part to lose.
    sl::WaConvolver verb;
    verb.prepare(48000.0);
    verb.buildImpulse(0.5, 0.8, 4242u);      // 24000 samples of impulse

    constexpr int kIrLen = 24000;
    constexpr int kPastEnd = 24576;          // three of the largest FFT blocks

    double earlyEnergy = 0.0, lateEnergy = 0.0;
    int lateNonZero = 0;
    for (int i = 0; i < kIrLen + kPastEnd; ++i) {
        verb.addInput(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0);
        verb.advance();
        const double y = verb.outL();
        REQUIRE(std::isfinite(y));
        if (i < kIrLen) {
            earlyEnergy += y * y;
        } else {
            lateEnergy += y * y;
            if (y != 0.0) ++lateNonZero;
        }
    }

    INFO("energy within the impulse " << earlyEnergy << ", after it " << lateEnergy
         << " over " << lateNonZero << " non-zero samples");
    REQUIRE(earlyEnergy > 0.0);

    // Counting samples, not summing energy. A truncated tail still emits the
    // one sample on which the cut-off triggers, so "energy after the impulse is
    // above zero" passes against the very bug this is here to catch -- as the
    // first version of this test did.
    REQUIRE(lateNonZero > 1000);
}
