#include <catch2/catch_test_macros.hpp>
#include "RackPool.h"
#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/InstrumentGen.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

// Regression test for the crash reported when dragging the seed control.
//
// Rebuilding a rack frees and reallocates every buffer inside it. Doing that
// from the message thread while the audio thread walks the same buffers is a
// use-after-free, and before RackPool existed this segfaulted within a few
// hundred rebuilds, every run.
//
// This drives the same RackPool the plugin uses, from two threads, exactly as
// OnParamChange and ProcessBlock do.
TEST_CASE("rebuilding the instrument while rendering is safe") {
    sl::RackPool racks;
    racks.prepare(48000.0, 4);

    sl::VoicePool pool;
    pool.prepare(48000.0, 32);

    std::atomic<bool> stop{false};
    std::atomic<int> rebuilt{0};
    std::atomic<int> refused{0};

    // Message thread, paced like a real drag rather than a spin loop: a mouse
    // produces a few hundred parameter changes a second, not millions.
    std::thread ui([&] {
        uint32_t s = 1000;
        while (!stop.load()) {
            if (racks.rebuild(sl::generateInstrument(s++), pool)) rebuilt.fetch_add(1);
            else refused.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // Audio thread. 256 frames at 48 kHz is 5.3 ms of audio per block.
    std::vector<float> l(256), r(256);
    const auto held = sl::generateInstrument(1000u);
    for (int b = 0; b < 6000; ++b) {
        racks.audioBlockStarted(pool);
        if (b % 30 == 0) pool.noteOn(racks.liveRack(), held, b % 12, 1.0, true);
        if (b % 30 == 15) pool.noteOff(b % 12);
        pool.render(l.data(), r.data(), 256);
        for (int i = 0; i < 256; ++i) REQUIRE(std::isfinite(l[i]));
    }

    stop.store(true);
    ui.join();

    // A fix that simply never rebuilds would also never crash, so the test
    // insists real work got through.
    INFO("rebuilt " << rebuilt.load() << ", refused " << refused.load());
    REQUIRE(rebuilt.load() > 50);
}

TEST_CASE("a rack is never rebuilt while a voice is bound to it") {
    sl::RackPool racks;
    racks.prepare(48000.0, 4);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    const auto inst = sl::generateInstrument(500u);
    sl::SharedFxRack* first = racks.liveRack();
    pool.noteOn(first, inst, 0, 1.0, true);
    REQUIRE(pool.rackInUse(first));

    std::vector<float> l(64), r(64);
    for (uint32_t s = 501; s < 520; ++s) {
        racks.audioBlockStarted(pool);
        pool.render(l.data(), r.data(), 64);
        racks.rebuild(sl::generateInstrument(s), pool);
        // Whatever it chose, it must not have been the rack holding the note.
        REQUIRE(pool.rackInUse(first));
    }
}

// Regression test for the crash reported after loading a few presets.
//
// The reverb-tail fix made VoicePool mix every RINGING rack, not just the ones
// sounding voices reference. RackPool did not know that: it gated rebuilds on
// voice ownership alone, so a rack whose last voice had ended -- but whose tail
// the audio thread was still mixing -- looked free. Rebuilding it reallocated
// the buffers under the mix, and the plugin died with an access violation after
// a handful of preset loads.
//
// The two earlier tests in this file miss it because they call render() without
// the all-racks argument, which is exactly the path that does not mix tails.
TEST_CASE("a rack is never rebuilt while its tail is still being mixed") {
    // Two racks makes the choice forced: after the first rebuild there is
    // exactly one candidate, and the test can say which one it must be.
    sl::RackPool racks;
    racks.prepare(48000.0, 2);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    // 2.44 s reverb plus a delay.
    const auto reverby = sl::generateInstrument(3703184240u);
    REQUIRE(racks.rebuild(reverby, pool));
    sl::SharedFxRack* sounded = racks.liveRack();

    std::vector<float> l(256), r(256);
    auto renderFor = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            racks.audioBlockStarted(pool);
            pool.render(l.data(), r.data(), 256, racks.all(), racks.allCount());
        }
    };

    pool.noteOn(sounded, reverby, 0, 1.0, true);
    renderFor(100);
    pool.noteOff(0);
    renderFor(200);

    REQUIRE(pool.activeCount() == 0);        // no voice owns it any more
    REQUIRE_FALSE(pool.rackInUse(sounded));
    REQUIRE(sounded->ringing());             // but the tail is still mixed

    // Move live off the ringing rack onto the other one.
    REQUIRE(racks.rebuild(sl::generateInstrument(1001u), pool));
    REQUIRE(racks.liveRack() != sounded);
    renderFor(4);

    // The ringing rack is now the only candidate, and it must be refused.
    REQUIRE_FALSE(racks.rebuild(sl::generateInstrument(1002u), pool));
    REQUIRE(racks.liveRack() != sounded);

    // Refusing forever would be its own bug: the refusal asks the audio thread
    // to retire the oldest non-live rack, which silences the tail and frees it.
    bool eventually = false;
    for (int attempt = 0; attempt < 20 && !eventually; ++attempt) {
        renderFor(4);
        eventually = racks.rebuild(sl::generateInstrument(1003u), pool);
    }
    REQUIRE(eventually);
}

TEST_CASE("a reverb tail keeps sounding after the note is released") {
    // Regression: VoicePool used to mix only racks that had ACTIVE voices, so
    // the instant a voice ended its reverb and delay tails were cut dead.
    // Releasing a key chopped seconds of tail off in the plugin, while the
    // offline renderer -- which mixes unconditionally -- never showed it.
    sl::RackPool racks;
    racks.prepare(48000.0, 4);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    // 1 oscillator, a 2.44 s reverb and a delay: the seed reported as having
    // "no decay" on release.
    const auto inst = sl::generateInstrument(3703184240u);
    racks.rebuild(inst, pool);

    std::vector<float> l(256), r(256);
    auto renderFor = [&](int blocks) {
        double peak = 0.0;
        for (int b = 0; b < blocks; ++b) {
            racks.audioBlockStarted(pool);
            pool.render(l.data(), r.data(), 256, racks.all(), racks.allCount());
            for (int i = 0; i < 256; ++i) peak = std::max(peak, std::abs(double(l[i])));
        }
        return peak;
    };

    pool.noteOn(racks.liveRack(), inst, 0, 1.0, true);
    renderFor(200);                       // ~1.1 s of held note
    pool.noteOff(0);

    // The amplitude envelope's release is 410 ms; give it a second to finish.
    renderFor(190);
    REQUIRE(pool.activeCount() == 0);     // the voice itself is done

    // The reverb tail must still be audible well beyond that.
    const double tail = renderFor(90);    // roughly another half second
    INFO("tail peak after the voice ended: " << tail);
    REQUIRE(tail > 1e-5);
}
