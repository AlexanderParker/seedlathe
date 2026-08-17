#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "OfflineRender.h"
#include "SampleMatch.h"
#include "WavIO.h"
#include "sl/InstrumentGen.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::vector<float> sine(double freq, double seconds, double rate, float amp = 0.5f) {
    const size_t n = static_cast<size_t>(seconds * rate);
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i)
        x[i] = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 *
                                                 freq * double(i) / rate));
    return x;
}

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace

TEST_CASE("readWavMono round-trips what writeWav produced") {
    const auto inst = sl::generateInstrument(13u);
    const sl::RenderResult r = sl::renderOffline(inst, 0, 1.0, 0.5, 48000.0);
    REQUIRE_FALSE(r.left.empty());

    const std::string path = tempPath("sl_wavio_roundtrip.wav");
    REQUIRE(sl::writeWav(path, r, 48000.0));

    const sl::WavData back = sl::readWavMono(path);
    INFO("error: " << back.error);
    REQUIRE(back.ok);
    REQUIRE(back.channels == 2);
    REQUIRE(back.sampleRate == 48000.0);
    REQUIRE(back.mono.size() == r.left.size());

    // writeWav emits 32-bit float, so the mixdown should be exact to within
    // float addition rounding.
    for (size_t i = 0; i < back.mono.size(); i += 97) {
        const float expected = 0.5f * (r.left[i] + r.right[i]);
        REQUIRE(std::fabs(back.mono[i] - expected) < 1e-6f);
    }
    std::remove(path.c_str());
}

TEST_CASE("readWavMono refuses a file that is not a WAV") {
    const std::string path = tempPath("sl_wavio_notawav.bin");
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        const char junk[64] = "this is not a wave file, not even close";
        std::fwrite(junk, 1, sizeof(junk), f);
        std::fclose(f);
    }
    const sl::WavData d = sl::readWavMono(path);
    REQUIRE_FALSE(d.ok);
    REQUIRE_FALSE(d.error.empty());
    std::remove(path.c_str());
}

TEST_CASE("resampleLinear changes length by the rate ratio") {
    const auto x = sine(440.0, 1.0, 48000.0);
    const auto down = sl::resampleLinear(x, 48000.0, 24000.0);
    REQUIRE(down.size() == 24000);

    // A 440 Hz sine stays a 440 Hz sine: check a zero crossing count rather
    // than samples, since the interpolation shifts individual values.
    int crossings = 0;
    for (size_t i = 1; i < down.size(); ++i)
        if ((down[i - 1] < 0.f) != (down[i] < 0.f)) ++crossings;
    REQUIRE(crossings >= 870);
    REQUIRE(crossings <= 890);
}

TEST_CASE("detectRootNote finds the pitch of a plain tone") {
    // Middle C is note 0; A440 is note 9.
    REQUIRE(sl::detectRootNote(sine(261.6256, 1.0, 44100.0), 44100.0) == 0);
    REQUIRE(sl::detectRootNote(sine(440.0, 1.0, 44100.0), 44100.0) == 9);
    REQUIRE(sl::detectRootNote(sine(130.8128, 1.0, 44100.0), 44100.0) == -12);
}

TEST_CASE("a sound matches itself better than it matches anything else") {
    const auto a = sl::generateInstrument(3703184240u);   // pad
    const auto b = sl::generateInstrument(2360196101u);   // bass

    const sl::SoundFeatures fa = sl::featuresOfInstrument(a, 0);
    const sl::SoundFeatures fb = sl::featuresOfInstrument(b, 0);
    REQUIRE(fa.ok);
    REQUIRE(fb.ok);

    const double self = sl::sampleSimilarity(fa, fa);
    const double cross = sl::sampleSimilarity(fa, fb);
    INFO("self " << self << ", cross " << cross);
    REQUIRE(self > 99.9);
    REQUIRE(cross < self);
}

TEST_CASE("similarity ignores level, which a recording never matches exactly") {
    const auto inst = sl::generateInstrument(3703184240u);
    const sl::RenderResult r = sl::renderOffline(inst, 0, 1.0, sl::kMatchSeconds,
                                                 sl::kMatchRate);
    std::vector<float> loud(r.left.size()), quiet(r.left.size());
    for (size_t i = 0; i < r.left.size(); ++i) {
        const float m = 0.5f * (r.left[i] + r.right[i]);
        loud[i] = m;
        quiet[i] = m * 0.02f;      // 34 dB down
    }

    const double score = sl::sampleSimilarity(
        sl::featuresOf(loud, sl::kMatchRate, false),
        sl::featuresOf(quiet, sl::kMatchRate, false));
    INFO("score across a 34 dB level difference: " << score);
    REQUIRE(score > 99.0);
}

TEST_CASE("the sample search finds a seed closer than a random guess") {
    // Target: the render of a known seed. The search does not get told the
    // seed, only the audio, so finding something at least as close as an
    // arbitrary starting point is the bar.
    const uint32_t targetSeed = 3703184240u;
    const auto target = sl::generateInstrument(targetSeed);
    const sl::RenderResult r =
        sl::renderOffline(target, 0, 1.0, sl::kMatchSeconds, sl::kMatchRate);
    std::vector<float> mono(r.left.size());
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (r.left[i] + r.right[i]);

    const sl::SoundFeatures features = sl::featuresOf(mono, sl::kMatchRate);
    REQUIRE(features.ok);

    int tested = 0;
    const auto result = sl::SampleSearch::run(
        features, /*typeFilter*/ 1, /*rngSeed*/ 4242u, /*threshold*/ 0.0,
        [&tested] { return ++tested > 120; });

    REQUIRE(result.found);
    REQUIRE(result.tested > 0);

    const double baseline = sl::sampleSimilarity(
        features, sl::featuresOfInstrument(sl::generateInstrument(1000000000u), 0));
    INFO("best " << result.score << " after " << result.tested
                 << " candidates, baseline " << baseline);
    REQUIRE(result.score >= baseline);
}

TEST_CASE("the sample search runner cancels promptly and keeps its best") {
    const auto inst = sl::generateInstrument(2471452471u);
    const sl::RenderResult r =
        sl::renderOffline(inst, 0, 1.0, sl::kMatchSeconds, sl::kMatchRate);
    std::vector<float> mono(r.left.size());
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (r.left[i] + r.right[i]);

    sl::SampleSearchRunner runner;
    runner.setTarget(mono, sl::kMatchRate);
    REQUIRE(runner.hasTarget());

    runner.start(/*typeFilter*/ 0, /*threshold*/ 0.0);
    // Long enough for the workers to render a handful of candidates each.
    for (int i = 0; i < 200 && runner.best().tested < 8; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const auto mid = runner.best();
    runner.cancel();

    REQUIRE(mid.found);
    REQUIRE(mid.score > 0.0);
    // Cancelling keeps the best rather than discarding it.
    REQUIRE(runner.best().score >= mid.score);
}
