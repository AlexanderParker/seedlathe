#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

// core/ must stay framework-free for the life of the project: the search and
// fidelity tests run headless in CI with no plugin host, and if iPlug2 ever
// disappoints, only plugin/ should be thrown away.
TEST_CASE("core never includes a plugin framework header") {
    namespace fs = std::filesystem;
    const fs::path coreDir = fs::path(SL_SOURCE_DIR) / "core";
    const char* banned[] = {"IPlug", "IGraphics", "public.sdk", "pluginterfaces", "clap/"};

    bool anyChecked = false;
    for (const auto& entry : fs::recursive_directory_iterator(coreDir)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".h" && ext != ".cpp") continue;
        anyChecked = true;

        std::ifstream in(entry.path());
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (line.find("#include") == std::string::npos) continue;
            for (const char* b : banned) {
                INFO(entry.path().string() << ":" << lineNo << " -> " << line);
                REQUIRE(line.find(b) == std::string::npos);
            }
        }
    }
    // Without this, a wrong path would make the whole test pass vacuously.
    REQUIRE(anyChecked);
}
