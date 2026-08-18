#include <catch2/catch_test_macros.hpp>
#include "sl/Instrument.h"
#include "PresetIO.h"
#include "sl/InstrumentGen.h"
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

TEST_CASE("a truncated FM matrix loads what it has instead of reading past it") {
    // Reachable from the designer's Paste JSON button, so the input is whatever
    // is on the clipboard. Indexing a CONST nlohmann::json array out of range
    // is undefined behaviour rather than an exception, so this used to read
    // past the end of a short matrix instead of stopping at it.
    const auto full = sl::instrumentToJson(sl::generateInstrument(3159763251u));

    nlohmann::json j = full;
    j["oscs"] = nlohmann::json::array();
    for (int i = 0; i < 3; ++i) j["oscs"].push_back(full["oscs"][0]);
    j["fmMatrix"] = nlohmann::json::array({nlohmann::json::array({0.5})});

    const sl::Instrument inst = sl::instrumentFromJson(j);
    REQUIRE(inst.oscCount == 3);
    REQUIRE(inst.hasFmMatrix);
    REQUIRE(inst.fmMatrix[0][0] == 0.5);
    // Everything the file did not supply stays at zero.
    REQUIRE(inst.fmMatrix[0][1] == 0.0);
    REQUIRE(inst.fmMatrix[1][0] == 0.0);
    REQUIRE(inst.fmMatrix[2][2] == 0.0);
}

TEST_CASE("a malformed FM matrix does not take the whole patch down") {
    const auto full = sl::instrumentToJson(sl::generateInstrument(3159763251u));

    nlohmann::json j = full;
    j["oscs"] = nlohmann::json::array();
    for (int i = 0; i < 2; ++i) j["oscs"].push_back(full["oscs"][0]);
    // Rows that are not arrays, and cells that are not numbers.
    j["fmMatrix"] = nlohmann::json::array({
        "not a row",
        nlohmann::json::array({nlohmann::json(), "also not a number"}),
    });

    const sl::Instrument inst = sl::instrumentFromJson(j);
    REQUIRE(inst.oscCount == 2);
    for (int s = 0; s < 2; ++s)
        for (int t = 0; t < 2; ++t)
            REQUIRE(inst.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)] == 0.0);
}

TEST_CASE("a short or mistyped envelope stage is rejected, not read past") {
    // Same undefined-behaviour class as the FM matrix: j.at("A")[1] on a const
    // json checks neither the length nor the type. A rejection here is right --
    // an envelope missing half of a stage is corrupt, not merely terse -- but
    // it has to be a rejection rather than a read past the end.
    const auto full = sl::instrumentToJson(sl::generateInstrument(13u));

    auto withGainEnv = [&full](nlohmann::json stageA) {
        nlohmann::json j = full;
        j["oscs"][0]["adsrGain"]["A"] = std::move(stageA);
        return j;
    };

    REQUIRE_THROWS(sl::instrumentFromJson(withGainEnv(nlohmann::json::array())));
    REQUIRE_THROWS(sl::instrumentFromJson(withGainEnv(nlohmann::json::array({0.1}))));
    REQUIRE_THROWS(sl::instrumentFromJson(withGainEnv(nlohmann::json(0.1))));
    REQUIRE_THROWS(sl::instrumentFromJson(
        withGainEnv(nlohmann::json::array({0.1, "not a number"}))));

    // And the well-formed one still loads.
    REQUIRE_NOTHROW(sl::instrumentFromJson(
        withGainEnv(nlohmann::json::array({0.1, 0.9}))));
}
