// Performance probe. Tagged [.perf] so it only runs when asked for by name.
// Reports the realtime factor of the render path: how many seconds of audio
// per second of CPU. Anything under about 20x for a single voice will not
// survive a real host, where several voices, a UI and the rest of the session
// share the core.
#include <catch2/catch_test_macros.hpp>
#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/InstrumentGen.h"
#include "webaudio/WaBiquad.h"
#include "webaudio/WaNodes.h"
#include "webaudio/WaOscillator.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdio>
#include <vector>

namespace {

double secondsFor(const std::function<void()>& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

} // namespace

TEST_CASE("render path realtime factor", "[.perf]") {
    const double sr = 48000.0;
    const int frames = static_cast<int>(sr * 2.0);
    std::vector<float> l(512), r(512);

    struct Case { const char* name; uint32_t seed; int voices; };
    const Case cases[] = {
        {"seed 13 (2 osc, verb)      1 voice ", 13u, 1},
        {"seed 13                    8 voices", 13u, 8},
        {"seed 3703184240 (1 osc)    1 voice ", 3703184240u, 1},
        {"seed 3703184240            8 voices", 3703184240u, 8},
    };

    for (const auto& c : cases) {
        sl::SharedFxRack rack;
        rack.prepare(sr);
        sl::VoicePool pool;
        pool.prepare(sr, 32);
        const auto inst = sl::generateInstrument(c.seed);
        rack.prewarm(inst);
        for (int v = 0; v < c.voices; ++v) pool.noteOn(&rack, inst, v * 3, 1.0, true);

        const double cpu = secondsFor([&] {
            for (int i = 0; i < frames; i += 512)
                pool.render(l.data(), r.data(), 512);
        });
        std::printf("  %s  %6.2f x realtime\n", c.name, 2.0 / cpu);
    }
    REQUIRE(true);
}

TEST_CASE("component cost breakdown", "[.perf]") {
    const double sr = 48000.0;
    const int n = static_cast<int>(sr);   // one second

    // Biquad with per-sample coefficient recomputation, as the voice runs it.
    {
        sl::WaBiquad f;
        f.prepare(sr);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i) {
                f.setCoefficients(sl::FilterType::Lowpass,
                                  1000.0 + double(i % 1000), 10.0, 0.0);
                sink += f.process(0.5);
            }
        });
        std::printf("  biquad  (recompute every sample) %6.2f x realtime\n", 1.0 / cpu);
    }

    // Biquad with fixed coefficients, to isolate the recompute cost.
    {
        sl::WaBiquad f;
        f.prepare(sr);
        f.setCoefficients(sl::FilterType::Lowpass, 1000.0, 10.0, 0.0);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i) sink += f.process(0.5);
        });
        std::printf("  biquad  (fixed coefficients)     %6.2f x realtime\n", 1.0 / cpu);
    }

    // Oscillator.
    {
        sl::WaOscillator o;
        o.prepare(sr);
        o.setType(sl::Waveform::Sawtooth);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i) sink += o.render(220.0);
        });
        std::printf("  oscillator                       %6.2f x realtime\n", 1.0 / cpu);
    }

    // Convolver, at the reverb durations the generator actually produces.
    for (double dur : {0.5, 1.5, 3.0}) {
        sl::WaConvolver c;
        c.prepare(sr);
        c.buildImpulse(dur, 0.75, 1234u);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i) {
                double a, b;
                c.process(0.1, 0.1, a, b);
                sink += a;
            }
        });
        std::printf("  convolver %.1fs tail              %6.2f x realtime\n", dur, 1.0 / cpu);
    }
    REQUIRE(true);
}

TEST_CASE("where the time actually goes", "[.perf]") {
    const double sr = 48000.0;
    const int frames = static_cast<int>(sr * 2.0);
    std::vector<float> l(512), r(512);

    auto run = [&](const char* label, sl::Instrument inst, int voices) {
        sl::SharedFxRack rack;
        rack.prepare(sr);
        sl::VoicePool pool;
        pool.prepare(sr, 32);
        rack.prewarm(inst);
        for (int v = 0; v < voices; ++v) pool.noteOn(&rack, inst, v * 3, 1.0, true);
        const double cpu = secondsFor([&] {
            for (int i = 0; i < frames; i += 512) pool.render(l.data(), r.data(), 512);
        });
        std::printf("  %-34s %7.1f x realtime\n", label, 2.0 / cpu);
    };

    const auto base = sl::generateInstrument(13u);

    run("seed 13 full                   1v", base, 1);

    { auto i = base; for (int k = 0; k < i.oscCount; ++k) i.oscs[k].verb.on = false;
      run("seed 13 no reverb             1v", i, 1); }

    { auto i = base; for (int k = 0; k < i.oscCount; ++k) i.oscs[k].fm.on = false;
      run("seed 13 no FM                 1v", i, 1); }

    { auto i = base; for (int k = 0; k < i.oscCount; ++k) i.oscs[k].gLfo.on = false;
      run("seed 13 no gLFO               1v", i, 1); }

    { auto i = base;
      for (int k = 0; k < i.oscCount; ++k) {
        i.oscs[k].verb.on = false; i.oscs[k].fm.on = false; i.oscs[k].gLfo.on = false;
      }
      run("seed 13 osc+filter only       1v", i, 1);
      run("seed 13 osc+filter only       8v", i, 8); }

    { auto i = base; for (int k = 0; k < i.oscCount; ++k) i.oscs[k].verb.on = false;
      run("seed 13 no reverb             8v", i, 8); }

    REQUIRE(true);
}

#include <xmmintrin.h>
#include <pmmintrin.h>

TEST_CASE("denormal cost in decaying tails", "[.perf]") {
    // A reverb tail decays toward zero. Once samples reach ~1e-38 the CPU
    // starts handling denormals in microcode, which can cost 100x per
    // operation. It presents exactly as this bug does: fine at first, then
    // progressively worse the longer notes ring.
    const double sr = 48000.0;

    auto measure = [&](bool ftz) {
        if (ftz) {
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
            _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
        } else {
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
            _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
        }

        sl::SharedFxRack rack;
        rack.prepare(sr);
        sl::VoicePool pool;
        pool.prepare(sr, 32);
        const auto inst = sl::generateInstrument(13u);
        rack.prewarm(inst);
        pool.noteOn(&rack, inst, 0, 1.0, false);

        std::vector<float> l(512), r(512);
        // Let the note finish so everything is deep in its decay.
        for (int i = 0; i < 48000 * 4; i += 512) pool.render(l.data(), r.data(), 512);

        return secondsFor([&] {
            for (int i = 0; i < 48000 * 4; i += 512) pool.render(l.data(), r.data(), 512);
        });
    };

    const double without = measure(false);
    const double with = measure(true);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);

    std::printf("  tail render, denormals ON  %6.1f x realtime\n", 4.0 / without);
    std::printf("  tail render, flush-to-zero %6.1f x realtime\n", 4.0 / with);
    std::printf("  speedup from FTZ/DAZ       %6.1f x\n", without / with);
    REQUIRE(true);
}

#include "webaudio/WaCompressor.h"
#include "OfflineRender.h"

TEST_CASE("compressor and full plugin chain", "[.perf]") {
    const double sr = 48000.0;
    const int n = static_cast<int>(sr);

    {
        sl::WaCompressor c;
        c.prepare(sr);
        c.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i) {
                double l, r;
                c.process(0.3 * std::sin(i * 0.01), 0.3 * std::sin(i * 0.01), l, r);
                sink += l;
            }
        });
        std::printf("  compressor alone                 %7.1f x realtime\n", 1.0 / cpu);
    }

    // What ProcessBlock actually runs: voices, then the compressor.
    {
        sl::SharedFxRack rack;
        rack.prepare(sr);
        sl::VoicePool pool;
        pool.prepare(sr, 32);
        sl::WaCompressor comp;
        comp.prepare(sr);
        comp.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);
        const auto inst = sl::generateInstrument(13u);
        rack.prewarm(inst);
        for (int v = 0; v < 8; ++v) pool.noteOn(&rack, inst, v * 3, 1.0, true);

        std::vector<float> l(512), r(512);
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n * 2; i += 512) {
                pool.render(l.data(), r.data(), 512);
                for (int s = 0; s < 512; ++s) {
                    double a, b;
                    comp.process(l[s], r[s], a, b);
                }
            }
        });
        std::printf("  full plugin chain, 8 voices      %7.1f x realtime\n", 2.0 / cpu);
    }
    REQUIRE(true);
}

TEST_CASE("worst-case instruments", "[.perf]") {
    const double sr = 48000.0;
    std::vector<float> l(512), r(512);

    // Find the heaviest instruments the generator can produce: many
    // oscillators, each with its own long reverb.
    struct Cand { uint32_t seed; int oscs; int verbs; double totalVerb; };
    std::vector<Cand> cands;
    for (uint32_t s = 0; s < 20000; ++s) {
        const auto inst = sl::generateInstrument(s);
        Cand c{s, inst.oscCount, 0, 0.0};
        for (int i = 0; i < inst.oscCount; ++i) {
            const auto& o = inst.oscs[static_cast<size_t>(i)];
            if (o.verb.on) { c.verbs++; c.totalVerb += o.verb.duration; }
        }
        cands.push_back(c);
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.totalVerb > b.totalVerb; });

    for (int k = 0; k < 3; ++k) {
        const auto& c = cands[static_cast<size_t>(k)];
        sl::SharedFxRack rack;
        rack.prepare(sr);
        sl::VoicePool pool;
        pool.prepare(sr, 32);
        sl::WaCompressor comp;
        comp.prepare(sr);
        comp.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);
        const auto inst = sl::generateInstrument(c.seed);
        rack.prewarm(inst);
        for (int v = 0; v < 8; ++v) pool.noteOn(&rack, inst, v * 3, 1.0, true);

        const double cpu = secondsFor([&] {
            for (int i = 0; i < 96000; i += 512) {
                pool.render(l.data(), r.data(), 512);
                for (int s2 = 0; s2 < 512; ++s2) { double a, b; comp.process(l[s2], r[s2], a, b); }
            }
        });
        std::printf("  seed %-11u %d osc, %d reverbs totalling %.1fs, 8 voices: %6.2f x realtime\n",
                    c.seed, c.oscs, c.verbs, c.totalVerb, 2.0 / cpu);
    }
    REQUIRE(true);
}

#include "webaudio/WaParam.h"

TEST_CASE("voice internals breakdown", "[.perf]") {
    const double sr = 48000.0;
    const int n = static_cast<int>(sr);

    {
        sl::WaParam p;
        p.reset(0.0);
        p.setValueAtTime(0.0, 0.0);
        p.linearRampToValueAtTime(1.0, 0.05);
        p.linearRampToValueAtTime(0.6, 0.2);
        p.linearRampToValueAtTime(0.3, 0.7);
        p.linearRampToValueAtTime(0.0, 1.0);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i) sink += p.valueAt(double(i) / sr);
        });
        std::printf("  WaParam::valueAt                 %8.1f x realtime\n", 1.0 / cpu);
    }

    // A voice reads three or four envelopes per oscillator per sample, so the
    // per-call cost is multiplied by roughly 4 * oscillators * voices.
    {
        sl::WaParam p;
        p.reset(0.0);
        p.setValueAtTime(0.0, 0.0);
        p.linearRampToValueAtTime(1.0, 0.05);
        p.linearRampToValueAtTime(0.6, 0.2);
        volatile double sink = 0.0;
        const double cpu = secondsFor([&] {
            for (int i = 0; i < n; ++i)
                for (int k = 0; k < 48; ++k) sink += p.valueAt(double(i) / sr);
        });
        std::printf("  WaParam x48 (8 voices, 2 oscs)   %8.1f x realtime\n", 1.0 / cpu);
    }
    REQUIRE(true);
}
