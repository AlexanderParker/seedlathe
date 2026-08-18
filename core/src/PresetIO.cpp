#include "PresetIO.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace sl {
namespace {

const char* const kTypeNames[10] = {"pad", "lead", "bass", "key", "pluck",
                                    "bell", "string", "drum", "perc", "fx"};
const char* const kWaveNames[5] = {"sine", "square", "sawtooth", "triangle", "noise"};
const char* const kFilterNames[7] = {"lowpass", "highpass", "bandpass", "lowshelf",
                                     "highshelf", "peaking", "allpass"};

Waveform waveformFromName(const std::string& s) {
    for (int i = 0; i < 5; ++i)
        if (s == kWaveNames[i]) return static_cast<Waveform>(i);
    throw std::runtime_error("unknown waveform: " + s);
}

FilterType filterFromName(const std::string& s) {
    for (int i = 0; i < 7; ++i)
        if (s == kFilterNames[i]) return static_cast<FilterType>(i);
    throw std::runtime_error("unknown filter: " + s);
}

int typeIndexFromName(const std::string& s) {
    for (int i = 0; i < 10; ++i)
        if (s == kTypeNames[i]) return i;
    throw std::runtime_error("unknown instrument type: " + s);
}

// One [time, value] entry. at() throws on a missing stage, which is what a
// corrupt preset deserves, but the INDICES were unchecked: on a const
// nlohmann::json, reading past the end of an array -- or indexing something
// that is not an array at all -- is undefined behaviour rather than an
// exception. A hand-written or clipboard-pasted envelope reaches this.
double envField(const nlohmann::json& j, const char* stage, size_t i) {
    const auto& p = j.at(stage);
    if (!p.is_array() || p.size() <= i || !p[i].is_number())
        throw std::runtime_error(std::string("bad envelope stage: ") + stage);
    return p[i].get<double>();
}

Adsr adsrFromJson(const nlohmann::json& j) {
    Adsr a;
    a.aT = envField(j, "A", 0); a.aV = envField(j, "A", 1);
    a.dT = envField(j, "D", 0); a.dV = envField(j, "D", 1);
    a.sT = envField(j, "S", 0); a.sV = envField(j, "S", 1);
    a.rT = envField(j, "R", 0); a.rV = envField(j, "R", 1);
    return a;
}

nlohmann::json adsrToJson(const Adsr& a) {
    return nlohmann::json{
        {"A", {a.aT, a.aV}},
        {"D", {a.dT, a.dV}},
        {"S", {a.sT, a.sV}},
        {"R", {a.rT, a.rV}},
    };
}

// zyn writes `false` for an absent LFO or FM block, an object when present.
Lfo lfoFromJson(const nlohmann::json& j) {
    Lfo l;
    if (!j.is_object()) return l;
    l.on = true;
    l.type = waveformFromName(j.value("type", std::string("sine")));
    l.frequency = j.value("frequency", 1.0);
    l.depth = j.value("depth", 0.0);
    return l;
}

nlohmann::json lfoToJson(const Lfo& l) {
    if (!l.on) return false;
    return nlohmann::json{{"type", kWaveNames[static_cast<int>(l.type)]},
                          {"frequency", l.frequency},
                          {"depth", l.depth}};
}

} // namespace

const char* typeName(int i) {
    return (i >= 0 && i < 10) ? kTypeNames[i] : "unknown";
}

Instrument instrumentFromJson(const nlohmann::json& j) {
    Instrument inst;
    inst.typeIndex = typeIndexFromName(j.at("type").get<std::string>());

    const auto& oscs = j.at("oscs");
    inst.oscCount = static_cast<int>(oscs.size());
    if (inst.oscCount < 1 || inst.oscCount > kMaxOscs)
        throw std::runtime_error("bad oscillator count: " + std::to_string(inst.oscCount));

    for (int i = 0; i < inst.oscCount; ++i) {
        const auto& o = oscs[static_cast<size_t>(i)];
        Osc& d = inst.oscs[static_cast<size_t>(i)];

        d.waveform    = waveformFromName(o.at("waveform").get<std::string>());
        d.adsrGain    = adsrFromJson(o.at("adsrGain"));
        d.filterType  = filterFromName(o.at("filterType").get<std::string>());
        d.adsrFilter  = adsrFromJson(o.at("adsrFilter"));
        d.filterQ     = o.at("filterQ").get<double>();
        d.adsrFilterQ = adsrFromJson(o.at("adsrFilterQ"));
        d.gLfo        = lfoFromJson(o.at("gLFO"));
        d.fLfo        = lfoFromJson(o.at("fLFO"));
        d.pLfo        = lfoFromJson(o.at("pLFO"));

        if (o.at("FM").is_object()) {
            d.fm.on = true;
            d.fm.type = waveformFromName(o["FM"].value("type", std::string("sine")));
            d.fm.frequency = o["FM"].value("frequency", 1.0);
            d.fm.depth = o["FM"].value("depth", 0.0);
        }
        if (o.at("pENV").is_object()) {
            d.pEnv.on = true;
            d.pEnv.amount = o["pENV"].value("amount", 0.0);
            d.pEnv.env = adsrFromJson(o["pENV"]);
        }
        // `dist` is undefined when unset, and JSON.stringify omits undefined
        // keys entirely -- so absence, not null, is the normal case.
        if (o.contains("dist") && o["dist"].is_object()) {
            d.dist.on = true;
            d.dist.amount = o["dist"].value("amount", 0.0);
            const std::string os = o["dist"].value("oversample", std::string("none"));
            d.dist.oversample = (os == "4x") ? 4 : (os == "2x") ? 2 : 1;
        }

        d.oct = o.at("oct").get<int>();
        d.detune = o.at("detune").get<double>();

        const auto& fx = o.at("fx");
        if (fx.at("del").is_object()) {
            d.del.on = true;
            d.del.time = fx["del"].at("time").get<double>();
            d.del.feedback = fx["del"].at("feedback").get<double>();
        }
        if (fx.at("verb").is_object()) {
            d.verb.on = true;
            d.verb.duration = fx["verb"].at("duration").get<double>();
            d.verb.decay = fx["verb"].at("decay").get<double>();
        }
    }

    // Indexing a CONST nlohmann::json array out of range is undefined
    // behaviour, not an exception -- so a hand-written or truncated matrix
    // would read past the end rather than being rejected. This arrives from
    // the designer's Paste JSON button, which means arbitrary clipboard
    // content, so the row and column counts are checked rather than assumed.
    // A short matrix loads what it has: that is friendlier than refusing an
    // otherwise good patch, and no less safe.
    const auto readMatrix = [&](const char* key, auto& dest) {
        if (!j.contains(key) || !j[key].is_array()) return false;
        const auto& m = j[key];
        for (int s = 0; s < inst.oscCount && s < static_cast<int>(m.size()); ++s) {
            const auto& row = m[static_cast<size_t>(s)];
            if (!row.is_array()) continue;
            for (int t = 0; t < inst.oscCount && t < static_cast<int>(row.size()); ++t) {
                const auto& cell = row[static_cast<size_t>(t)];
                if (cell.is_number())
                    dest[static_cast<size_t>(s)][static_cast<size_t>(t)] =
                        cell.get<double>();
            }
        }
        return true;
    };

    if (readMatrix("fmMatrix", inst.fmMatrix))
        inst.hasFmMatrix = true;
    readMatrix("fmDelays", inst.fmDelays);
    return inst;
}

nlohmann::json instrumentToJson(const Instrument& inst) {
    nlohmann::json j;
    j["type"] = kTypeNames[inst.typeIndex];

    if (inst.hasFmMatrix) {
        auto m = nlohmann::json::array();
        for (int s = 0; s < inst.oscCount; ++s) {
            auto row = nlohmann::json::array();
            for (int t = 0; t < inst.oscCount; ++t)
                row.push_back(inst.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)]);
            m.push_back(row);
        }
        j["fmMatrix"] = m;
    } else {
        j["fmMatrix"] = nullptr;
    }

    auto arr = nlohmann::json::array();
    for (int i = 0; i < inst.oscCount; ++i) {
        const Osc& d = inst.oscs[static_cast<size_t>(i)];
        nlohmann::json o;
        o["waveform"] = kWaveNames[static_cast<int>(d.waveform)];
        o["adsrGain"] = adsrToJson(d.adsrGain);
        o["filterType"] = kFilterNames[static_cast<int>(d.filterType)];
        o["adsrFilter"] = adsrToJson(d.adsrFilter);
        o["filterQ"] = d.filterQ;
        o["adsrFilterQ"] = adsrToJson(d.adsrFilterQ);
        o["gLFO"] = lfoToJson(d.gLfo);
        o["fLFO"] = lfoToJson(d.fLfo);
        o["pLFO"] = lfoToJson(d.pLfo);

        if (d.fm.on)
            o["FM"] = nlohmann::json{{"type", kWaveNames[static_cast<int>(d.fm.type)]},
                                     {"frequency", d.fm.frequency},
                                     {"depth", d.fm.depth}};
        else
            o["FM"] = false;

        if (d.pEnv.on) {
            nlohmann::json p = adsrToJson(d.pEnv.env);
            p["amount"] = d.pEnv.amount;
            o["pENV"] = p;
        } else {
            o["pENV"] = false;
        }

        // Written only when present, matching zyn's undefined-key behaviour.
        if (d.dist.on) {
            const char* os = d.dist.oversample == 4 ? "4x"
                           : d.dist.oversample == 2 ? "2x" : "none";
            o["dist"] = nlohmann::json{{"amount", d.dist.amount}, {"oversample", os}};
        }

        o["oct"] = d.oct;
        o["detune"] = d.detune;

        nlohmann::json fx;
        fx["del"] = d.del.on ? nlohmann::json{{"time", d.del.time},
                                              {"feedback", d.del.feedback}}
                             : nlohmann::json(nullptr);
        fx["verb"] = d.verb.on ? nlohmann::json{{"duration", d.verb.duration},
                                                {"decay", d.verb.decay}}
                               : nlohmann::json(nullptr);
        o["fx"] = fx;

        arr.push_back(o);
    }
    j["oscs"] = arr;
    return j;
}

} // namespace sl
