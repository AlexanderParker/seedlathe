#include <catch2/catch_test_macros.hpp>

#include "UserPresets.h"
#include "sl/InstrumentGen.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// A directory of its own per test case, removed afterwards, so one test cannot
// see another's presets.
struct TempDir {
    explicit TempDir(const char* name)
    : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
    fs::path path;
};

} // namespace

TEST_CASE("sanitisePresetName keeps names legal on every platform") {
    REQUIRE(sl::sanitisePresetName("Warm Pad") == "Warm Pad");
    REQUIRE(sl::sanitisePresetName("bass/lead") == "bass_lead");
    REQUIRE(sl::sanitisePresetName("a:b*c?d") == "a_b_c_d");
    // Windows strips trailing dots and spaces itself, which would make save and
    // delete disagree about the file name.
    REQUIRE(sl::sanitisePresetName("trailing. ") == "trailing");
    REQUIRE(sl::sanitisePresetName("   ") == "Untitled");
    REQUIRE(sl::sanitisePresetName("") == "Untitled");
    REQUIRE(sl::sanitisePresetName(std::string(200, 'x')).size() == 64);
}

TEST_CASE("a saved preset comes back with the same seed and octave") {
    TempDir dir("sl_presets_basic");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));
    REQUIRE(store.presets().empty());

    sl::UserPreset p;
    p.name = "Warm Pad";
    p.seed = 3703184240u;
    p.octave = -1;
    REQUIRE(store.save(p));

    REQUIRE(store.presets().size() == 1);
    REQUIRE(store.presets()[0].name == "Warm Pad");
    REQUIRE(store.presets()[0].seed == 3703184240u);
    REQUIRE(store.presets()[0].octave == -1);
    REQUIRE_FALSE(store.presets()[0].edited);

    // A second store over the same directory sees it too.
    sl::UserPresetStore reopened;
    REQUIRE(reopened.open(dir.str()));
    REQUIRE(reopened.presets().size() == 1);
    REQUIRE(reopened.presets()[0].seed == 3703184240u);
}

TEST_CASE("an edited preset carries its instrument, an unedited one does not") {
    TempDir dir("sl_presets_edited");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));

    sl::UserPreset plain;
    plain.name = "From seed";
    plain.seed = 13u;
    REQUIRE(store.save(plain));

    sl::UserPreset edited;
    edited.name = "Hand tuned";
    edited.seed = 13u;
    edited.edited = true;
    edited.instrument = sl::generateInstrument(13u);
    edited.instrument.oscs[0].detune = 7.25;      // something the seed never produces
    REQUIRE(store.save(edited));

    REQUIRE(store.presets().size() == 2);
    for (const auto& p : store.presets()) {
        if (p.name == "From seed") {
            REQUIRE_FALSE(p.edited);
        } else {
            REQUIRE(p.edited);
            REQUIRE(p.instrument.oscs[0].detune == 7.25);
        }
    }
}

TEST_CASE("saving over a name replaces it rather than duplicating it") {
    TempDir dir("sl_presets_overwrite");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));

    sl::UserPreset p;
    p.name = "Take";
    p.seed = 1u;
    REQUIRE(store.save(p));
    p.seed = 2u;
    REQUIRE(store.save(p));

    REQUIRE(store.presets().size() == 1);
    REQUIRE(store.presets()[0].seed == 2u);
}

TEST_CASE("rename keeps the preset, delete removes it") {
    TempDir dir("sl_presets_rename");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));

    sl::UserPreset p;
    p.name = "Before";
    p.seed = 99u;
    REQUIRE(store.save(p));

    REQUIRE(store.rename("Before", "After"));
    REQUIRE(store.presets().size() == 1);
    REQUIRE(store.presets()[0].name == "After");
    REQUIRE(store.presets()[0].seed == 99u);

    REQUIRE_FALSE(store.rename("Before", "Elsewhere"));   // already moved

    REQUIRE(store.remove("After"));
    REQUIRE(store.presets().empty());
    REQUIRE_FALSE(store.remove("After"));
}

TEST_CASE("a corrupt preset file hides itself instead of breaking the list") {
    TempDir dir("sl_presets_corrupt");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));

    sl::UserPreset good;
    good.name = "Good";
    good.seed = 7u;
    REQUIRE(store.save(good));

    {
        std::ofstream f((dir.path / "broken.json").string());
        f << "{ this is not json";
    }
    store.refresh();

    REQUIRE(store.presets().size() == 1);
    REQUIRE(store.presets()[0].name == "Good");
}

TEST_CASE("the preset scan ignores what is not a preset") {
    TempDir dir("sl_presets_junk");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));

    sl::UserPreset good;
    good.name = "Keeper";
    good.seed = 42u;
    REQUIRE(store.save(good));

    // A directory whose name ends in .json. is_regular_file has to be what
    // decides, not the extension -- opening a directory as a file and parsing
    // it is not something to find out about in front of a user.
    std::filesystem::create_directory(dir.path / "afolder.json");

    // Empty file, and one far larger than any preset this writes.
    { std::ofstream f((dir.path / "empty.json").string()); }
    {
        std::ofstream f((dir.path / "huge.json").string(), std::ios::binary);
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 24; ++i) f << chunk;    // ~1.5 MB
    }
    // Right shape, wrong types.
    {
        std::ofstream f((dir.path / "wrongtypes.json").string());
        f << R"({"name": 12345, "seed": "not a number", "octave": []})";
    }
    // Not a preset at all, but valid JSON.
    {
        std::ofstream f((dir.path / "other.json").string());
        f << R"({"unrelated": true})";
    }

    store.refresh();

    // The valid preset survives; so does the one that is merely missing fields,
    // since defaults cover it. Nothing else gets through, and nothing throws.
    bool sawKeeper = false, sawOther = false;
    for (const auto& p : store.presets()) {
        if (p.name == "Keeper") sawKeeper = true;
        if (p.name == "other") sawOther = true;
        REQUIRE_FALSE(p.name.empty());
        REQUIRE(p.octave >= -3);
        REQUIRE(p.octave <= 3);
    }
    REQUIRE(sawKeeper);
    REQUIRE(sawOther);          // defaults to its file name, seed 0
    REQUIRE(store.presets().size() == 2);
}

TEST_CASE("an out-of-range octave in a preset file is clamped") {
    // The value is whatever the file says, and it is handed straight to a
    // parameter with a -3..3 range.
    TempDir dir("sl_presets_octave");
    sl::UserPresetStore store;
    REQUIRE(store.open(dir.str()));
    {
        std::ofstream f((dir.path / "wild.json").string());
        f << R"({"name": "Wild", "seed": 7, "octave": 9000})";
    }
    store.refresh();
    REQUIRE(store.presets().size() == 1);
    REQUIRE(store.presets()[0].octave == 3);
}
