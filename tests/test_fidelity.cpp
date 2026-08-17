#include <catch2/catch_test_macros.hpp>
#include "Analysis.h"
#include "OfflineRender.h"
#include "sl/InstrumentGen.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Wav {
    std::vector<float> mono;
    double sampleRate = 48000.0;
};

// Reads the mono 32-bit float WAV written by tools/export-reference-audio.mjs.
// Fixed 44-byte header, format tag 3, so no chunk walking is needed.
Wav readWavMono32(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char hdr[44];
    f.read(hdr, 44);
    uint32_t sr = 0, dataBytes = 0;
    uint16_t channels = 0;
    std::memcpy(&channels, hdr + 22, 2);
    std::memcpy(&sr, hdr + 24, 4);
    std::memcpy(&dataBytes, hdr + 40, 4);
    if (channels != 1) throw std::runtime_error("expected mono reference: " + path);

    Wav w;
    w.sampleRate = double(sr);
    w.mono.resize(dataBytes / sizeof(float));
    f.read(reinterpret_cast<char*>(w.mono.data()), dataBytes);
    return w;
}

struct Score {
    uint32_t seed = 0;
    double mel = 0.0;
    double rms = 0.0;
};

} // namespace

// The acceptance gate for the whole engine: does a seed rendered in C++ sound
// like the same seed rendered by Chrome. Everything else in P1 is in service of
// this number.
TEST_CASE("C++ renders match Chrome within the locked thresholds", "[fidelity]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(SL_VECTORS_DIR) / "audio";
    REQUIRE(fs::exists(dir));

    nlohmann::json manifest;
    {
        std::ifstream in((dir / "MANIFEST.json").string());
        REQUIRE(in.good());
        in >> manifest;
    }
    const double seconds = manifest.at("seconds").get<double>();
    const double gain = manifest.at("gain").get<double>();
    const int note = manifest.at("note").get<int>();

    nlohmann::json th;
    {
        std::ifstream in(std::string(SL_VECTORS_DIR) + "/fidelity-thresholds.json");
        REQUIRE(in.good());
        in >> th;
    }
    const double melLimit = th.at("melMaxDb").get<double>();
    const double rmsLimit = th.at("rmsMaxDb").get<double>();
    const double meanMelLimit = th.at("meanMelMaxDb").get<double>();

    std::vector<Score> scores;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".wav") continue;
        const auto seed = static_cast<uint32_t>(std::stoull(e.path().stem().string()));

        const Wav ref = readWavMono32(e.path().string());
        const auto got = sl::renderOffline(sl::generateInstrument(seed), note, gain,
                                           seconds, ref.sampleRate);

        const double mel = sl::melDistanceDb(sl::melSpectrogram(got.left, ref.sampleRate),
                                             sl::melSpectrogram(ref.mono, ref.sampleRate));
        const double rms = sl::rmsEnvelopeDistanceDb(got.left, ref.mono, ref.sampleRate);
        scores.push_back({seed, mel, rms});
    }
    REQUIRE(scores.size() >= 50);

    // Report the worst offenders before asserting, so a failure names the seeds
    // to investigate instead of just the first one alphabetically.
    std::sort(scores.begin(), scores.end(),
              [](const Score& a, const Score& b) { return a.mel > b.mel; });
    for (size_t i = 0; i < std::min<size_t>(10, scores.size()); ++i)
        WARN("worst mel: seed " << scores[i].seed << " = " << scores[i].mel
             << " dB (rms " << scores[i].rms << " dB)");

    double melSum = 0.0;
    for (const auto& s : scores) melSum += s.mel;
    const double meanMel = melSum / double(scores.size());
    WARN("mean mel distance " << meanMel << " dB over " << scores.size() << " seeds");

    // Per-seed ceiling: catches one instrument breaking badly.
    for (const auto& s : scores) {
        INFO("seed " << s.seed << " mel " << s.mel << " dB, rms " << s.rms << " dB");
        REQUIRE(s.mel <= melLimit);
        REQUIRE(s.rms <= rmsLimit);
    }

    // Mean: the sensitive gate. A systematic regression -- a filter constant, a
    // gain stage, an envelope shape -- moves this long before it pushes any
    // single seed past its ceiling.
    INFO("mean mel " << meanMel << " dB across " << scores.size() << " seeds");
    REQUIRE(meanMel <= meanMelLimit);
}

// The sustained path was invisible to the test above: every reference render
// was a one-shot, so the sustain plateau and the release ramp off it had no
// coverage at all. A per-oscillator release-time bug lived there unnoticed --
// the engine ramped every oscillator down at the slowest of their release
// times, while zyn ramps each at its own.
TEST_CASE("held notes match Chrome through sustain and release", "[fidelity]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(SL_VECTORS_DIR) / "audio-sustained";
    REQUIRE(fs::exists(dir));

    nlohmann::json manifest;
    {
        std::ifstream in((dir / "MANIFEST.json").string());
        REQUIRE(in.good());
        in >> manifest;
    }
    REQUIRE(manifest.at("sustained").get<bool>());
    const double seconds = manifest.at("seconds").get<double>();
    const double hold = manifest.at("holdSeconds").get<double>();
    const double gain = manifest.at("gain").get<double>();
    const int note = manifest.at("note").get<int>();

    nlohmann::json th;
    {
        std::ifstream in(std::string(SL_VECTORS_DIR) + "/fidelity-thresholds.json");
        REQUIRE(in.good());
        in >> th;
    }
    const double melLimit = th.at("sustainedMelMaxDb").get<double>();
    const double rmsLimit = th.at("sustainedRmsMaxDb").get<double>();
    const double meanMelLimit = th.at("sustainedMeanMelMaxDb").get<double>();

    std::vector<Score> scores;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() != ".wav") continue;
        const auto seed = static_cast<uint32_t>(std::stoull(e.path().stem().string()));

        const Wav ref = readWavMono32(e.path().string());
        const auto got = sl::renderOfflineSustained(sl::generateInstrument(seed), note,
                                                    gain, hold, seconds, ref.sampleRate);

        const double mel = sl::melDistanceDb(sl::melSpectrogram(got.left, ref.sampleRate),
                                             sl::melSpectrogram(ref.mono, ref.sampleRate));
        const double rms = sl::rmsEnvelopeDistanceDb(got.left, ref.mono, ref.sampleRate);
        scores.push_back({seed, mel, rms});
    }
    REQUIRE(scores.size() >= 50);

    std::sort(scores.begin(), scores.end(),
              [](const Score& a, const Score& b) { return a.mel > b.mel; });
    for (size_t i = 0; i < std::min<size_t>(10, scores.size()); ++i)
        WARN("worst sustained mel: seed " << scores[i].seed << " = " << scores[i].mel
             << " dB (rms " << scores[i].rms << " dB)");

    double melSum = 0.0;
    for (const auto& s : scores) melSum += s.mel;
    const double meanMel = melSum / double(scores.size());
    WARN("mean sustained mel distance " << meanMel << " dB over " << scores.size()
         << " seeds");

    for (const auto& s : scores) {
        INFO("seed " << s.seed << " mel " << s.mel << " dB, rms " << s.rms << " dB");
        REQUIRE(s.mel <= melLimit);
        REQUIRE(s.rms <= rmsLimit);
    }
    INFO("mean sustained mel " << meanMel << " dB across " << scores.size() << " seeds");
    REQUIRE(meanMel <= meanMelLimit);
}

// Diagnostic, not a gate: hidden by the leading dot, run by name. Prints where
// in the note a sustained render diverges from Chrome, which is the difference
// between "the sustain plateau is wrong" and "the release ramp is wrong".
//
//   sl_tests.exe "sustained divergence over time"
TEST_CASE("sustained divergence over time", "[.diag]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(SL_VECTORS_DIR) / "audio-sustained";
    nlohmann::json manifest;
    {
        std::ifstream in((dir / "MANIFEST.json").string());
        in >> manifest;
    }
    const double seconds = manifest.at("seconds").get<double>();
    const double hold = manifest.at("holdSeconds").get<double>();
    const double gain = manifest.at("gain").get<double>();
    const int note = manifest.at("note").get<int>();

    for (uint32_t seed : {1596900054u, 439588263u, 1946705502u}) {
        const fs::path wav = dir / (std::to_string(seed) + ".wav");
        if (!fs::exists(wav)) continue;
        const Wav ref = readWavMono32(wav.string());
        const auto got = sl::renderOfflineSustained(sl::generateInstrument(seed), note,
                                                    gain, hold, seconds, ref.sampleRate);

        const size_t win = static_cast<size_t>(ref.sampleRate * 0.25);
        const sl::Instrument inst = sl::generateInstrument(seed);
        WARN("seed " << seed << " (release at " << hold << " s), "
             << inst.oscCount << " osc");
        for (int o = 0; o < inst.oscCount; ++o) {
            const sl::Adsr& g = inst.oscs[static_cast<size_t>(o)].adsrGain;
            WARN("  osc " << o << " gain env  A " << g.aT << "/" << g.aV
                 << "  D " << g.dT << "/" << g.dV
                 << "  S " << g.sT << "/" << g.sV
                 << "  R " << g.rT << "/" << g.rV);
        }
        for (size_t start = 0; start + win <= ref.mono.size(); start += win) {
            double se = 0.0, sr = 0.0, sg = 0.0;
            for (size_t i = start; i < start + win; ++i) {
                const double d = double(got.left[i]) - double(ref.mono[i]);
                se += d * d;
                sr += double(ref.mono[i]) * double(ref.mono[i]);
                sg += double(got.left[i]) * double(got.left[i]);
            }
            WARN("  t=" << (double(start) / ref.sampleRate) << "s  ref "
                 << std::sqrt(sr / double(win)) << "  ours "
                 << std::sqrt(sg / double(win)) << "  err "
                 << std::sqrt(se / double(win)));
        }
    }
}
