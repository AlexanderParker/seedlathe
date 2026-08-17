#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "webaudio/WaParam.h"
#include <cmath>

using Catch::Matchers::WithinAbs;

TEST_CASE("WaParam holds its value with no events") {
    sl::WaParam p;
    p.reset(0.5);
    REQUIRE(p.valueAt(0.0) == 0.5);
    REQUIRE(p.valueAt(99.0) == 0.5);
}

TEST_CASE("WaParam linear ramp interpolates from the previous event") {
    sl::WaParam p;
    p.reset(0.0);
    p.setValueAtTime(0.0, 1.0);
    p.linearRampToValueAtTime(1.0, 2.0);
    REQUIRE_THAT(p.valueAt(1.0),  WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(p.valueAt(1.25), WithinAbs(0.25, 1e-12));
    REQUIRE_THAT(p.valueAt(1.5),  WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(p.valueAt(2.0),  WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(p.valueAt(9.0),  WithinAbs(1.0, 1e-12));   // holds after the last event
}

TEST_CASE("WaParam reproduces a zyn ADSR chain") {
    // Z.adsr: setValueAtTime(0, t) then four linear ramps, with the attack time
    // clamped to a 5 ms minimum to avoid a click.
    sl::WaParam p;
    p.reset(0.0);
    const double t = 0.0, max = 1.0;
    p.setValueAtTime(0.0, t);
    p.linearRampToValueAtTime(1.0 * max, t + 0.05);
    p.linearRampToValueAtTime(0.5 * max, t + 0.05 + 0.10);
    p.linearRampToValueAtTime(0.3 * max, t + 0.05 + 0.10 + 0.20);
    p.linearRampToValueAtTime(0.0,       t + 0.05 + 0.10 + 0.20 + 0.10);

    REQUIRE_THAT(p.valueAt(0.025), WithinAbs(0.5, 1e-12));
    REQUIRE_THAT(p.valueAt(0.05),  WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(p.valueAt(0.10),  WithinAbs(0.75, 1e-12));
    REQUIRE_THAT(p.valueAt(0.45),  WithinAbs(0.0, 1e-12));
}

TEST_CASE("WaParam cancelScheduledValues drops later events") {
    sl::WaParam p;
    p.reset(0.0);
    p.setValueAtTime(0.0, 0.0);
    p.linearRampToValueAtTime(1.0, 1.0);
    p.cancelScheduledValues(0.5);
    REQUIRE_THAT(p.valueAt(0.9), WithinAbs(0.0, 1e-12));
}

TEST_CASE("WaParam is order-independent despite the sequential cursor") {
    // The cursor optimisation assumes monotonically advancing time. Querying
    // backwards must still be correct, or noteOff -- which reads the current
    // value after the render loop has advanced -- returns nonsense.
    sl::WaParam p;
    p.reset(0.0);
    p.setValueAtTime(0.0, 0.0);
    p.linearRampToValueAtTime(1.0, 1.0);

    REQUIRE_THAT(p.valueAt(0.9), WithinAbs(0.9, 1e-12));
    REQUIRE_THAT(p.valueAt(0.1), WithinAbs(0.1, 1e-12));   // backwards
    REQUIRE_THAT(p.valueAt(0.5), WithinAbs(0.5, 1e-12));
}

TEST_CASE("WaParam ignores events beyond capacity rather than overflowing") {
    sl::WaParam p;
    p.reset(0.0);
    for (int i = 0; i < 40; ++i) p.linearRampToValueAtTime(1.0, double(i));
    REQUIRE(std::isfinite(p.valueAt(100.0)));
}
