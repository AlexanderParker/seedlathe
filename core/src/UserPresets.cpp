#include "UserPresets.h"

#include "PresetIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
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

// A preset is a couple of kilobytes: the instrument JSON runs to about 1.7 kB
// for five oscillators. A megabyte is far beyond anything this writes, and
// stops a large file that happens to be sitting in the folder from being
// parsed in full before it is rejected.
constexpr std::uintmax_t kMaxPresetBytes = 1024 * 1024;

// Long enough for any name worth typing. The value is whatever the file says,
// so it is bounded before it reaches a list control.
constexpr size_t kMaxNameChars = 128;

void UserPresetStore::refresh() {
    presets_.clear();
    if (!ready_) return;

    // Every filesystem call here uses its error_code overload, and the
    // iterator is advanced by hand. The range-for form calls the THROWING
    // operator++, and directory_iterator(dir, ec) only reports errors from
    // construction -- so a preset disappearing mid-scan, which another plugin
    // instance, an antivirus quarantine or a sync client can all cause, throws
    // filesystem_error out of a function called during plugin construction and
    // editor layout.
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

        const std::uintmax_t size = entry.file_size(fileEc);
        if (fileEc || size == 0 || size > kMaxPresetBytes) continue;

        std::ifstream f(entry.path());
        if (!f) continue;

        UserPreset p;
        try {
            const nlohmann::json j = nlohmann::json::parse(f);
            p.name = j.value("name", entry.path().stem().string());
            if (p.name.size() > kMaxNameChars) p.name.resize(kMaxNameChars);
            p.seed = j.value("seed", 0u);
            p.octave = std::clamp(j.value("octave", 0), -3, 3);
            if (j.contains("instrument") && !j["instrument"].is_null()) {
                p.instrument = instrumentFromJson(j["instrument"]);
                p.edited = true;
            }
        } catch (const std::exception&) {
            // A file someone hand-edited into invalid JSON should hide itself,
            // not take the whole preset list down with it.
            continue;
        }
        if (p.name.empty()) continue;

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
