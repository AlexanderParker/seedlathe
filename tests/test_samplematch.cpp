#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "OfflineRender.h"
#include "SampleMatch.h"
#include "WavIO.h"
#include "sl/FactoryPresets.h"
#include "sl/InstrumentGen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>
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

// The gate the metric actually has to pass. Reflexivity -- a sound matching
// itself -- proves nothing; what matters is whether the target's own seed comes
// FIRST out of the whole factory bank, and by enough to mean something.
//
// Two targets rather than the diagnostic's five, because each one renders the
// entire bank and the suite should not take a minute to say the same thing.
TEST_CASE("a target's own seed ranks first in the factory bank") {
    for (uint32_t seed : {3703184240u /*pad*/, 2360196101u /*bass*/}) {
        const sl::SoundFeatures target =
            sl::featuresOfInstrument(sl::generateInstrument(seed), 0);
        REQUIRE(target.ok);

        std::vector<std::pair<double, uint32_t>> scored;
        for (int i = 0; i < sl::kNumFactoryPresets; ++i) {
            const uint32_t candidate = sl::kFactoryPresets[i].seed;
            scored.push_back({sl::sampleSimilarity(
                target, sl::featuresOfInstrument(sl::generateInstrument(candidate),
                                                 target.rootNote)), candidate});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        double sum = 0.0;
        for (const auto& e : scored) sum += e.first;
        const double mean = sum / double(scored.size());

        INFO("seed " << seed << ": top " << scored.front().second
             << " at " << scored.front().first << ", runner-up "
             << scored[1].first << ", mean " << mean);

        REQUIRE(scored.front().second == seed);
        // And by a margin. A metric that put everything within a point of
        // everything else would pass the rank check while being useless.
        REQUIRE(scored.front().first - scored[1].first > 5.0);
        REQUIRE(scored.front().first - mean > 30.0);
    }
}

// Noise robustness, locked in. This is the property the whole minimum-statistics
// noise estimate exists for, and it is easy to lose by accident: averaging the
// mel bands in decibels rather than in power undoes most of it, and so does
// dropping the per-band subtraction.
//
// Before the estimate went in, the true match at 20 dB SNR -- an ordinary
// recording, not a bad one -- sat at rank 83 of 115, and scored HIGHER than a
// clean target because a noisy sample resembles everything a little.
TEST_CASE("a noisy target still finds its own seed near the top") {
    const uint32_t seed = 2471452471u;
    const sl::RenderResult r = sl::renderOffline(sl::generateInstrument(seed), 0, 1.0,
                                                 sl::kMatchSeconds, sl::kMatchRate);
    std::vector<float> mono(r.left.size());
    double peak = 0.0;
    for (size_t i = 0; i < mono.size(); ++i) {
        mono[i] = 0.5f * (r.left[i] + r.right[i]);
        peak = std::max(peak, double(std::fabs(mono[i])));
    }

    // Deterministic noise at 20 dB below the peak.
    uint32_t rng = 0xC0FFEEu;
    const double amp = peak * 0.1;
    for (float& v : mono) {
        rng = rng * 1664525u + 1013904223u;
        v += static_cast<float>(((double(rng) / 4294967296.0) * 2.0 - 1.0) * amp);
    }

    const sl::SoundFeatures target = sl::featuresOf(mono, sl::kMatchRate);
    REQUIRE(target.ok);

    std::vector<std::pair<double, uint32_t>> scored;
    for (int i = 0; i < sl::kNumFactoryPresets; ++i) {
        const uint32_t candidate = sl::kFactoryPresets[i].seed;
        scored.push_back({sl::sampleSimilarity(
            target, sl::featuresOfInstrument(sl::generateInstrument(candidate),
                                             target.rootNote)), candidate});
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    int rank = 0;
    for (size_t i = 0; i < scored.size(); ++i)
        if (scored[i].second == seed) { rank = static_cast<int>(i) + 1; break; }

    INFO("rank " << rank << "/" << scored.size() << ", top " << scored.front().first);
    REQUIRE(rank > 0);
    REQUIRE(rank <= 15);

    // And it must not be CONFIDENT about it. A noisy target scoring as high as
    // a clean one would mean the score cannot be used as a stopping condition.
    REQUIRE(scored.front().first < 85.0);
}

// Diagnostic, not a gate. Run by name:
//   sl_tests.exe "sample match ranking"
//
// The similarity metric has only ever been checked against renders of itself,
// which proves it is reflexive and nothing more. The question that matters is
// whether it RANKS: given a target, does its own instrument come first out of
// the whole factory bank, and by how much does it beat the rest? A metric that
// scores everything 85-95% would make the search look busy while returning
// noise, and would make the "search until 85%" threshold meaningless.
TEST_CASE("sample match ranking", "[.diag]") {
    // Spread across instrument types rather than the first N, so a metric that
    // only works on sustained tones is caught.
    const uint32_t targets[] = {
        3703184240u,   // pad
        2471452471u,   // lead
        2360196101u,   // bass
        1455000701u,   // lead
        185045751u,    // lead
    };

    // Score every factory preset once per target.
    std::vector<sl::SoundFeatures> bank;
    bank.reserve(static_cast<size_t>(sl::kNumFactoryPresets));

    for (uint32_t seed : targets) {
        const sl::SoundFeatures target =
            sl::featuresOfInstrument(sl::generateInstrument(seed), 0);
        REQUIRE(target.ok);

        std::vector<std::pair<double, uint32_t>> scored;
        for (int i = 0; i < sl::kNumFactoryPresets; ++i) {
            const uint32_t candidate = sl::kFactoryPresets[i].seed;
            const double score = sl::sampleSimilarity(
                target, sl::featuresOfInstrument(sl::generateInstrument(candidate), 0));
            scored.push_back({score, candidate});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        int rank = 0;
        for (size_t i = 0; i < scored.size(); ++i)
            if (scored[i].second == seed) { rank = static_cast<int>(i) + 1; break; }

        double sum = 0.0;
        for (const auto& s : scored) sum += s.first;

        WARN("target " << seed << ": own rank " << rank << "/" << scored.size()
             << ", self " << scored.front().first
             << ", runner-up " << scored[1].first
             << ", median " << scored[scored.size() / 2].first
             << ", worst " << scored.back().first
             << ", mean " << (sum / double(scored.size())));
    }
}

// Diagnostic. Run by name:
//   sl_tests.exe "sample match under degradation"
//
// Ranking a clean render against the bank proves the metric discriminates
// between seeds. It says nothing about a real recording, which arrives with
// noise, a room on it, a dull microphone and a pitch nobody agreed on. This
// degrades a known target four ways and asks whether its own seed still comes
// first -- if a little noise sinks it, the feature is decorative.
TEST_CASE("sample match under degradation", "[.diag]") {
    const uint32_t seed = 2471452471u;   // lead
    const sl::RenderResult r = sl::renderOffline(sl::generateInstrument(seed), 0, 1.0,
                                                 sl::kMatchSeconds, sl::kMatchRate);
    std::vector<float> clean(r.left.size());
    for (size_t i = 0; i < clean.size(); ++i) clean[i] = 0.5f * (r.left[i] + r.right[i]);

    double peak = 0.0;
    for (float v : clean) peak = std::max(peak, double(std::fabs(v)));

    // Deterministic noise, so the numbers are comparable run to run.
    uint32_t rng = 0xC0FFEEu;
    auto noise = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return (double(rng) / 4294967296.0) * 2.0 - 1.0;
    };

    auto rankOf = [&](const std::vector<float>& signal, double rate) {
        const sl::SoundFeatures target = sl::featuresOf(signal, rate);
        std::vector<std::pair<double, uint32_t>> scored;
        for (int i = 0; i < sl::kNumFactoryPresets; ++i) {
            const uint32_t candidate = sl::kFactoryPresets[i].seed;
            scored.push_back({sl::sampleSimilarity(
                target, sl::featuresOfInstrument(sl::generateInstrument(candidate),
                                                 target.rootNote)), candidate});
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        int rank = 0;
        for (size_t i = 0; i < scored.size(); ++i)
            if (scored[i].second == seed) { rank = static_cast<int>(i) + 1; break; }
        return std::make_tuple(rank, scored.front().first, scored[1].first,
                               target.rootNote);
    };

    auto report = [&](const char* what, const std::vector<float>& s, double rate) {
        const auto [rank, top, second, root] = rankOf(s, rate);
        WARN(what << ": own rank " << rank << "/" << sl::kNumFactoryPresets
             << ", top score " << top << ", second " << second
             << ", detected root " << root);
    };

    report("clean", clean, sl::kMatchRate);

    for (double snrDb : {20.0, 10.0}) {
        const double amp = peak * std::pow(10.0, -snrDb / 20.0);
        std::vector<float> noisy(clean.size());
        for (size_t i = 0; i < clean.size(); ++i)
            noisy[i] = clean[i] + static_cast<float>(noise() * amp);
        char label[48];
        std::snprintf(label, sizeof(label), "white noise at %.0f dB SNR", snrDb);
        report(label, noisy, sl::kMatchRate);
    }

    {
        // One-pole at 4 kHz: a dull microphone, or a source across a room.
        std::vector<float> dull(clean.size());
        const double a = std::exp(-2.0 * 3.14159265358979323846 * 4000.0 / sl::kMatchRate);
        double z = 0.0;
        for (size_t i = 0; i < clean.size(); ++i) {
            z = double(clean[i]) * (1.0 - a) + z * a;
            dull[i] = static_cast<float>(z);
        }
        report("lowpassed at 4 kHz", dull, sl::kMatchRate);
    }

    {
        // A semitone sharp, by resampling. Tests whether the pitch detection
        // is load-bearing: without it the mel profile shifts and every band
        // lands somewhere else.
        const auto sharp = sl::resampleLinear(clean, sl::kMatchRate,
                                              sl::kMatchRate / std::pow(2.0, 1.0 / 12.0));
        report("one semitone sharp", sharp, sl::kMatchRate);
    }
}

namespace {

// A minimal well-formed 16-bit PCM WAV, built in memory so the tests can
// corrupt it in specific ways.
std::vector<unsigned char> makeWav(uint32_t rate, uint16_t channels,
                                   uint16_t bits, uint16_t format,
                                   size_t frames) {
    const uint32_t bytesPerSample = bits / 8u;
    const uint32_t dataBytes = static_cast<uint32_t>(frames) * bytesPerSample * channels;
    std::vector<unsigned char> b(44 + dataBytes, 0);
    auto put32 = [&b](size_t at, uint32_t v) {
        b[at] = v & 0xFF; b[at + 1] = (v >> 8) & 0xFF;
        b[at + 2] = (v >> 16) & 0xFF; b[at + 3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&b](size_t at, uint16_t v) {
        b[at] = v & 0xFF; b[at + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(&b[0], "RIFF", 4);  put32(4, 36 + dataBytes);
    std::memcpy(&b[8], "WAVE", 4);
    std::memcpy(&b[12], "fmt ", 4); put32(16, 16);
    put16(20, format); put16(22, channels); put32(24, rate);
    put32(28, rate * bytesPerSample * channels);
    put16(32, static_cast<uint16_t>(bytesPerSample * channels)); put16(34, bits);
    std::memcpy(&b[36], "data", 4); put32(40, dataBytes);
    for (size_t i = 44; i < b.size(); ++i) b[i] = static_cast<unsigned char>(i * 37u);
    return b;
}

std::string writeBytes(const char* name, const std::vector<unsigned char>& b) {
    const std::string path = tempPath(name);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
    return path;
}

} // namespace

TEST_CASE("a WAV claiming an absurd sample rate is refused") {
    // The rate divides into the resampler's output length, so a file claiming
    // 1 Hz would turn one second of audio into a 22050x allocation -- gigabytes,
    // from the Load Sample button, on a file the user did not write.
    for (uint32_t rate : {1u, 10u, 999u, 2000000u}) {
        const std::string path =
            writeBytes("sl_wav_rate.wav", makeWav(rate, 1, 16, 1, 1000));
        const sl::WavData d = sl::readWavMono(path);
        INFO("rate " << rate << ": " << d.error);
        REQUIRE_FALSE(d.ok);
        std::remove(path.c_str());
    }

    // And the plausible ones still load.
    for (uint32_t rate : {8000u, 44100u, 48000u, 192000u}) {
        const std::string path =
            writeBytes("sl_wav_rate.wav", makeWav(rate, 1, 16, 1, 1000));
        const sl::WavData d = sl::readWavMono(path);
        INFO("rate " << rate << ": " << d.error);
        REQUIRE(d.ok);
        REQUIRE(d.sampleRate == double(rate));
        std::remove(path.c_str());
    }
}

TEST_CASE("the WAV reader survives corrupted files") {
    // Deterministic fuzz. The reader walks a chunk list with sizes taken
    // straight from the file, so a wrong length is the obvious way to walk off
    // the end. Every outcome is acceptable except a crash or a lie: either it
    // refuses the file, or what it returns is internally consistent.
    const std::vector<unsigned char> good = makeWav(44100, 2, 16, 1, 500);

    uint32_t rng = 0x1234567u;
    auto next = [&rng] { rng = rng * 1664525u + 1013904223u; return rng; };

    int accepted = 0, refused = 0;
    for (int iter = 0; iter < 400; ++iter) {
        std::vector<unsigned char> b = good;

        // Corrupt a handful of bytes, favouring the header where the lengths
        // and counts live.
        const int edits = 1 + int(next() % 6);
        for (int e = 0; e < edits; ++e) {
            const size_t at = (next() % 4 == 0) ? (next() % b.size())
                                                : (next() % 44);
            b[at] = static_cast<unsigned char>(next());
        }
        // Sometimes truncate as well.
        if (next() % 3 == 0 && b.size() > 20)
            b.resize(20 + next() % (b.size() - 20));

        const std::string path = writeBytes("sl_wav_fuzz.wav", b);
        const sl::WavData d = sl::readWavMono(path);
        std::remove(path.c_str());

        if (!d.ok) {
            REQUIRE_FALSE(d.error.empty());
            ++refused;
            continue;
        }
        ++accepted;
        REQUIRE(d.channels > 0);
        REQUIRE(d.sampleRate >= sl::kMinWavRate);
        REQUIRE(d.sampleRate <= sl::kMaxWavRate);
        REQUIRE_FALSE(d.mono.empty());
        for (float v : d.mono) REQUIRE(std::isfinite(v));
    }

    // A fuzzer that rejected everything would prove nothing about the decoder.
    INFO("accepted " << accepted << ", refused " << refused);
    REQUIRE(accepted > 0);
    REQUIRE(refused > 0);
}

TEST_CASE("the sample search survives being restarted repeatedly") {
    // Every restart cancels and joins a whole worker pool, not a single thread.
    // Getting that wrong leaks threads and lets an old pool publish into the new
    // search -- and in the plugin the restart is one button click.
    const auto inst = sl::generateInstrument(2471452471u);
    const sl::RenderResult r =
        sl::renderOffline(inst, 0, 1.0, sl::kMatchSeconds, sl::kMatchRate);
    std::vector<float> mono(r.left.size());
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (r.left[i] + r.right[i]);

    sl::SampleSearchRunner runner;
    runner.setTarget(mono, sl::kMatchRate);
    REQUIRE(runner.hasTarget());

    for (int i = 0; i < 12; ++i) {
        runner.start(/*typeFilter*/ 0, /*threshold*/ 0.0);
        if (i % 2 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        runner.cancel();

        // Whatever is published mid-churn has to be coherent on its own terms.
        const auto top = runner.top();
        REQUIRE(top.size() <= sl::TopList::kCapacity);
        for (size_t k = 1; k < top.size(); ++k)
            REQUIRE(top[k - 1].score >= top[k].score);
        for (const auto& e : top) {
            REQUIRE(e.score >= 0.0);
            REQUIRE(e.score <= 100.0);
        }
    }

    for (int i = 0; i < 400 && runner.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE_FALSE(runner.running());

    // Replacing the target mid-flight has to cancel and join too, or the old
    // pool keeps scoring against audio that has been freed.
    runner.start(0, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    runner.setTarget(mono, sl::kMatchRate);
    REQUIRE_FALSE(runner.running());
}
