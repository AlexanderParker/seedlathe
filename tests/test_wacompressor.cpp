#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaCompressor.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

std::vector<float> readF32(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    const std::streamsize bytes = f.tellg();
    f.seekg(0);
    std::vector<float> v(static_cast<size_t>(bytes) / sizeof(float));
    f.read(reinterpret_cast<char*>(v.data()), bytes);
    return v;
}

sl::WaCompressor zynCompressor() {
    sl::WaCompressor c;
    c.prepare(48000.0);
    c.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);   // exactly zyn's Z.init settings
    return c;
}

double dbOf(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

} // namespace

// The real test: run the identical stimulus through the C++ port and compare
// against what Chrome's own DynamicsCompressorNode produced. Hand-computed
// expectations would only prove the port agrees with my arithmetic; this proves
// it agrees with the browser.
TEST_CASE("compressor matches Chrome's DynamicsCompressorNode") {
    const std::string dir = std::string(SL_VECTORS_DIR) + "/compressor/";
    const auto in = readF32(dir + "input.f32");
    const auto ref = readF32(dir + "output.f32");
    REQUIRE(in.size() == ref.size());
    REQUIRE(in.size() > 100000u);

    auto c = zynCompressor();
    std::vector<double> got(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        double l, r;
        c.process(in[i], in[i], l, r);
        got[i] = l;
        REQUIRE(std::isfinite(l));
    }

    // Compare in dB on the envelope rather than sample-by-sample: Chrome
    // renders in 128-frame quanta and computes envelopes per 32-frame
    // division, so exact per-sample equality is not the right bar. What must
    // match is the gain trajectory.
    const size_t win = 512;
    double worstDb = 0.0;
    size_t worstAt = 0;
    for (size_t start = 0; start + win <= in.size(); start += win) {
        double sumRef = 0.0, sumGot = 0.0;
        for (size_t i = start; i < start + win; ++i) {
            sumRef += double(ref[i]) * double(ref[i]);
            sumGot += got[i] * got[i];
        }
        const double rmsRef = std::sqrt(sumRef / double(win));
        const double rmsGot = std::sqrt(sumGot / double(win));
        if (rmsRef < 1e-5 && rmsGot < 1e-5) continue;   // both silent
        const double diff = std::abs(dbOf(rmsGot) - dbOf(rmsRef));
        if (diff > worstDb) { worstDb = diff; worstAt = start; }
    }

    // Measured worst case is 0.00092 dB across the whole stimulus. The gate is
    // set an order of magnitude above that: tight enough that any real
    // regression trips it, loose enough to absorb float-vs-double noise.
    INFO("worst window RMS error " << worstDb << " dB at frame " << worstAt);
    REQUIRE(worstDb < 0.01);
}

TEST_CASE("compressor applies makeup gain to quiet signal") {
    // A signal below threshold comes out LOUDER, because Blink derives a
    // makeup gain of pow(1/saturate(1,k), 0.6) -- about +4.8 dB at zyn's
    // settings. A generic compressor would leave it alone and every seed
    // would be quiet by that amount.
    auto c = zynCompressor();
    double peak = 0.0, l, r;
    for (int i = 0; i < 96000; ++i) {
        const double x = 0.05 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        c.process(x, x, l, r);
        if (i > 48000) peak = std::max(peak, std::abs(l));
    }
    const double gainDb = dbOf(peak) - dbOf(0.05);
    INFO("quiet-signal gain " << gainDb << " dB");
    REQUIRE(gainDb > 3.0);
    REQUIRE(gainDb < 7.0);
}

TEST_CASE("compressor reduces a loud signal and reports reduction") {
    auto c = zynCompressor();
    double peak = 0.0, l, r;
    for (int i = 0; i < 144000; ++i) {
        const double x = std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        c.process(x, x, l, r);
        if (i > 96000) peak = std::max(peak, std::abs(l));
    }
    REQUIRE(peak < 1.0);
    REQUIRE(c.reduction() < 0.0);
}

TEST_CASE("compressor output is always finite under abuse") {
    auto c = zynCompressor();
    double l, r;
    for (int i = 0; i < 48000; ++i) {
        const double x = (i % 2 == 0) ? 8.0 : -8.0;   // square, far over unity
        c.process(x, x, l, r);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
    }
    for (int i = 0; i < 4800; ++i) {
        c.process(0.0, 0.0, l, r);
        REQUIRE(std::isfinite(l));
    }
}

TEST_CASE("compressor pre-delay is six milliseconds") {
    // The kernel delays the signal by 288 frames at 48 kHz so it can compute
    // gain from the undelayed input -- the lookahead.
    auto c = zynCompressor();
    double l, r;
    int firstNonZero = -1;
    for (int i = 0; i < 1000; ++i) {
        c.process(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0, l, r);
        if (std::abs(l) > 1e-6 && firstNonZero < 0) firstNonZero = i;
    }
    REQUIRE(firstNonZero == 288);
}
