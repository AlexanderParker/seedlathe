#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>
#include "Voice.h"
#include "SharedFxRack.h"
#include "sl/InstrumentGen.h"
#include <algorithm>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

struct Rig {
    sl::SharedFxRack rack;
    sl::Voice voice;
    explicit Rig(double sr = 48000.0) {
        rack.prepare(sr);
        voice.prepare(sr);
    }
    // Runs the graph one sample and returns the master output.
    float bufL[1] = {0.f}, bufR[1] = {0.f};
    double step() {
        rack.beginBlock(1);
        voice.processBlock(1);
        rack.mixBlock(bufL, bufR, 1);
        return bufL[0];
    }
};

} // namespace

TEST_CASE("frequency mapping matches zyn's Z.freq") {
    REQUIRE(std::abs(sl::noteFrequency(0, 0) - 261.63) < 1e-9);
    REQUIRE(std::abs(sl::noteFrequency(0, 12) - 523.26) < 1e-6);
    REQUIRE(std::abs(sl::noteFrequency(0, -12) - 130.815) < 1e-6);
}

TEST_CASE("a voice produces sound and then goes idle") {
    Rig rig;
    const auto inst = sl::generateInstrument(13);   // "key", 2 oscillators
    rig.voice.noteOn(&rig.rack, inst, 0, 1.0, /*sustained=*/false);
    rig.rack.buildPending();

    double peak = 0.0;
    for (int i = 0; i < 48000 * 6; ++i) {
        const double y = rig.step();
        REQUIRE(std::isfinite(y));
        peak = std::max(peak, std::abs(y));
    }
    REQUIRE(peak > 1e-4);
    REQUIRE_FALSE(rig.voice.active());
}

TEST_CASE("a sustained voice holds until noteOff") {
    Rig rig;
    const auto inst = sl::generateInstrument(3703184240u);
    rig.voice.noteOn(&rig.rack, inst, 0, 1.0, /*sustained=*/true);
    rig.rack.buildPending();

    for (int i = 0; i < 48000 * 3; ++i) rig.step();
    REQUIRE(rig.voice.active());        // still held after three seconds

    rig.voice.noteOff();
    for (int i = 0; i < 48000 * 5; ++i) rig.step();
    REQUIRE_FALSE(rig.voice.active());
}

TEST_CASE("editing the instrument mid-note does not disturb a ringing voice") {
    Rig rig;
    sl::Instrument inst = sl::generateInstrument(13);
    rig.voice.noteOn(&rig.rack, inst, 0, 1.0, true);
    rig.rack.buildPending();
    for (int i = 0; i < 1000; ++i) rig.step();

    inst.oscs[0].detune = 7.0;          // mutate the caller's copy
    inst.oscCount = 1;
    const double y = rig.step();
    REQUIRE(std::isfinite(y));
    REQUIRE(rig.voice.active());        // the voice holds its own snapshot
}

TEST_CASE("voice output is linear in gain") {
    // Deliberately a hand-built instrument rather than a seed. Real seeds run
    // through shared delay and reverb whose tails dominate the peak, so their
    // peak barely moves with gain -- Chrome behaves the same way. Linearity is
    // a property of the voice path, so test it on a voice path.
    sl::Instrument inst;
    inst.typeIndex = 0;
    inst.oscCount = 1;
    sl::Osc& o = inst.oscs[0];
    o.waveform = sl::Waveform::Sine;
    o.filterType = sl::FilterType::Allpass;
    o.adsrGain = {0.01, 1.0, 0.1, 1.0, 0.5, 1.0, 0.1, 0.0};
    o.adsrFilter = {0.001, 1.0, 0.1, 1.0, 0.5, 1.0, 0.1, 1.0};
    o.adsrFilterQ = {0.001, 1.0, 0.1, 1.0, 0.5, 1.0, 0.1, 1.0};

    auto peakFor = [&](double gain) {
        Rig rig;
        rig.voice.noteOn(&rig.rack, inst, 0, gain, false);
        rig.rack.buildPending();
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i)
            peak = std::max(peak, std::abs(rig.step()));
        return peak;
    };

    const double quiet = peakFor(0.25);
    const double loud = peakFor(1.0);
    REQUIRE(quiet > 1e-4);
    INFO("quiet " << quiet << " loud " << loud << " ratio " << loud / quiet);
    REQUIRE_THAT(loud / quiet, WithinAbs(4.0, 0.05));
}

TEST_CASE("pool steals when it runs out of voices") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 4);

    const auto inst = sl::generateInstrument(13);
    for (int n = 0; n < 8; ++n) pool.noteOn(&rack, inst, n, 1.0, true);
    rack.buildPending();
    REQUIRE(pool.activeCount() == 4);

    std::vector<float> L(512), R(512);
    pool.render(L.data(), R.data(), 512);
    for (int i = 0; i < 512; ++i) {
        REQUIRE(std::isfinite(L[i]));
        REQUIRE(std::isfinite(R[i]));
    }
}

TEST_CASE("noteOff releases only the matching note") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    const auto inst = sl::generateInstrument(3703184240u);
    pool.noteOn(&rack, inst, 0, 1.0, true);
    pool.noteOn(&rack, inst, 4, 1.0, true);
    pool.noteOn(&rack, inst, 7, 1.0, true);
    rack.buildPending();
    REQUIRE(pool.activeCount() == 3);

    pool.noteOff(4);
    std::vector<float> L(4096), R(4096);
    for (int b = 0; b < 60; ++b) pool.render(L.data(), R.data(), 4096);
    REQUIRE(pool.activeCount() == 2);
}

TEST_CASE("every seed renders finite audio") {
    // The cheap net for NaN-producing edge cases: a zero envelope maximum
    // giving 0/0, a filter blowing up at Q 30, an FM matrix self-modulation
    // route running away.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::Voice v;
    v.prepare(48000.0);

    for (uint32_t s = 0; s < 400; ++s) {
        rack.reset();
        v.noteOn(&rack, sl::generateInstrument(s), 0, 1.0, false);
        rack.buildPending();
        float bl[256], br[256];
        for (int b = 0; b < 12000 / 256; ++b) {
            rack.beginBlock(256);
            v.processBlock(256);
            rack.mixBlock(bl, br, 256);
            for (int i = 0; i < 256; ++i) {
                INFO("seed " << s << " block " << b << " sample " << i);
                REQUIRE(std::isfinite(bl[i]));
                REQUIRE(std::isfinite(br[i]));
            }
        }
        v.kill();
    }
}

TEST_CASE("seeds with an FM matrix stay bounded") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::Voice v;
    v.prepare(48000.0);

    int checked = 0;
    for (uint32_t s = 0; s < 3000 && checked < 20; ++s) {
        const auto inst = sl::generateInstrument(s);
        if (!inst.hasFmMatrix) continue;
        ++checked;

        rack.reset();
        v.noteOn(&rack, inst, 0, 1.0, false);
        rack.buildPending();
        double peak = 0.0;
        float bl[256], br[256];
        for (int b = 0; b < 24000 / 256; ++b) {
            rack.beginBlock(256);
            v.processBlock(256);
            rack.mixBlock(bl, br, 256);
            for (int i = 0; i < 256; ++i) {
                INFO("seed " << s << " block " << b << " sample " << i);
                REQUIRE(std::isfinite(bl[i]));
                peak = std::max(peak, std::abs(double(bl[i])));
            }
        }
        INFO("seed " << s << " peak " << peak);
        REQUIRE(peak < 50.0);
        v.kill();
    }
    REQUIRE(checked == 20);
}

TEST_CASE("the sustain pedal defers note-offs until it lifts") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    const auto inst = sl::generateInstrument(3703184240u);   // pad, long release
    rack.prewarm(inst);

    std::vector<float> l(256), r(256);
    auto render = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) pool.render(l.data(), r.data(), 256);
    };

    pool.setSustainPedal(true);
    pool.noteOn(&rack, inst, 0, 1.0, true);
    render(20);
    REQUIRE(pool.activeCount() == 1);

    // Key up while the pedal is down: the note keeps sounding.
    pool.noteOff(0);
    render(120);                       // well past the 410 ms release
    REQUIRE(pool.activeCount() == 1);

    // Pedal up: now it releases and ends.
    pool.setSustainPedal(false);
    render(120);
    REQUIRE(pool.activeCount() == 0);
}

TEST_CASE("a note released before the pedal is pressed is unaffected") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    const auto inst = sl::generateInstrument(3703184240u);
    rack.prewarm(inst);

    std::vector<float> l(256), r(256);
    pool.noteOn(&rack, inst, 0, 1.0, true);
    for (int b = 0; b < 20; ++b) pool.render(l.data(), r.data(), 256);
    pool.noteOff(0);                   // pedal is up: releases normally
    pool.setSustainPedal(true);        // pressing it now must not revive the note
    for (int b = 0; b < 120; ++b) pool.render(l.data(), r.data(), 256);
    REQUIRE(pool.activeCount() == 0);
}

TEST_CASE("all notes off beats the pedal") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    sl::VoicePool pool;
    pool.prepare(48000.0, 8);

    const auto inst = sl::generateInstrument(3703184240u);
    rack.prewarm(inst);

    pool.setSustainPedal(true);
    pool.noteOn(&rack, inst, 0, 1.0, true);
    pool.noteOff(0);
    pool.allNotesOff();

    REQUIRE(pool.activeCount() == 0);
    // And the pedal is cleared, so the next note is not silently held.
    REQUIRE_FALSE(pool.sustainPedal());
}

TEST_CASE("pitch bend shifts a sounding note by the semitones asked for") {
    // A one-oscillator sine with no effects, so the zero crossings are the
    // pitch and nothing else.
    sl::Instrument inst;
    inst.oscCount = 1;
    sl::Osc& o = inst.oscs[0];
    o.waveform = sl::Waveform::Sine;
    o.adsrGain = {0.005, 1.0, 0.005, 1.0, 0.005, 1.0, 0.05, 0.0};
    o.adsrFilter = {0.005, 1.0, 0.005, 1.0, 0.005, 1.0, 0.05, 1.0};

    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    rack.prewarm(inst);

    auto crossings = [&](double semitones) {
        sl::VoicePool pool;
        pool.prepare(48000.0, 4);
        pool.setPitchBend(semitones);
        pool.noteOn(&rack, inst, 0, 1.0, true);

        std::vector<float> l(24000), r(24000);
        pool.render(l.data(), r.data(), 24000);   // half a second

        int n = 0;
        for (size_t i = 12000; i + 1 < l.size(); ++i)
            if ((l[i] < 0.f) != (l[i + 1] < 0.f)) ++n;
        return n;
    };

    const int flat = crossings(0.0);
    const int up = crossings(2.0);
    const int down = crossings(-2.0);
    INFO("crossings: flat " << flat << ", +2 " << up << ", -2 " << down);

    REQUIRE(flat > 0);
    // Two semitones is a ratio of 2^(1/6), about 1.1225.
    REQUIRE(double(up) / double(flat) == Catch::Approx(1.1225).epsilon(0.02));
    REQUIRE(double(down) / double(flat) == Catch::Approx(0.8909).epsilon(0.02));
}

TEST_CASE("a note started while bent is already in tune with the bend") {
    // Bending, then playing, must not snap the new note into place a block
    // later -- the voice has to inherit the bend at note-on.
    sl::Instrument inst;
    inst.oscCount = 1;
    inst.oscs[0].waveform = sl::Waveform::Sine;
    inst.oscs[0].adsrGain = {0.005, 1.0, 0.005, 1.0, 0.005, 1.0, 0.05, 0.0};
    inst.oscs[0].adsrFilter = {0.005, 1.0, 0.005, 1.0, 0.005, 1.0, 0.05, 1.0};

    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    rack.prewarm(inst);

    sl::VoicePool bentFirst;
    bentFirst.prepare(48000.0, 4);
    bentFirst.setPitchBend(2.0);
    bentFirst.noteOn(&rack, inst, 0, 1.0, true);

    sl::VoicePool bentAfter;
    bentAfter.prepare(48000.0, 4);
    bentAfter.noteOn(&rack, inst, 0, 1.0, true);
    bentAfter.setPitchBend(2.0);

    std::vector<float> a(4800), b(4800), junk(4800);
    bentFirst.render(a.data(), junk.data(), 4800);
    bentAfter.render(b.data(), junk.data(), 4800);

    for (size_t i = 0; i < a.size(); ++i)
        REQUIRE(std::fabs(a[i] - b[i]) < 1e-6f);
}
