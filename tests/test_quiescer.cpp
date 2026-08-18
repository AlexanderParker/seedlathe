#include <catch2/catch_test_macros.hpp>

#include "Quiescer.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

TEST_CASE("an idle quiescer runs the work immediately") {
    sl::Quiescer q;
    bool ran = false;
    REQUIRE(q.withQuiescence([&ran] { ran = true; }));
    REQUIRE(ran);
    REQUIRE_FALSE(q.blocked());
}

TEST_CASE("work never overlaps a block in flight") {
    // The property the whole thing exists for. The audio thread marks itself
    // present for the duration of a block; the work asserts nobody is present,
    // before and after a pause long enough for a sloppy handshake to be caught.
    //
    // What this cannot check, having been tried: reversing enter() so the flag
    // is read BEFORE the counter is claimed still passes, three runs out of
    // three. That window is a few instructions wide and the control thread has
    // to raise the flag inside it and then find the counter even. The ordering
    // rests on the argument in Quiescer.h, not on this test.
    sl::Quiescer q;
    std::atomic<bool> inside{false};
    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::atomic<int> overlaps{0};
    std::atomic<int> bailed{0};

    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (q.enter()) {
                inside.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                inside.store(false, std::memory_order_release);
                blocks.fetch_add(1, std::memory_order_relaxed);
            } else {
                bailed.fetch_add(1, std::memory_order_relaxed);
            }
            q.leave();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    int ran = 0;
    for (int i = 0; i < 60; ++i) {
        const bool ok = q.withQuiescence([&] {
            if (inside.load(std::memory_order_acquire))
                overlaps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            if (inside.load(std::memory_order_acquire))
                overlaps.fetch_add(1, std::memory_order_relaxed);
        });
        if (ok) ++ran;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop.store(true, std::memory_order_release);
    audio.join();

    INFO("blocks " << blocks.load() << ", bailed " << bailed.load()
         << ", reconfigures " << ran);
    REQUIRE(overlaps.load() == 0);
    REQUIRE(ran > 0);              // it is not simply refusing everything
    REQUIRE(blocks.load() > 0);    // the audio thread really was running
    REQUIRE(bailed.load() > 0);    // and really did bail while blocked
}

TEST_CASE("a wedged audio thread makes the quiescer refuse rather than proceed") {
    // Some hosts suspend the audio thread. One suspended mid-block resumes in
    // the middle of whatever the control thread freed, so a timeout has to mean
    // "not now" and never "go ahead anyway".
    sl::Quiescer q;
    REQUIRE(q.enter());            // a block that is never left

    bool ran = false;
    REQUIRE_FALSE(q.withQuiescence([&ran] { ran = true; }, 20));
    REQUIRE_FALSE(ran);
    // And it must not stay blocked, or every later block would be silent.
    REQUIRE_FALSE(q.blocked());

    q.leave();
    REQUIRE(q.withQuiescence([&ran] { ran = true; }, 20));
    REQUIRE(ran);
}

TEST_CASE("a block starting while blocked is turned away") {
    sl::Quiescer q;
    std::atomic<bool> allowed{true};

    std::thread control([&] {
        q.withQuiescence([&] {
            allowed.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            allowed.store(true, std::memory_order_release);
        });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    int refusals = 0;
    for (int i = 0; i < 40; ++i) {
        if (!q.enter()) {
            ++refusals;
            // Anything refused must have been refused for a reason.
            REQUIRE_FALSE(allowed.load(std::memory_order_acquire));
        }
        q.leave();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    control.join();

    INFO("refusals while reconfiguring: " << refusals);
    REQUIRE(refusals > 0);
}

TEST_CASE("the sequence counter is odd exactly while a block is in flight") {
    // The parity IS the handshake: an odd count is what tells the control
    // thread to keep waiting. Claiming the block after reading the flag instead
    // of before would leave this even inside a block, and the whole thing would
    // silently stop working.
    sl::Quiescer q;
    REQUIRE((q.sequence() & 1u) == 0u);
    q.enter();
    REQUIRE((q.sequence() & 1u) == 1u);
    q.leave();
    REQUIRE((q.sequence() & 1u) == 0u);

    // Including on the path where the block is turned away: that one still
    // claimed and released, or the counter would drift out of step forever.
    q.withQuiescence([&q] {
        REQUIRE((q.sequence() & 1u) == 0u);
    });
    REQUIRE((q.sequence() & 1u) == 0u);
}
