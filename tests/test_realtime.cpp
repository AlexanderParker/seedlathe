#include <catch2/catch_test_macros.hpp>
#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/InstrumentGen.h"
#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

// Counts heap traffic while armed. This is the direct check for the rule that
// matters most in a plugin: ProcessBlock must not allocate. A sanitiser can
// find leaks and overruns but will happily let a std::vector grow inside the
// audio callback.
namespace {
std::atomic<int> g_allocs{0};
std::atomic<bool> g_armed{false};
} // namespace

void* operator new(size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new[](size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

TEST_CASE("rendering does not allocate once prepared") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 32, &rack);

    const auto inst = sl::generateInstrument(3703184240u);
    rack.prewarm(inst);            // message-thread work, allocation allowed here

    std::vector<float> l(512), r(512);
    pool.render(l.data(), r.data(), 512);   // let any first-call caches settle
    rack.prewarm(inst);

    g_allocs.store(0);
    g_armed.store(true);
    for (int block = 0; block < 200; ++block) {
        if (block % 20 == 0) pool.noteOn(inst, block % 12, 1.0, true);
        if (block % 20 == 10) pool.noteOff(block % 12);
        pool.render(l.data(), r.data(), 512);
    }
    g_armed.store(false);

    INFO("allocations during render: " << g_allocs.load());
    REQUIRE(g_allocs.load() == 0);
}

TEST_CASE("generating an instrument does not allocate") {
    // generateInstrument runs on the audio thread when the seed parameter
    // changes, so the Instrument tree must stay a fixed-size POD. A std::vector
    // creeping into it would show up here.
    sl::generateInstrument(1u);    // warm any static tables

    g_allocs.store(0);
    g_armed.store(true);
    volatile int sink = 0;
    for (uint32_t s = 0; s < 500; ++s) sink += sl::generateInstrument(s).oscCount;
    g_armed.store(false);
    (void)sink;

    INFO("allocations during generation: " << g_allocs.load());
    REQUIRE(g_allocs.load() == 0);
}

TEST_CASE("voice stealing does not allocate") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 4, &rack);

    const auto inst = sl::generateInstrument(13u);
    rack.prewarm(inst);
    std::vector<float> l(256), r(256);
    pool.render(l.data(), r.data(), 256);

    g_allocs.store(0);
    g_armed.store(true);
    for (int i = 0; i < 200; ++i) {
        pool.noteOn(inst, i % 24, 1.0, true);   // far more notes than voices
        pool.render(l.data(), r.data(), 256);
    }
    g_armed.store(false);

    INFO("allocations during stealing: " << g_allocs.load());
    REQUIRE(g_allocs.load() == 0);
}
