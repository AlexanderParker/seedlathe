#include <catch2/catch_test_macros.hpp>
#include "SharedFxRack.h"
#include "sl/Instrument.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
sl::Osc oscWithDelay(double time, double feedback) {
    sl::Osc o;
    o.del.on = true;
    o.del.time = time;
    o.del.feedback = feedback;
    return o;
}
sl::Osc oscWithVerb(double duration, double decay) {
    sl::Osc o;
    o.verb.on = true;
    o.verb.duration = duration;
    o.verb.decay = decay;
    return o;
}
} // namespace

TEST_CASE("identical delay configs share one node") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const auto a = rack.acquireRoute(oscWithDelay(0.25, 0.4));
    const auto b = rack.acquireRoute(oscWithDelay(0.25, 0.4));
    REQUIRE(a.delaySlot == b.delaySlot);
    REQUIRE(a.delaySlot >= 0);
    REQUIRE(rack.nodeCount() == 1);
}

TEST_CASE("different delay configs get different nodes") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const auto a = rack.acquireRoute(oscWithDelay(0.25, 0.4));
    const auto b = rack.acquireRoute(oscWithDelay(0.25, 0.5));
    REQUIRE(a.delaySlot != b.delaySlot);
    REQUIRE(rack.nodeCount() == 2);
}

TEST_CASE("two voices sharing a delay sum into one tail") {
    // The behavioural consequence of sharing, and the reason it must not be
    // "fixed". Both voices push within the same sample, then the graph
    // advances once -- not once per voice.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const auto route = rack.acquireRoute(oscWithDelay(0.01, 0.0));

    double l, r;
    rack.push(route, 1.0, 1.0);
    rack.push(route, 1.0, 1.0);   // second voice, same sample
    rack.mixAndAdvance(l, r);

    double peak = 0.0;
    for (int i = 0; i < 2000; ++i) {
        rack.mixAndAdvance(l, r);
        peak = std::max(peak, std::abs(l));
    }
    REQUIRE(peak > 1.5);          // one tap carrying both voices
    REQUIRE(peak < 2.5);
}

TEST_CASE("a delayed oscillator keeps its dry signal") {
    // zyn connects the panner to both the delay and a dry gain of 1.0, and
    // both feed the reverb. Losing the dry leg would make every delayed
    // oscillator sound entirely wet.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const auto route = rack.acquireRoute(oscWithDelay(0.01, 0.0));

    double l, r;
    rack.push(route, 1.0, 1.0);
    rack.mixAndAdvance(l, r);
    REQUIRE(l == 1.0);            // dry arrives immediately, undelayed
}

TEST_CASE("an oscillator with no effects passes straight to master") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::Osc plain;
    const auto route = rack.acquireRoute(plain);
    REQUIRE(route.delaySlot == -1);
    REQUIRE(route.verbSlot == -1);

    double l, r;
    rack.push(route, 0.5, -0.25);
    rack.mixAndAdvance(l, r);
    REQUIRE(l == 0.5);
    REQUIRE(r == -0.25);
}

TEST_CASE("identical reverb configs share one node") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const auto a = rack.acquireRoute(oscWithVerb(1.0, 0.7));
    const auto b = rack.acquireRoute(oscWithVerb(1.0, 0.7));
    REQUIRE(a.verbSlot == b.verbSlot);
    REQUIRE(a.verbSlot >= 0);
}

TEST_CASE("a reverb is silent until its impulse is built") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const auto route = rack.acquireRoute(oscWithVerb(0.5, 0.7));
    REQUIRE(rack.hasPending());

    double l, r;
    rack.push(route, 1.0, 1.0);
    rack.mixAndAdvance(l, r);
    REQUIRE(l == 0.0);            // no impulse yet

    rack.buildPending();          // off the audio thread
    REQUIRE_FALSE(rack.hasPending());

    double energy = 0.0;
    for (int i = 0; i < 4000; ++i) {
        rack.push(route, i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0);
        rack.mixAndAdvance(l, r);
        energy += l * l;
    }
    REQUIRE(energy > 0.0);
}

TEST_CASE("the same reverb config yields an identical tail across racks") {
    sl::SharedFxRack r1, r2;
    r1.prepare(48000.0);
    r2.prepare(48000.0);
    const auto a = r1.acquireRoute(oscWithVerb(0.4, 0.6));
    const auto b = r2.acquireRoute(oscWithVerb(0.4, 0.6));
    r1.buildPending();
    r2.buildPending();

    for (int i = 0; i < 3000; ++i) {
        const double x = (i == 0) ? 1.0 : 0.0;
        double al, ar, bl, br;
        r1.push(a, x, x);
        r2.push(b, x, x);
        r1.mixAndAdvance(al, ar);
        r2.mixAndAdvance(bl, br);
        REQUIRE(al == bl);
    }
}

TEST_CASE("passthrough placeholders count toward the eviction budget") {
    // zyn caches a no-op gain node per distinct osc config even with no delay,
    // keyed on the whole config. Those entries fill the cache and therefore
    // change WHEN eviction fires.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::Osc o;
    for (int i = 0; i < 10; ++i) {
        o.filterQ = double(i);
        rack.acquireRoute(o);
    }
    REQUIRE(rack.nodeCount() == 10);

    rack.acquireRoute(o);         // identical config, no new entry
    REQUIRE(rack.nodeCount() == 10);
}

TEST_CASE("cache evicts half once it exceeds fifty nodes") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    for (int i = 0; i < 60; ++i)
        rack.acquireRoute(oscWithDelay(0.001 * double(i), 0.1));
    REQUIRE(rack.nodeCount() == 60);

    rack.beginRender();           // zyn calls cleanupFxNodes here
    REQUIRE(rack.nodeCount() == 30);
}

TEST_CASE("eviction does not fire at or below the limit") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    for (int i = 0; i < 50; ++i)
        rack.acquireRoute(oscWithDelay(0.001 * double(i), 0.1));
    rack.beginRender();
    REQUIRE(rack.nodeCount() == 50);
}

TEST_CASE("a rack sized to the minimum still routes five distinct effects") {
    // Multitimbral parts prepare their racks with kMinNodes rather than the
    // single-mode eight, because sixteen parts at eight delay lines each is
    // over a hundred megabytes of buffer. Five is the floor an instrument can
    // actually reach: five oscillators, so five distinct delays and reverbs.
    sl::SharedFxRack rack;
    rack.prepare(48000.0, sl::SharedFxRack::kMinNodes);

    sl::Instrument inst;
    inst.oscCount = sl::kMaxOscs;
    for (int i = 0; i < sl::kMaxOscs; ++i) {
        sl::Osc& o = inst.oscs[static_cast<size_t>(i)];
        o.del.on = true;
        o.del.time = 0.05 + 0.03 * i;      // distinct, so each needs its own node
        o.del.feedback = 0.3;
        o.verb.on = true;
        o.verb.duration = 0.4 + 0.1 * i;
        o.verb.decay = 0.7;
    }
    rack.prewarm(inst);

    // Every oscillator must come back with a route of its own, not share one.
    std::vector<int> delaySlots, verbSlots;
    for (int i = 0; i < sl::kMaxOscs; ++i) {
        const auto route = rack.acquireRoute(inst.oscs[static_cast<size_t>(i)]);
        REQUIRE(route.delaySlot >= 0);
        REQUIRE(route.verbSlot >= 0);
        delaySlots.push_back(route.delaySlot);
        verbSlots.push_back(route.verbSlot);
    }
    std::sort(delaySlots.begin(), delaySlots.end());
    std::sort(verbSlots.begin(), verbSlots.end());
    REQUIRE(std::unique(delaySlots.begin(), delaySlots.end()) == delaySlots.end());
    REQUIRE(std::unique(verbSlots.begin(), verbSlots.end()) == verbSlots.end());

    // And it must still be silent until something is pushed through it.
    REQUIRE_FALSE(rack.ringing());
}
