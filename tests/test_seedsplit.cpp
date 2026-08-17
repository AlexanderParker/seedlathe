#include <catch2/catch_test_macros.hpp>
#include "SeedlatheParams.h"
#include <cmath>
#include <cstdint>

TEST_CASE("seed survives a round trip through float32 host automation") {
    const uint32_t seeds[] = {0u, 1u, 13u, 65535u, 65536u, 65537u,
                              3703184240u, 4294967295u, 2147483648u};
    for (uint32_t seed : seeds) {
        const int hi = sl::seedHi(seed);
        const int lo = sl::seedLo(seed);
        REQUIRE(hi >= 0); REQUIRE(hi <= 65535);
        REQUIRE(lo >= 0); REQUIRE(lo <= 65535);

        // What a host actually stores: normalise to [0,1] as float32, then back.
        const float nHi = static_cast<float>(double(hi) / 65535.0);
        const float nLo = static_cast<float>(double(lo) / 65535.0);
        const int rHi = static_cast<int>(std::lround(double(nHi) * 65535.0));
        const int rLo = static_cast<int>(std::lround(double(nLo) * 65535.0));

        INFO("seed " << seed << " hi " << hi << " lo " << lo);
        REQUIRE(sl::seedFrom(rHi, rLo) == seed);
    }
}

TEST_CASE("a single float32 parameter provably cannot carry a 32-bit seed") {
    // The reason the split exists. If this ever starts passing, someone has
    // changed the arithmetic and the split can be reconsidered.
    const uint32_t seed = 3703184240u;
    const float n = static_cast<float>(double(seed) / 4294967295.0);
    const auto back = static_cast<uint32_t>(std::llround(double(n) * 4294967295.0));
    INFO("seed " << seed << " round-tripped to " << back);
    REQUIRE(back != seed);
}

TEST_CASE("seed split covers the full 32-bit range") {
    // Every seed the generator accepts must be representable.
    for (uint64_t s = 0; s <= 0xFFFFFFFFull; s += 7777771ull) {
        const auto seed = static_cast<uint32_t>(s);
        INFO("seed " << seed);
        REQUIRE(sl::seedFrom(sl::seedHi(seed), sl::seedLo(seed)) == seed);
    }
    REQUIRE(sl::seedFrom(sl::seedHi(4294967295u), sl::seedLo(4294967295u)) == 4294967295u);
}

#include "sl/InstrumentGen.h"

TEST_CASE("parameter pair drives the same instrument as the raw seed") {
    // Closes the loop from host parameters to engine: whatever the two
    // parameters hold must generate exactly the instrument that seed names.
    const uint32_t seed = 3703184240u;   // the demo page's "Forest"
    REQUIRE(sl::seedHi(seed) == 56506);
    REQUIRE(sl::seedLo(seed) == 7024);

    const auto viaParams = sl::generateInstrument(
        sl::seedFrom(sl::seedHi(seed), sl::seedLo(seed)));
    const auto direct = sl::generateInstrument(seed);

    REQUIRE(viaParams.typeIndex == direct.typeIndex);
    REQUIRE(viaParams.oscCount == direct.oscCount);
    for (int i = 0; i < direct.oscCount; ++i) {
        INFO("osc " << i);
        REQUIRE(viaParams.oscs[static_cast<size_t>(i)].waveform ==
                direct.oscs[static_cast<size_t>(i)].waveform);
        REQUIRE(viaParams.oscs[static_cast<size_t>(i)].adsrGain.aT ==
                direct.oscs[static_cast<size_t>(i)].adsrGain.aT);
        REQUIRE(viaParams.oscs[static_cast<size_t>(i)].filterQ ==
                direct.oscs[static_cast<size_t>(i)].filterQ);
    }
}
