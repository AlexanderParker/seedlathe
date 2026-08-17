#include <catch2/catch_test_macros.hpp>
#include "OfflineRender.h"
#include "sl/InstrumentGen.h"
#include <algorithm>
#include <cmath>

TEST_CASE("offline render returns the requested length of finite audio") {
    const auto out = sl::renderOffline(sl::generateInstrument(13), 0, 1.0, 2.0, 48000.0);
    REQUIRE(out.left.size() == 96000u);
    REQUIRE(out.right.size() == 96000u);

    double peak = 0.0;
    for (float s : out.left) {
        REQUIRE(std::isfinite(s));
        peak = std::max(peak, std::abs(double(s)));
    }
    REQUIRE(peak > 1e-4);
    REQUIRE(peak <= 1.5);
}

TEST_CASE("offline render is deterministic") {
    // Without this the fidelity test cannot have a stable threshold at all.
    const auto a = sl::renderOffline(sl::generateInstrument(3703184240u), 0, 1.0, 1.0, 48000.0);
    const auto b = sl::renderOffline(sl::generateInstrument(3703184240u), 0, 1.0, 1.0, 48000.0);
    REQUIRE(a.left == b.left);
    REQUIRE(a.right == b.right);
}

TEST_CASE("offline render of every instrument type is finite and audible") {
    for (uint32_t type = 0; type < 10; ++type) {
        const uint32_t seed = 1230 + type;
        const auto out = sl::renderOffline(sl::generateInstrument(seed), 0, 1.0, 1.5, 48000.0);
        double peak = 0.0;
        for (float s : out.left) {
            INFO("seed " << seed);
            REQUIRE(std::isfinite(s));
            peak = std::max(peak, std::abs(double(s)));
        }
        INFO("seed " << seed << " type " << type << " peak " << peak);
        REQUIRE(peak > 1e-6);
    }
}
