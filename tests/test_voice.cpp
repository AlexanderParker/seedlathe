#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
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
        voice.prepare(sr, &rack);
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
    rig.voice.noteOn(inst, 0, 1.0, /*sustained=*/false);
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
    rig.voice.noteOn(inst, 0, 1.0, /*sustained=*/true);
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
    rig.voice.noteOn(inst, 0, 1.0, true);
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
        rig.voice.noteOn(inst, 0, gain, false);
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
    pool.prepare(48000.0, 4, &rack);

    const auto inst = sl::generateInstrument(13);
    for (int n = 0; n < 8; ++n) pool.noteOn(inst, n, 1.0, true);
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
    pool.prepare(48000.0, 8, &rack);

    const auto inst = sl::generateInstrument(3703184240u);
    pool.noteOn(inst, 0, 1.0, true);
    pool.noteOn(inst, 4, 1.0, true);
    pool.noteOn(inst, 7, 1.0, true);
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
    v.prepare(48000.0, &rack);

    for (uint32_t s = 0; s < 400; ++s) {
        rack.reset();
        v.noteOn(sl::generateInstrument(s), 0, 1.0, false);
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
    v.prepare(48000.0, &rack);

    int checked = 0;
    for (uint32_t s = 0; s < 3000 && checked < 20; ++s) {
        const auto inst = sl::generateInstrument(s);
        if (!inst.hasFmMatrix) continue;
        ++checked;

        rack.reset();
        v.noteOn(inst, 0, 1.0, false);
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
