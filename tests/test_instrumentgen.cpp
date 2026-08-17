#include <catch2/catch_test_macros.hpp>
#include "sl/InstrumentGen.h"
#include "PresetIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

namespace {

void requireAdsrEqual(const sl::Adsr& a, const sl::Adsr& b, const char* what) {
    INFO(what);
    REQUIRE(a.aT == b.aT); REQUIRE(a.aV == b.aV);
    REQUIRE(a.dT == b.dT); REQUIRE(a.dV == b.dV);
    REQUIRE(a.sT == b.sT); REQUIRE(a.sV == b.sV);
    REQUIRE(a.rT == b.rT); REQUIRE(a.rV == b.rV);
}

const nlohmann::json& goldenVectors() {
    static nlohmann::json v = [] {
        std::ifstream in(std::string(SL_VECTORS_DIR) + "/instruments.json");
        if (!in.good()) throw std::runtime_error("vectors/instruments.json missing");
        nlohmann::json j;
        in >> j;
        return j;
    }();
    return v;
}

} // namespace

TEST_CASE("generateInstrument reproduces every golden vector exactly") {
    int checked = 0;
    for (const auto& e : goldenVectors()["seeds"]) {
        const auto seed = static_cast<uint32_t>(e["seed"].get<int64_t>());
        const sl::Instrument want = sl::instrumentFromJson(e["instrument"]);
        const sl::Instrument got = sl::generateInstrument(seed);

        INFO("seed " << seed);
        REQUIRE(got.typeIndex == want.typeIndex);
        REQUIRE(got.oscCount == want.oscCount);
        REQUIRE(got.hasFmMatrix == want.hasFmMatrix);

        for (int i = 0; i < want.oscCount; ++i) {
            INFO("osc " << i);
            const auto& a = got.oscs[static_cast<size_t>(i)];
            const auto& b = want.oscs[static_cast<size_t>(i)];
            REQUIRE(a.waveform == b.waveform);
            requireAdsrEqual(a.adsrGain, b.adsrGain, "adsrGain");
            REQUIRE(a.filterType == b.filterType);
            requireAdsrEqual(a.adsrFilter, b.adsrFilter, "adsrFilter");
            REQUIRE(a.filterQ == b.filterQ);
            requireAdsrEqual(a.adsrFilterQ, b.adsrFilterQ, "adsrFilterQ");

            REQUIRE(a.gLfo.on == b.gLfo.on);
            if (a.gLfo.on) {
                REQUIRE(a.gLfo.type == b.gLfo.type);
                REQUIRE(a.gLfo.frequency == b.gLfo.frequency);
                REQUIRE(a.gLfo.depth == b.gLfo.depth);
            }
            REQUIRE(a.fLfo.on == b.fLfo.on);
            if (a.fLfo.on) {
                REQUIRE(a.fLfo.type == b.fLfo.type);
                REQUIRE(a.fLfo.frequency == b.fLfo.frequency);
                REQUIRE(a.fLfo.depth == b.fLfo.depth);
            }
            REQUIRE(a.pLfo.on == b.pLfo.on);
            if (a.pLfo.on) {
                REQUIRE(a.pLfo.type == b.pLfo.type);
                REQUIRE(a.pLfo.frequency == b.pLfo.frequency);
                REQUIRE(a.pLfo.depth == b.pLfo.depth);
            }
            REQUIRE(a.fm.on == b.fm.on);
            if (a.fm.on) {
                REQUIRE(a.fm.type == b.fm.type);
                REQUIRE(a.fm.frequency == b.fm.frequency);
                REQUIRE(a.fm.depth == b.fm.depth);
            }
            REQUIRE(a.pEnv.on == b.pEnv.on);
            if (a.pEnv.on) {
                REQUIRE(a.pEnv.amount == b.pEnv.amount);
                requireAdsrEqual(a.pEnv.env, b.pEnv.env, "pENV");
            }
            REQUIRE(a.dist.on == b.dist.on);
            if (a.dist.on) {
                REQUIRE(a.dist.amount == b.dist.amount);
                REQUIRE(a.dist.oversample == b.dist.oversample);
            }
            REQUIRE(a.oct == b.oct);
            REQUIRE(a.detune == b.detune);
            REQUIRE(a.del.on == b.del.on);
            if (a.del.on) {
                REQUIRE(a.del.time == b.del.time);
                REQUIRE(a.del.feedback == b.del.feedback);
            }
            REQUIRE(a.verb.on == b.verb.on);
            if (a.verb.on) {
                REQUIRE(a.verb.duration == b.verb.duration);
                REQUIRE(a.verb.decay == b.verb.decay);
            }
        }

        if (want.hasFmMatrix)
            for (int s = 0; s < want.oscCount; ++s)
                for (int t = 0; t < want.oscCount; ++t)
                    REQUIRE(got.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)] ==
                            want.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)]);
        ++checked;
    }
    REQUIRE(checked >= 2000);
}

TEST_CASE("oscillator count distribution proves the loop condition redraws") {
    // zyn re-evaluates `i < Math.floor(r(o) + 1)` every iteration, consuming a
    // random number each pass. A single up-front draw would give a roughly
    // uniform 1..4 spread for pads; the real generator does not.
    int counts[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t k = 0; k < 3000; ++k)
        counts[sl::generateInstrument(k * 10).oscCount]++;
    REQUIRE(counts[1] == 761);
    REQUIRE(counts[2] == 1118);
    REQUIRE(counts[3] == 837);
    REQUIRE(counts[4] == 284);
}
