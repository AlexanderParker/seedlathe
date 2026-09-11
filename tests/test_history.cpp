#include <catch2/catch_test_macros.hpp>

#include "SeedlathePart.h"
#include "sl/InstrumentGen.h"

#include <chrono>

// The Back button's memory. The plugin side of it -- which paths record and
// what Back restores -- needs a host to exercise; what is testable here is the
// part that decides whether an entry is worth keeping, which is where an undo
// stack usually goes wrong: it either records nothing useful or records the
// same thing a hundred times.

namespace {

seedlathe::Snapshot snap(uint32_t seed, bool edited = false, int octave = 0) {
    seedlathe::Snapshot s;
    s.seed = seed;
    s.inst = sl::generateInstrument(seed);
    s.edited = edited;
    s.octave = octave;
    return s;
}

// A clock the test drives, so entries can be placed inside or outside the
// coalescing window without waiting for a real half-second.
struct Clock {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point after(int ms) {
        t += std::chrono::milliseconds(ms);
        return t;
    }
    // Comfortably past the window, which is what a deliberate second action
    // looks like.
    std::chrono::steady_clock::time_point next() { return after(1000); }
};

} // namespace

TEST_CASE("history keeps each sound visited, oldest first") {
    seedlathe::Part part;
    Clock c;
    part.pushHistory(snap(10u), c.next());
    part.pushHistory(snap(20u), c.next());
    part.pushHistory(snap(30u), c.next());

    REQUIRE(part.history.size() == 3);
    REQUIRE(part.history.front().seed == 10u);
    REQUIRE(part.history.back().seed == 30u);
}

TEST_CASE("history collapses a sound replaced by the same sound") {
    // Rolling onto the seed you are already on, or loading the preset that is
    // already loaded, must not consume an undo step -- otherwise Back appears
    // to do nothing and the user presses it again.
    seedlathe::Part part;
    Clock c;
    part.pushHistory(snap(10u), c.next());
    part.pushHistory(snap(10u), c.next());
    part.pushHistory(snap(10u), c.next());
    REQUIRE(part.history.size() == 1);

    // A different octave on the same seed is a different sound.
    part.pushHistory(snap(10u, false, 2), c.next());
    REQUIRE(part.history.size() == 2);
}

TEST_CASE("history never collapses edited patches, whose seeds say nothing") {
    // Two edits of one seed are two different instruments. Collapsing them on
    // the seed alone would silently lose one.
    seedlathe::Part part;
    Clock c;
    seedlathe::Snapshot a = snap(10u, true);
    seedlathe::Snapshot b = a;
    b.inst.oscs[0].detune = 3.0;

    part.pushHistory(a, c.next());
    part.pushHistory(b, c.next());
    REQUIRE(part.history.size() == 2);
    REQUIRE(part.history[0].inst.oscs[0].detune != part.history[1].inst.oscs[0].detune);
}

TEST_CASE("a burst of replacements records only where it started") {
    // Dragging the seed box emits a seed per mouse move. Without coalescing,
    // one gesture fills the whole history and Back has to be pressed a
    // hundred times to undo it.
    seedlathe::Part part;
    Clock c;
    part.pushHistory(snap(500u), c.next());       // the sound before the drag

    for (uint32_t i = 0; i < 200u; ++i)
        part.pushHistory(snap(1000u + i), c.after(8));   // ~8 ms apart

    REQUIRE(part.history.size() == 1);
    REQUIRE(part.history.back().seed == 500u);

    // And the gesture after it records normally.
    part.pushHistory(snap(7u), c.next());
    REQUIRE(part.history.size() == 2);
    REQUIRE(part.history.back().seed == 7u);
}

TEST_CASE("history is bounded and drops the oldest entry first") {
    seedlathe::Part part;
    Clock c;
    const size_t cap = seedlathe::Part::kMaxHistory;
    for (uint32_t i = 0; i < cap + 10u; ++i)
        part.pushHistory(snap(1000u + i), c.next());

    REQUIRE(part.history.size() == cap);
    // The last `cap` seeds pushed, so the oldest survivor is the eleventh.
    REQUIRE(part.history.front().seed == 1010u);
    REQUIRE(part.history.back().seed == 1000u + static_cast<uint32_t>(cap) + 9u);
}

TEST_CASE("each part remembers its own history") {
    // Going back should undo what happened to the instrument on screen, not
    // walk backwards through sixteen parts interleaved.
    std::array<seedlathe::Part, 2> parts;
    Clock c;
    parts[0].pushHistory(snap(10u), c.next());
    parts[0].pushHistory(snap(20u), c.next());
    parts[1].pushHistory(snap(99u), c.next());

    REQUIRE(parts[0].history.size() == 2);
    REQUIRE(parts[1].history.size() == 1);
    REQUIRE(parts[1].history.back().seed == 99u);
}
