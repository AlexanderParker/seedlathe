#include <catch2/catch_test_macros.hpp>

#include "PresetLibrary.h"
#include "sl/InstrumentGen.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// A directory of its own per test case, removed afterwards, so one test
// cannot see another's packs.
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

sl::Preset makePreset(const char* name, const char* category, uint32_t seed) {
    sl::Preset p;
    p.name = name;
    p.category = category;
    p.seed = seed;
    return p;
}

const sl::PresetPack* packNamed(const sl::PresetLibrary& lib, const std::string& name) {
    for (const auto& pack : lib.packs())
        if (pack.name == name) return &pack;
    return nullptr;
}

void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::trunc);
    f << text;
}

} // namespace

TEST_CASE("sanitisePresetName keeps names legal on every platform") {
    REQUIRE(sl::sanitisePresetName("Warm Pad") == "Warm Pad");
    REQUIRE(sl::sanitisePresetName("bass/lead") == "bass_lead");
    REQUIRE(sl::sanitisePresetName("a:b*c?d") == "a_b_c_d");
    // Windows strips trailing dots and spaces itself, which would make save
    // and delete disagree about the file name.
    REQUIRE(sl::sanitisePresetName("trailing. ") == "trailing");
    REQUIRE(sl::sanitisePresetName("   ") == "Untitled");
    REQUIRE(sl::sanitisePresetName("") == "Untitled");
    REQUIRE(sl::sanitisePresetName(std::string(200, 'x')).size() ==
            sl::kMaxPresetNameChars);
}

TEST_CASE("the built-in bank is there before the disk is touched") {
    // Compiled in, not installed: there is no state of the filesystem in
    // which the user opens the plugin and has no presets at all.
    sl::PresetLibrary lib;
    TempDir dir("sl_lib_builtin");
    REQUIRE(lib.open(dir.str()));

    const sl::PresetPack* factory = packNamed(lib, "Factory");
    REQUIRE(factory != nullptr);
    REQUIRE(factory->builtIn);
    REQUIRE(factory->presets.size() > 100);
    REQUIRE(factory->file.empty());

    // Its categories are the instrument types, which is what the browser
    // filter offers.
    const auto cats = lib.categories();
    REQUIRE(std::find(cats.begin(), cats.end(), "Pad") != cats.end());
    REQUIRE(std::find(cats.begin(), cats.end(), "Drum") != cats.end());
}

TEST_CASE("the built-in bank runs through each category exactly once") {
    // The browser prints a heading whenever the category changes, so a bank
    // ordered as the exporter happened to emit it shows "Perc" four times.
    sl::PresetLibrary lib;
    TempDir dir("sl_lib_order");
    REQUIRE(lib.open(dir.str()));

    const sl::PresetPack* factory = packNamed(lib, "Factory");
    REQUIRE(factory != nullptr);

    std::vector<std::string> runs;
    for (const auto& p : factory->presets)
        if (runs.empty() || runs.back() != p.category) runs.push_back(p.category);

    std::vector<std::string> sorted = runs;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    // And in the order the roll-type grid uses, not alphabetical.
    REQUIRE(runs.front() == "Pad");
    REQUIRE(runs.back() == "FX");
}

TEST_CASE("the built-in bank refuses to be written to") {
    sl::PresetLibrary lib;
    TempDir dir("sl_lib_readonly");
    REQUIRE(lib.open(dir.str()));

    REQUIRE_FALSE(lib.save("Factory", makePreset("Mine", "Pad", 10u)));
    REQUIRE_FALSE(lib.error().empty());
    REQUIRE_FALSE(lib.remove("Factory", "Chime"));
    REQUIRE(packNamed(lib, "Factory")->presets.size() > 100);
}

TEST_CASE("a preset round-trips every value a sound is made of") {
    // The four performance values are the point of this test: a preset that
    // restored only the seed would load a pad at a kick's level.
    TempDir dir("sl_lib_roundtrip");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    sl::Preset p = makePreset("Deep", "Bass", 2360196101u);
    p.octave = -2;
    p.volume = 317.0;
    p.cutoff = -12.5;
    p.resonance = 6.25;
    REQUIRE(lib.save("My Presets", p));

    sl::PresetLibrary reopened;
    REQUIRE(reopened.open(dir.str()));
    const sl::Preset* back = reopened.find("My Presets", "Deep");
    REQUIRE(back != nullptr);
    REQUIRE(back->category == "Bass");
    REQUIRE(back->seed == 2360196101u);
    REQUIRE(back->octave == -2);
    REQUIRE(back->volume == 317.0);
    REQUIRE(back->cutoff == -12.5);
    REQUIRE(back->resonance == 6.25);
    REQUIRE_FALSE(back->edited);
}

TEST_CASE("an edited preset carries its instrument, an unedited one does not") {
    TempDir dir("sl_lib_edited");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    sl::Preset plain = makePreset("Plain", "Pad", 3703184240u);
    sl::Preset edited = makePreset("Edited", "Pad", 3703184240u);
    edited.instrument = sl::generateInstrument(3703184240u);
    edited.instrument.oscs[0].detune = 4.25;
    edited.edited = true;

    REQUIRE(lib.save("My Presets", plain));
    REQUIRE(lib.save("My Presets", edited));

    sl::PresetLibrary reopened;
    REQUIRE(reopened.open(dir.str()));
    REQUIRE_FALSE(reopened.find("My Presets", "Plain")->edited);

    const sl::Preset* back = reopened.find("My Presets", "Edited");
    REQUIRE(back->edited);
    REQUIRE(back->instrument.oscs[0].detune == 4.25);
}

TEST_CASE("one pack holds many presets, grouped by category") {
    TempDir dir("sl_lib_grouping");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    REQUIRE(lib.save("Live Set", makePreset("Sub", "Bass", 12u)));
    REQUIRE(lib.save("Live Set", makePreset("Air", "Pad", 20u)));
    REQUIRE(lib.save("Live Set", makePreset("Stab", "Bass", 32u)));

    // One file, not three: a pack is the unit people share.
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir.path))
        if (e.path().extension() == ".json") ++files;
    REQUIRE(files == 1);

    sl::PresetLibrary reopened;
    REQUIRE(reopened.open(dir.str()));
    const sl::PresetPack* pack = packNamed(reopened, "Live Set");
    REQUIRE(pack != nullptr);
    REQUIRE(pack->presets.size() == 3);
    // Sorted by category then name, which is the order the browser shows.
    REQUIRE(pack->presets[0].category == "Bass");
    REQUIRE(pack->presets[0].name == "Stab");
    REQUIRE(pack->presets[1].name == "Sub");
    REQUIRE(pack->presets[2].category == "Pad");
}

TEST_CASE("saving over a name replaces it rather than duplicating it") {
    TempDir dir("sl_lib_replace");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    REQUIRE(lib.save("My Presets", makePreset("One", "Pad", 10u)));
    REQUIRE(lib.save("My Presets", makePreset("One", "Lead", 21u)));

    const sl::PresetPack* pack = packNamed(lib, "My Presets");
    REQUIRE(pack->presets.size() == 1);
    REQUIRE(pack->presets[0].seed == 21u);
    REQUIRE(pack->presets[0].category == "Lead");
}

TEST_CASE("a pack that loses its last preset leaves no empty heading behind") {
    TempDir dir("sl_lib_emptied");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    REQUIRE(lib.save("Scratch", makePreset("Only", "Pad", 10u)));
    REQUIRE(packNamed(lib, "Scratch") != nullptr);

    REQUIRE(lib.remove("Scratch", "Only"));
    REQUIRE(packNamed(lib, "Scratch") == nullptr);
    REQUIRE_FALSE(fs::exists(dir.path / "Scratch.json"));
}

TEST_CASE("moving a preset between packs leaves exactly one copy") {
    TempDir dir("sl_lib_move");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    REQUIRE(lib.save("From", makePreset("Travel", "Pad", 10u)));
    REQUIRE(lib.move("From", "Travel", "To", "Lead"));

    REQUIRE(lib.find("From", "Travel") == nullptr);
    const sl::Preset* moved = lib.find("To", "Travel");
    REQUIRE(moved != nullptr);
    REQUIRE(moved->category == "Lead");
}

TEST_CASE("importing never overwrites a pack that is already there") {
    // An import is additive. Silently replacing someone's bank because two
    // packs happen to share a name is not a recoverable mistake.
    TempDir dir("sl_lib_import");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));
    REQUIRE(lib.save("Shared", makePreset("Mine", "Pad", 10u)));

    const fs::path incoming = dir.path / "incoming.pack";
    writeFile(incoming,
              R"({"format":"seedlathe-pack","version":1,"name":"Shared",)"
              R"("presets":[{"name":"Theirs","category":"Bell","seed":55}]})");

    std::string landedAs;
    REQUIRE(lib.importPack(incoming.string(), &landedAs));
    REQUIRE(landedAs != "Shared");

    REQUIRE(lib.find("Shared", "Mine") != nullptr);
    REQUIRE(lib.find(landedAs, "Theirs") != nullptr);
}

TEST_CASE("a file that is not a preset pack is refused by name") {
    TempDir dir("sl_lib_notapack");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    const fs::path junk = dir.path / "junk.pack";
    writeFile(junk, R"({"hello":"world"})");
    REQUIRE_FALSE(lib.importPack(junk.string()));
    REQUIRE_FALSE(lib.error().empty());
}

TEST_CASE("a corrupt pack hides itself instead of breaking the library") {
    TempDir dir("sl_lib_corrupt");
    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));
    REQUIRE(lib.save("Good", makePreset("Fine", "Pad", 10u)));

    writeFile(dir.path / "Broken.json", "{ this is not json");

    sl::PresetLibrary reopened;
    REQUIRE(reopened.open(dir.str()));
    REQUIRE(packNamed(reopened, "Good") != nullptr);
    REQUIRE(packNamed(reopened, "Broken") == nullptr);
    // And the built-in bank is still there, which is the real point.
    REQUIRE(packNamed(reopened, "Factory") != nullptr);
}

TEST_CASE("one malformed entry loses that preset, not the pack around it") {
    TempDir dir("sl_lib_partial");
    std::error_code ec;
    fs::create_directories(dir.path, ec);
    writeFile(dir.path / "Mixed.json",
              R"({"format":"seedlathe-pack","version":1,"name":"Mixed","presets":[)"
              R"({"name":"Keeper","category":"Pad","seed":10},)"
              R"({"category":"Pad","seed":11},)"
              R"({"name":"AlsoKeeper","category":"Bell","seed":12}]})");

    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));
    const sl::PresetPack* pack = packNamed(lib, "Mixed");
    REQUIRE(pack != nullptr);
    REQUIRE(pack->presets.size() == 2);   // the nameless one is dropped
}

TEST_CASE("presets saved in the old one-file-per-preset format are migrated") {
    // Anyone who saved before packs existed has a folder full of single
    // preset files. They must not simply stop appearing.
    TempDir dir("sl_lib_migrate");
    std::error_code ec;
    fs::create_directories(dir.path, ec);

    writeFile(dir.path / "Old One.json",
              R"({"name":"Old One","seed":1234,"octave":2})");
    writeFile(dir.path / "Old Two.json",
              R"({"name":"Old Two","seed":5678,"octave":-1})");

    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));

    const sl::PresetPack* pack = packNamed(lib, sl::kDefaultPackName);
    REQUIRE(pack != nullptr);
    REQUIRE(pack->presets.size() == 2);
    REQUIRE(lib.find(sl::kDefaultPackName, "Old One")->octave == 2);
    REQUIRE(lib.find(sl::kDefaultPackName, "Old Two")->seed == 5678u);

    // The originals are gone, so the next launch does not migrate them again
    // and end up with duplicates.
    REQUIRE_FALSE(fs::exists(dir.path / "Old One.json"));
    REQUIRE_FALSE(fs::exists(dir.path / "Old Two.json"));
}

TEST_CASE("an exported pack imports again unchanged") {
    TempDir dir("sl_lib_export");
    TempDir other("sl_lib_export_dest");
    std::error_code ec;
    fs::create_directories(other.path, ec);

    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));
    sl::Preset p = makePreset("Travelled", "Bell", 99u);
    p.volume = 250.0;
    p.cutoff = 7.0;
    REQUIRE(lib.save("Sharing", p));

    const fs::path out = other.path / "Sharing.json";
    REQUIRE(lib.exportPack("Sharing", out.string()));

    sl::PresetLibrary elsewhere;
    REQUIRE(elsewhere.open(other.str()));
    const sl::Preset* back = elsewhere.find("Sharing", "Travelled");
    REQUIRE(back != nullptr);
    REQUIRE(back->volume == 250.0);
    REQUIRE(back->cutoff == 7.0);
}

TEST_CASE("the factory bank can be exported as an editable file") {
    // Read-only in place, but nobody should have to retype 115 presets to
    // make a pack based on them.
    TempDir dir("sl_lib_exportfactory");
    std::error_code ec;
    fs::create_directories(dir.path, ec);

    sl::PresetLibrary lib;
    REQUIRE(lib.open(dir.str()));
    const fs::path out = dir.path / "elsewhere" / "Factory.json";
    fs::create_directories(out.parent_path(), ec);
    REQUIRE(lib.exportPack("Factory", out.string()));

    sl::PresetLibrary copy;
    REQUIRE(copy.open(out.parent_path().string()));
    const sl::PresetPack* pack = packNamed(copy, "Factory (2)");
    // The built-in one is always present, so the file lands beside it under
    // a disambiguated name rather than shadowing it.
    REQUIRE(pack != nullptr);
    REQUIRE(pack->presets.size() > 100);
    REQUIRE_FALSE(pack->builtIn);
}
