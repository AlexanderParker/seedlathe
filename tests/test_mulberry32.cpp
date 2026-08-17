#include <catch2/catch_test_macros.hpp>
#include "sl/Mulberry32.h"

// Expected values produced by running zyn's Z.m32 in Node 22 and printing with
// toPrecision(17). Exact equality is deliberate -- Approx would hide the
// one-ULP drift that means the port is subtly wrong.
TEST_CASE("Mulberry32 matches zyn's Z.m32 exactly") {
    struct Case { uint32_t seed; double expect[5]; };
    const Case cases[] = {
        {0u, {0.26642920868471265, 0.00032974570058286190, 0.22327202744781971,
              0.14620214793831110, 0.46732782293111086}},
        {1u, {0.62707394058816135, 0.0027357211802154779, 0.52744703995995224,
              0.98105096747167408, 0.96837789821438491}},
        {13u, {0.56632264936342835, 0.36011716164648533, 0.070590845309197903,
               0.048166851978749037, 0.036770868115127087}},
        {3703184240u, {0.23630721494555473, 0.019152297405526042, 0.30907804355956614,
                       0.40662032505497336, 0.033574980916455388}},
    };

    for (const auto& c : cases) {
        sl::Mulberry32 r(c.seed);
        for (int i = 0; i < 5; ++i) {
            INFO("seed " << c.seed << " draw " << i);
            REQUIRE(r() == c.expect[i]);
        }
    }
}

TEST_CASE("Mulberry32 scale factor multiplies the draw") {
    sl::Mulberry32 r(13u);
    r(); r();
    REQUIRE(r(30.0) == 2.1177253592759371);
}

TEST_CASE("toUint32 follows JS semantics") {
    REQUIRE(sl::toUint32(0.0) == 0u);
    REQUIRE(sl::toUint32(13.0) == 13u);
    REQUIRE(sl::toUint32(4294967295.0) == 4294967295u);
    REQUIRE(sl::toUint32(-5.0) == 4294967291u);     // wraps
    REQUIRE(sl::toUint32(4294967296.0) == 0u);      // mod 2^32
    REQUIRE(sl::toUint32(13.9) == 13u);             // truncates toward zero
    REQUIRE(sl::toUint32(-13.9) == 4294967283u);    // truncates toward zero, then wraps
}
