#pragma once
#include "sl/Instrument.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sl {

// A saved patch: the seed that generates it, plus the designer's edit when
// there is one.
//
// The instrument is stored only for an edited patch. Storing it always would
// give a preset two sources of truth, and a later change to the generator would
// silently put them out of step -- the seed would say one thing and the saved
// parameters another, with nothing to say which was intended.
struct UserPreset {
    std::string name;
    uint32_t seed = 0;
    int octave = 0;
    bool edited = false;
    Instrument instrument{};
    std::string file;      // absolute path, filled in by the store
};

// User presets on disk, one JSON file each.
//
// One file per preset rather than a single bank file: a bank has to be
// rewritten in full on every save, so an interrupted write loses every preset
// rather than one, and two plugin instances saving at once lose each other's
// work.
class UserPresetStore {
public:
    // Creates the directory if it does not exist. Everything else is a no-op
    // until this succeeds.
    bool open(const std::string& directory);

    bool ready() const { return ready_; }
    const std::string& directory() const { return dir_; }
    const std::string& error() const { return error_; }

    // Re-reads the directory. Files that fail to parse are skipped, not fatal.
    void refresh();
    const std::vector<UserPreset>& presets() const { return presets_; }

    // Writes a preset, overwriting any existing one with the same name, and
    // refreshes. Returns false and fills error() on failure.
    bool save(const UserPreset& preset);
    bool remove(const std::string& name);
    bool rename(const std::string& from, const std::string& to);

private:
    std::string pathFor(const std::string& name) const;

    std::string dir_;
    std::string error_;
    std::vector<UserPreset> presets_;
    bool ready_ = false;
};

// Turns a user-typed name into something safe to use as a file name, keeping
// it recognisable. Empty or entirely unusable input becomes "Untitled".
std::string sanitisePresetName(const std::string& name);

} // namespace sl
