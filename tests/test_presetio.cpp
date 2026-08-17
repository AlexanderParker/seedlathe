#include <catch2/catch_test_macros.hpp>
#include "sl/Instrument.h"
#include "PresetIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

namespace {
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

TEST_CASE("instrumentFromJson reads a golden vector entry") {
    const auto& v = goldenVectors();
    REQUIRE(v["count"].get<int>() >= 2000);

    // Seed 13 is a "key" with 2 oscillators and no FM matrix.
    const nlohmann::json* entry = nullptr;
    for (const auto& e : v["seeds"])
        if (e["seed"].get<int64_t>() == 13) entry = &e;
    REQUIRE(entry != nullptr);

    const sl::Instrument inst = sl::instrumentFromJson((*entry)["instrument"]);
    REQUIRE(inst.oscCount == 2);
    REQUIRE(inst.typeIndex == 3);
    REQUIRE(inst.hasFmMatrix == false);
    REQUIRE(inst.oscs[0].waveform == sl::Waveform::Square);
    REQUIRE(inst.oscs[0].filterType == sl::FilterType::Highshelf);
    REQUIRE(inst.oscs[0].adsrGain.aT == 0.0009120745095424354);
    REQUIRE(inst.oscs[0].adsrGain.aV == 1.0);
    REQUIRE(inst.oscs[0].filterQ == 19.394281120039523);
}

TEST_CASE("every golden vector entry parses") {
    for (const auto& e : goldenVectors()["seeds"]) {
        const sl::Instrument inst = sl::instrumentFromJson(e["instrument"]);
        INFO("seed " << e["seed"].get<int64_t>());
        REQUIRE(inst.oscCount >= 1);
        REQUIRE(inst.oscCount <= sl::kMaxOscs);
    }
}

TEST_CASE("instrumentToJson round-trips every golden vector") {
    // Guards the export path used by presets and by "copy JSON". A field
    // dropped on write would otherwise only surface as a silently different
    // instrument after a save/load cycle.
    for (const auto& e : goldenVectors()["seeds"]) {
        const sl::Instrument a = sl::instrumentFromJson(e["instrument"]);
        const sl::Instrument b = sl::instrumentFromJson(sl::instrumentToJson(a));
        INFO("seed " << e["seed"].get<int64_t>());
        REQUIRE(b.typeIndex == a.typeIndex);
        REQUIRE(b.oscCount == a.oscCount);
        REQUIRE(b.hasFmMatrix == a.hasFmMatrix);
        for (int i = 0; i < a.oscCount; ++i) {
            const auto& x = a.oscs[static_cast<size_t>(i)];
            const auto& y = b.oscs[static_cast<size_t>(i)];
            REQUIRE(y.waveform == x.waveform);
            REQUIRE(y.filterType == x.filterType);
            REQUIRE(y.filterQ == x.filterQ);
            REQUIRE(y.oct == x.oct);
            REQUIRE(y.detune == x.detune);
            REQUIRE(y.adsrGain.aT == x.adsrGain.aT);
            REQUIRE(y.adsrGain.rV == x.adsrGain.rV);
            REQUIRE(y.adsrFilterQ.sV == x.adsrFilterQ.sV);
            REQUIRE(y.gLfo.on == x.gLfo.on);
            REQUIRE(y.fLfo.on == x.fLfo.on);
            REQUIRE(y.pLfo.on == x.pLfo.on);
            REQUIRE(y.fm.on == x.fm.on);
            REQUIRE(y.fm.depth == x.fm.depth);
            REQUIRE(y.pEnv.on == x.pEnv.on);
            REQUIRE(y.pEnv.amount == x.pEnv.amount);
            REQUIRE(y.dist.on == x.dist.on);
            REQUIRE(y.dist.amount == x.dist.amount);
            REQUIRE(y.dist.oversample == x.dist.oversample);
            REQUIRE(y.del.on == x.del.on);
            REQUIRE(y.del.time == x.del.time);
            REQUIRE(y.verb.on == x.verb.on);
            REQUIRE(y.verb.decay == x.verb.decay);
        }
        if (a.hasFmMatrix)
            for (int s = 0; s < a.oscCount; ++s)
                for (int t = 0; t < a.oscCount; ++t)
                    REQUIRE(b.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)] ==
                            a.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)]);
    }
}
