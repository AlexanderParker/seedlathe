#include "UserPresets.h"

#include "PresetIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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
    if (out.size() > 64) out.resize(64);
    return out;
}

bool UserPresetStore::open(const std::string& directory) {
    std::error_code ec;
    fs::create_directories(fs::u8path(directory), ec);
    if (ec) {
        error_ = ec.message();
        ready_ = false;
        return false;
    }
    dir_ = directory;
    ready_ = true;
    error_.clear();
    refresh();
    return true;
}

std::string UserPresetStore::pathFor(const std::string& name) const {
    return (fs::u8path(dir_) / (sanitisePresetName(name) + ".json")).string();
}

void UserPresetStore::refresh() {
    presets_.clear();
    if (!ready_) return;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::u8path(dir_), ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;

        std::ifstream f(entry.path());
        if (!f) continue;

        UserPreset p;
        try {
            const nlohmann::json j = nlohmann::json::parse(f);
            p.name = j.value("name", entry.path().stem().string());
            p.seed = j.value("seed", 0u);
            p.octave = j.value("octave", 0);
            if (j.contains("instrument") && !j["instrument"].is_null()) {
                p.instrument = instrumentFromJson(j["instrument"]);
                p.edited = true;
            }
        } catch (const std::exception&) {
            // A file someone hand-edited into invalid JSON should hide itself,
            // not take the whole preset list down with it.
            continue;
        }
        p.file = entry.path().string();
        presets_.push_back(std::move(p));
    }

    std::sort(presets_.begin(), presets_.end(),
              [](const UserPreset& a, const UserPreset& b) { return a.name < b.name; });
}

bool UserPresetStore::save(const UserPreset& preset) {
    if (!ready_) { error_ = "no preset directory"; return false; }

    nlohmann::json j;
    j["name"] = preset.name;
    j["seed"] = preset.seed;
    j["octave"] = preset.octave;
    if (preset.edited)
        j["instrument"] = instrumentToJson(preset.instrument);

    const std::string path = pathFor(preset.name);
    {
        std::ofstream f(path, std::ios::trunc);
        if (!f) { error_ = "could not write " + path; return false; }
        f << j.dump(2);
        if (!f) { error_ = "write failed for " + path; return false; }
    }
    error_.clear();
    refresh();
    return true;
}

bool UserPresetStore::remove(const std::string& name) {
    if (!ready_) { error_ = "no preset directory"; return false; }

    std::error_code ec;
    if (!fs::remove(fs::u8path(pathFor(name)), ec) || ec) {
        error_ = ec ? ec.message() : "no such preset";
        return false;
    }
    error_.clear();
    refresh();
    return true;
}

bool UserPresetStore::rename(const std::string& from, const std::string& to) {
    if (!ready_) { error_ = "no preset directory"; return false; }

    const auto it = std::find_if(presets_.begin(), presets_.end(),
                                 [&from](const UserPreset& p) { return p.name == from; });
    if (it == presets_.end()) { error_ = "no such preset"; return false; }

    // Save under the new name first, then drop the old file. The other order
    // loses the preset outright if the write fails.
    UserPreset moved = *it;
    moved.name = to;
    if (!save(moved)) return false;
    if (sanitisePresetName(from) != sanitisePresetName(to))
        remove(from);
    return true;
}

} // namespace sl
