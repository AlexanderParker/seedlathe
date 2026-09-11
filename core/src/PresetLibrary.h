#pragma once
#include "sl/Instrument.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sl {

// A saved sound.
//
// The seed is the whole instrument for an unedited preset, and the instrument
// is stored only when the designer has been used -- storing it always would
// give a preset two sources of truth that a later generator change could put
// out of step, with nothing to say which was intended.
//
// The four performance values travel with it because a preset is a SOUND, not
// a patch: a pad wants a different level and octave from a kick, and the
// filter offsets are part of how a preset was voiced.
struct Preset {
    std::string name;
    std::string category;        // free text; the instrument type by default
    uint32_t seed = 0;
    int octave = 0;
    double volume = 100.0;       // percent, as the host parameter reads it
    double cutoff = 0.0;         // semitones of filter transposition
    double resonance = 0.0;      // dB added to the filter Q
    bool edited = false;
    Instrument instrument{};
};

// A named collection of presets. One JSON file on disk, or the built-in bank.
//
// One file per pack rather than one per preset: a pack is the unit people
// share, and "send me your pack" should be one attachment rather than a zip
// of ninety files. The cost is that two plugin instances saving into the same
// pack at once could lose one of the two writes, which save() narrows by
// re-reading the file immediately before rewriting it.
struct PresetPack {
    std::string name;
    std::string file;            // absolute path; empty for the built-in bank
    bool builtIn = false;
    std::vector<Preset> presets;
};

// Longest name a preset or pack may carry. sanitisePresetName truncates to
// it, so a text entry that collects one must allow at least this many
// characters -- IGraphics defaults every entry to seven.
inline constexpr size_t kMaxPresetNameChars = 64;

// The pack that Save writes to when the user does not name another.
inline constexpr const char* kDefaultPackName = "My Presets";

// What the file format calls itself, so a JSON file that is not one of ours
// is refused by name rather than half-read.
inline constexpr const char* kPackFormatTag = "seedlathe-pack";
inline constexpr int kPackFormatVersion = 1;

// The whole library: the built-in bank plus every pack file in a folder.
class PresetLibrary {
public:
    // Creates the directory if it does not exist. The built-in bank is
    // available either way -- it is compiled in, so there is no state of the
    // filesystem in which the user has no presets at all.
    bool open(const std::string& directory);

    bool ready() const { return ready_; }
    const std::string& directory() const { return dir_; }
    const std::string& error() const { return error_; }

    // Re-reads the folder. Files that fail to parse are skipped, not fatal:
    // one hand-edited pack must not take the whole library down with it.
    void refresh();

    const std::vector<PresetPack>& packs() const { return packs_; }

    // Every distinct category across the library, sorted, for the browser's
    // filter. The built-in bank contributes the ten instrument types.
    std::vector<std::string> categories() const;

    // Finds a preset by pack and name, or null.
    const Preset* find(const std::string& pack, const std::string& name) const;

    // Writes `p` into `pack`, creating the pack if it does not exist and
    // replacing any preset of the same name within it. Refuses to write to
    // the built-in bank.
    bool save(const std::string& pack, const Preset& p);

    bool remove(const std::string& pack, const std::string& name);
    bool rename(const std::string& pack, const std::string& from,
                const std::string& to);

    // Moves a preset between packs, or between categories within one.
    bool move(const std::string& fromPack, const std::string& name,
              const std::string& toPack, const std::string& toCategory);

    // Copies a pack file into the library folder. Returns the name it landed
    // under in `importedAs`, which differs from the file's own name when one
    // of that name was already there.
    bool importPack(const std::string& path, std::string* importedAs = nullptr);

    // Writes a pack out as a single file, for sharing. Works on the built-in
    // bank too, which is how you get an editable copy of the factory sounds.
    bool exportPack(const std::string& pack, const std::string& path);

private:
    PresetPack* findPack(const std::string& name);
    const PresetPack* findPack(const std::string& name) const;
    std::string pathForPack(const std::string& name) const;
    bool writePack(const PresetPack& pack);

    // Folds any pre-pack single-preset files into one pack, so an existing
    // user bank survives the format change instead of vanishing.
    void migrateLooseFiles();

    std::string dir_;
    std::string error_;
    std::vector<PresetPack> packs_;
    bool ready_ = false;
};

// Turns a user-typed name into something safe to use as a file name, keeping
// it recognisable. Empty or entirely unusable input becomes "Untitled".
std::string sanitisePresetName(const std::string& name);

} // namespace sl
