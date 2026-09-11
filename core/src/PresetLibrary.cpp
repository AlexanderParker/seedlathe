#include "PresetLibrary.h"

#include "PresetIO.h"
#include "sl/FactoryPresets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace sl {
namespace {

// Conservative on purpose: this has to be a legal file name on Windows, macOS
// and Linux at once, so the intersection of their rules is what applies.
bool safeChar(unsigned char c) {
    return std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' ||
           c == '(' || c == ')';
}

// A pack is a couple of hundred kilobytes at worst: the instrument JSON runs
// to about 1.7 kB for five oscillators, so even a thousand edited presets fit
// in two megabytes. Eight stops a large file that happens to be sitting in the
// folder from being parsed in full before it is rejected.
constexpr std::uintmax_t kMaxPackBytes = 8u * 1024u * 1024u;

const char* typeCategory(int typeIndex) {
    static const char* kNames[] = {"Pad", "Lead", "Bass", "Key", "Pluck",
                                   "Bell", "String", "Drum", "Perc", "FX"};
    return (typeIndex >= 0 && typeIndex < 10) ? kNames[typeIndex] : "Other";
}

// zyn's preset volume is a multiplier around 1; the plugin's parameter is a
// percentage. One conversion, in one place, so the two cannot drift.
double volumeFromZyn(double multiplier) { return multiplier * 100.0; }

Preset presetFromJson(const nlohmann::json& j) {
    Preset p;
    p.name = j.value("name", std::string());
    if (p.name.size() > kMaxPresetNameChars) p.name.resize(kMaxPresetNameChars);
    p.category = j.value("category", std::string());
    if (p.category.size() > kMaxPresetNameChars) p.category.resize(kMaxPresetNameChars);
    p.seed = j.value("seed", 0u);
    p.octave = std::clamp(j.value("octave", 0), -3, 3);
    p.volume = std::clamp(j.value("volume", 100.0), 0.0, 500.0);
    p.cutoff = std::clamp(j.value("cutoff", 0.0), -48.0, 48.0);
    p.resonance = std::clamp(j.value("resonance", 0.0), -30.0, 30.0);
    if (j.contains("instrument") && !j["instrument"].is_null()) {
        p.instrument = instrumentFromJson(j["instrument"]);
        p.edited = true;
    }
    return p;
}

nlohmann::json presetToJson(const Preset& p) {
    nlohmann::json j;
    j["name"] = p.name;
    j["category"] = p.category;
    j["seed"] = p.seed;
    j["octave"] = p.octave;
    j["volume"] = p.volume;
    j["cutoff"] = p.cutoff;
    j["resonance"] = p.resonance;
    // Only for an edited patch: see the note on Preset.
    if (p.edited) j["instrument"] = instrumentToJson(p.instrument);
    return j;
}

// The compiled-in bank, as a pack. It is not a file, so it cannot go missing
// and there is nothing to install: a fresh copy of the plugin has 115 sounds
// before it has touched the disk.
PresetPack builtInPack() {
    PresetPack pack;
    pack.name = "Factory";
    pack.builtIn = true;
    pack.presets.reserve(kNumFactoryPresets);

    // Emitted in instrument-type order rather than the order the exporter
    // happened to produce. A browser that prints a heading whenever the
    // category changes would otherwise show "Perc" four times, and the type
    // order is the same one the roll-type grid uses.
    for (int t = 0; t < 10; ++t) {
        for (int i = 0; i < kNumFactoryPresets; ++i) {
            const FactoryPreset& f = kFactoryPresets[i];
            if (f.typeIndex != t) continue;
            Preset p;
            p.name = f.name;
            p.category = typeCategory(f.typeIndex);
            p.seed = f.seed;
            p.octave = std::clamp(f.octave, -3, 3);
            p.volume = volumeFromZyn(f.volume);
            pack.presets.push_back(std::move(p));
        }
    }
    return pack;
}

bool readJsonFile(const fs::path& path, nlohmann::json& out, std::string& why) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) { why = "could not stat the file"; return false; }
    if (size == 0 || size > kMaxPackBytes) { why = "implausible file size"; return false; }

    std::ifstream f(path);
    if (!f) { why = "could not open the file"; return false; }
    try {
        out = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
    return true;
}

} // namespace

std::string sanitisePresetName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
        out.push_back(safeChar(c) ? static_cast<char>(c) : '_');

    // Windows rejects trailing dots and spaces, silently, by stripping them --
    // which would make save and delete disagree about the file name.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());

    if (out.empty()) out = "Untitled";
    if (out.size() > kMaxPresetNameChars) out.resize(kMaxPresetNameChars);
    return out;
}

bool PresetLibrary::open(const std::string& directory) {
    std::error_code ec;
    dir_ = directory;
    fs::create_directories(fs::u8path(dir_), ec);
    // An unwritable folder is not fatal: the built-in bank still loads, and
    // the user finds out when they try to save rather than on startup.
    ready_ = !ec && fs::is_directory(fs::u8path(dir_), ec);
    if (!ready_) error_ = "could not open " + dir_;
    else error_.clear();

    refresh();
    return ready_;
}

void PresetLibrary::refresh() {
    packs_.clear();
    packs_.push_back(builtInPack());

    if (ready_) migrateLooseFiles();
    if (!ready_) return;

    // Every filesystem call uses its error_code overload and the iterator is
    // advanced by hand. The range-for form calls the THROWING operator++, and
    // a file disappearing mid-scan -- another instance, an antivirus
    // quarantine, a sync client -- would throw out of a function called
    // during plugin construction.
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir_), ec);
    if (ec) return;

    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& entry = *it;

        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) continue;
        if (entry.path().extension() != ".json") continue;

        nlohmann::json j;
        std::string why;
        if (!readJsonFile(entry.path(), j, why)) continue;
        if (!j.is_object() || !j.contains("presets") || !j["presets"].is_array())
            continue;

        PresetPack pack;
        pack.name = j.value("name", entry.path().stem().string());
        if (pack.name.size() > kMaxPresetNameChars) pack.name.resize(kMaxPresetNameChars);
        if (pack.name.empty()) pack.name = entry.path().stem().string();
        pack.file = entry.path().string();

        for (const auto& pj : j["presets"]) {
            if (!pj.is_object()) continue;
            try {
                Preset p = presetFromJson(pj);
                if (p.name.empty()) continue;
                pack.presets.push_back(std::move(p));
            } catch (const std::exception&) {
                // One malformed entry loses that preset, not the pack.
            }
        }

        // A pack whose name collides with the built-in one would be
        // unreachable through the browser, so it is disambiguated rather than
        // silently shadowed.
        while (findPack(pack.name)) pack.name += " (2)";
        packs_.push_back(std::move(pack));
    }

    for (auto& pack : packs_) {
        if (pack.builtIn) continue;   // the factory order is deliberate
        std::sort(pack.presets.begin(), pack.presets.end(),
                  [](const Preset& a, const Preset& b) {
                      if (a.category != b.category) return a.category < b.category;
                      return a.name < b.name;
                  });
    }
    std::sort(packs_.begin() + 1, packs_.end(),
              [](const PresetPack& a, const PresetPack& b) { return a.name < b.name; });
}

void PresetLibrary::migrateLooseFiles() {
    // The first format wrote one file per preset, with no "presets" array.
    // Anyone who saved before the change has a folder full of them, and they
    // must not simply stop appearing.
    std::error_code ec;
    fs::directory_iterator it(fs::u8path(dir_), ec);
    if (ec) return;

    std::vector<fs::path> loose;
    std::vector<Preset> found;

    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::directory_entry& entry = *it;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) continue;
        if (entry.path().extension() != ".json") continue;

        nlohmann::json j;
        std::string why;
        if (!readJsonFile(entry.path(), j, why)) continue;
        if (!j.is_object() || j.contains("presets")) continue;   // already a pack
        if (!j.contains("seed")) continue;                       // not ours

        try {
            Preset p = presetFromJson(j);
            if (p.name.empty()) p.name = entry.path().stem().string();
            if (p.category.empty()) p.category = "Imported";
            found.push_back(std::move(p));
            loose.push_back(entry.path());
        } catch (const std::exception&) {
            continue;
        }
    }

    if (found.empty()) return;

    PresetPack pack;
    pack.name = kDefaultPackName;
    pack.file = pathForPack(pack.name);
    pack.presets = std::move(found);

    // Fold into an existing default pack rather than overwrite it.
    nlohmann::json existing;
    std::string why;
    if (readJsonFile(fs::u8path(pack.file), existing, why) && existing.is_object() &&
        existing.contains("presets") && existing["presets"].is_array()) {
        for (const auto& pj : existing["presets"]) {
            if (!pj.is_object()) continue;
            try {
                Preset p = presetFromJson(pj);
                if (p.name.empty()) continue;
                const bool clash = std::any_of(
                    pack.presets.begin(), pack.presets.end(),
                    [&p](const Preset& q) { return q.name == p.name; });
                if (!clash) pack.presets.push_back(std::move(p));
            } catch (const std::exception&) {
            }
        }
    }

    if (!writePack(pack)) return;

    // Only once the pack is safely on disk.
    for (const auto& path : loose) {
        std::error_code rmEc;
        fs::remove(path, rmEc);
    }
}

std::vector<std::string> PresetLibrary::categories() const {
    std::set<std::string> seen;
    for (const auto& pack : packs_)
        for (const auto& p : pack.presets)
            if (!p.category.empty()) seen.insert(p.category);
    return std::vector<std::string>(seen.begin(), seen.end());
}

PresetPack* PresetLibrary::findPack(const std::string& name) {
    for (auto& pack : packs_)
        if (pack.name == name) return &pack;
    return nullptr;
}

const PresetPack* PresetLibrary::findPack(const std::string& name) const {
    for (const auto& pack : packs_)
        if (pack.name == name) return &pack;
    return nullptr;
}

const Preset* PresetLibrary::find(const std::string& pack,
                                  const std::string& name) const {
    const PresetPack* p = findPack(pack);
    if (!p) return nullptr;
    for (const auto& preset : p->presets)
        if (preset.name == name) return &preset;
    return nullptr;
}

std::string PresetLibrary::pathForPack(const std::string& name) const {
    return (fs::u8path(dir_) / (sanitisePresetName(name) + ".json")).string();
}

bool PresetLibrary::writePack(const PresetPack& pack) {
    nlohmann::json j;
    j["format"] = kPackFormatTag;
    j["version"] = kPackFormatVersion;
    j["name"] = pack.name;
    j["presets"] = nlohmann::json::array();
    for (const auto& p : pack.presets) j["presets"].push_back(presetToJson(p));

    const std::string path = pack.file.empty() ? pathForPack(pack.name) : pack.file;

    // Write beside the target and rename over it. A crash or a full disk
    // partway through a direct write would leave the whole pack truncated,
    // which for a one-file-per-pack format means losing every preset in it
    // rather than one.
    const std::string temp = path + ".tmp";
    {
        std::ofstream f(temp, std::ios::trunc);
        if (!f) { error_ = "could not write " + temp; return false; }
        f << j.dump(2);
        if (!f) { error_ = "write failed for " + temp; return false; }
    }

    std::error_code ec;
    fs::rename(fs::u8path(temp), fs::u8path(path), ec);
    if (ec) {
        fs::remove(fs::u8path(temp), ec);
        error_ = "could not replace " + path;
        return false;
    }
    error_.clear();
    return true;
}

bool PresetLibrary::save(const std::string& packName, const Preset& preset) {
    if (!ready_) { error_ = "no preset directory"; return false; }
    if (preset.name.empty()) { error_ = "a preset needs a name"; return false; }

    const std::string target = packName.empty() ? kDefaultPackName : packName;
    if (const PresetPack* existing = findPack(target)) {
        if (existing->builtIn) {
            error_ = "the Factory pack is read-only -- save to another pack";
            return false;
        }
    }

    // Re-read from disk rather than trusting the in-memory copy: another
    // instance may have written to this pack since the last refresh, and a
    // whole-file rewrite would otherwise drop its work.
    PresetPack pack;
    pack.name = target;
    pack.file = pathForPack(target);

    nlohmann::json j;
    std::string why;
    if (readJsonFile(fs::u8path(pack.file), j, why) && j.is_object() &&
        j.contains("presets") && j["presets"].is_array()) {
        pack.name = j.value("name", target);
        for (const auto& pj : j["presets"]) {
            if (!pj.is_object()) continue;
            try {
                Preset p = presetFromJson(pj);
                if (!p.name.empty()) pack.presets.push_back(std::move(p));
            } catch (const std::exception&) {
            }
        }
    }

    const auto it = std::find_if(pack.presets.begin(), pack.presets.end(),
                                 [&preset](const Preset& p) { return p.name == preset.name; });
    if (it != pack.presets.end()) *it = preset;
    else pack.presets.push_back(preset);

    if (!writePack(pack)) return false;
    refresh();
    return true;
}

bool PresetLibrary::remove(const std::string& packName, const std::string& name) {
    if (!ready_) { error_ = "no preset directory"; return false; }
    PresetPack* pack = findPack(packName);
    if (!pack) { error_ = "no such pack"; return false; }
    if (pack->builtIn) { error_ = "the Factory pack is read-only"; return false; }

    const auto it = std::find_if(pack->presets.begin(), pack->presets.end(),
                                 [&name](const Preset& p) { return p.name == name; });
    if (it == pack->presets.end()) { error_ = "no such preset"; return false; }

    PresetPack copy = *pack;
    copy.presets.erase(copy.presets.begin() + (it - pack->presets.begin()));

    // An empty pack is removed outright rather than left as a file with
    // nothing in it, which would clutter the browser with a dead heading.
    if (copy.presets.empty()) {
        std::error_code ec;
        fs::remove(fs::u8path(copy.file.empty() ? pathForPack(copy.name) : copy.file), ec);
        if (ec) { error_ = ec.message(); return false; }
        error_.clear();
        refresh();
        return true;
    }

    if (!writePack(copy)) return false;
    refresh();
    return true;
}

bool PresetLibrary::rename(const std::string& packName, const std::string& from,
                           const std::string& to) {
    const PresetPack* pack = findPack(packName);
    if (!pack) { error_ = "no such pack"; return false; }
    if (pack->builtIn) { error_ = "the Factory pack is read-only"; return false; }

    const Preset* p = find(packName, from);
    if (!p) { error_ = "no such preset"; return false; }

    Preset moved = *p;
    moved.name = to;
    if (!save(packName, moved)) return false;
    if (from != to) return remove(packName, from);
    return true;
}

bool PresetLibrary::move(const std::string& fromPack, const std::string& name,
                         const std::string& toPack, const std::string& toCategory) {
    const Preset* p = find(fromPack, name);
    if (!p) { error_ = "no such preset"; return false; }

    Preset moved = *p;
    moved.category = toCategory;
    if (!save(toPack, moved)) return false;

    // Copy first, then drop the original -- and only when it really moved.
    if (fromPack != toPack) {
        const PresetPack* src = findPack(fromPack);
        if (src && !src->builtIn) return remove(fromPack, name);
    }
    return true;
}

bool PresetLibrary::importPack(const std::string& path, std::string* importedAs) {
    if (!ready_) { error_ = "no preset directory"; return false; }

    nlohmann::json j;
    std::string why;
    if (!readJsonFile(fs::u8path(path), j, why)) { error_ = why; return false; }
    if (!j.is_object() || !j.contains("presets") || !j["presets"].is_array()) {
        error_ = "not a Seedlathe preset pack";
        return false;
    }

    PresetPack pack;
    pack.name = j.value("name", fs::u8path(path).stem().string());
    if (pack.name.size() > kMaxPresetNameChars) pack.name.resize(kMaxPresetNameChars);
    if (pack.name.empty()) pack.name = "Imported";

    for (const auto& pj : j["presets"]) {
        if (!pj.is_object()) continue;
        try {
            Preset p = presetFromJson(pj);
            if (!p.name.empty()) pack.presets.push_back(std::move(p));
        } catch (const std::exception&) {
        }
    }
    if (pack.presets.empty()) { error_ = "that pack has no presets in it"; return false; }

    // Never overwrite a pack that is already there: an import is additive,
    // and silently replacing someone's bank because two packs share a name
    // is not a recoverable mistake.
    std::string name = pack.name;
    int suffix = 2;
    while (findPack(name) ||
           fs::exists(fs::u8path(pathForPack(name)))) {
        name = pack.name + " (" + std::to_string(suffix++) + ")";
        if (suffix > 99) { error_ = "too many packs of that name"; return false; }
    }
    pack.name = name;
    pack.file = pathForPack(name);

    if (!writePack(pack)) return false;
    if (importedAs) *importedAs = name;
    refresh();
    return true;
}

bool PresetLibrary::exportPack(const std::string& packName, const std::string& path) {
    const PresetPack* pack = findPack(packName);
    if (!pack) { error_ = "no such pack"; return false; }

    PresetPack out = *pack;
    out.file = path;
    out.builtIn = false;
    return writePack(out);
}

} // namespace sl
