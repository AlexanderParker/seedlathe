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

namespace {

// A plain two-oscillator instrument for the modulation tests: sine carriers,
// flat envelopes, no filter movement, no effects. Anything that shows up in the
// output is the thing under test and not an artefact of the patch.
sl::Instrument twoSines(double gain0, double gain1) {
    sl::Instrument inst;
    inst.oscCount = 2;
    for (int i = 0; i < 2; ++i) {
        sl::Osc& o = inst.oscs[static_cast<size_t>(i)];
        const double g = (i == 0) ? gain0 : gain1;
        o.waveform = sl::Waveform::Sine;
        o.adsrGain = {0.005, g, 0.005, g, 0.005, g, 0.05, 0.0};
        o.adsrFilter = {0.005, 1.0, 0.005, 1.0, 0.005, 1.0, 0.05, 1.0};
    }
    return inst;
}

std::vector<float> renderVoice(const sl::Instrument& inst, sl::SharedFxRack& rack,
                               int frames) {
    sl::VoicePool pool;
    pool.prepare(48000.0, 4);
    pool.noteOn(&rack, inst, 0, 1.0, true);
    std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
    pool.render(l.data(), r.data(), frames);
    return l;
}

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
    }
    return std::sqrt(sum / double(n));
}

} // namespace

TEST_CASE("the FM matrix actually modulates, and only when switched on") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    const sl::Instrument plain = twoSines(1.0, 1.0);
    rack.prewarm(plain);

    sl::Instrument routed = plain;
    routed.hasFmMatrix = true;
    routed.fmMatrix[1][0] = 0.8;      // osc 1 modulates osc 0

    // Same matrix values, switch off: nothing should reach the audio.
    sl::Instrument gated = routed;
    gated.hasFmMatrix = false;

    const auto base = renderVoice(plain, rack, 12000);
    const auto withFm = renderVoice(routed, rack, 12000);
    const auto offAgain = renderVoice(gated, rack, 12000);

    INFO("modulated vs plain: " << rmsDiff(base, withFm));
    REQUIRE(rmsDiff(base, withFm) > 0.01);      // it does something
    REQUIRE(rmsDiff(base, offAgain) == 0.0);    // and nothing when off
}

TEST_CASE("a zero FM matrix entry is the same as no entry at all") {
    sl::SharedFxRack rack;
    rack.prepare(48000.0);
    const sl::Instrument plain = twoSines(1.0, 1.0);
    rack.prewarm(plain);

    sl::Instrument zeroed = plain;
    zeroed.hasFmMatrix = true;      // every amount still 0

    REQUIRE(rmsDiff(renderVoice(plain, rack, 8000),
                    renderVoice(zeroed, rack, 8000)) == 0.0);
}

TEST_CASE("the FM matrix routes source to target, not the reverse") {
    // Transposing the two indices is a one-character bug that every other test
    // here would pass, because both directions modulate *something*. This
    // separates them: oscillator 1 is silent in the output but still runs as an
    // oscillator, since the matrix taps the raw waveform ahead of the gain
    // envelope -- which is what zyn does, connecting FM to osc.frequency rather
    // than through the gain node.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    const sl::Instrument plain = twoSines(1.0, 0.0);   // osc 1 inaudible
    rack.prewarm(plain);

    sl::Instrument intoAudible = plain;
    intoAudible.hasFmMatrix = true;
    intoAudible.fmMatrix[1][0] = 0.8;    // silent osc modulates the audible one

    sl::Instrument intoSilent = plain;
    intoSilent.hasFmMatrix = true;
    intoSilent.fmMatrix[0][1] = 0.8;     // audible osc modulates the silent one

    const auto base = renderVoice(plain, rack, 12000);
    const auto audible = rmsDiff(base, renderVoice(intoAudible, rack, 12000));
    const auto silent = rmsDiff(base, renderVoice(intoSilent, rack, 12000));

    INFO("into the audible oscillator: " << audible
         << ", into the silent one: " << silent);
    REQUIRE(audible > 0.01);
    REQUIRE(silent == 0.0);
}

TEST_CASE("the pitch envelope sweeps the oscillator up from zero") {
    // zyn's quirk, and the reason this needs pinning: the pitch envelope
    // REPLACES the oscillator frequency rather than offsetting it. Z.adsr
    // schedules setValueAtTime(0) and then ramps to oFreq * amount, so a note
    // with a pitch envelope starts at 0 Hz and sweeps up. Anyone "fixing" that
    // to an offset would change the sound of every seed that uses one.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    sl::Instrument inst;
    inst.oscCount = 1;
    sl::Osc& o = inst.oscs[0];
    o.waveform = sl::Waveform::Sine;
    o.adsrGain = {0.002, 1.0, 0.002, 1.0, 0.002, 1.0, 0.05, 0.0};
    o.adsrFilter = {0.002, 1.0, 0.002, 1.0, 0.002, 1.0, 0.05, 1.0};
    o.pEnv.on = true;
    o.pEnv.amount = 1.0;                                  // ends at the note pitch
    o.pEnv.env = {0.25, 1.0, 0.05, 1.0, 0.05, 1.0, 0.05, 1.0};   // 250 ms sweep up
    rack.prewarm(inst);

    const auto out = renderVoice(inst, rack, 24000);      // half a second

    auto crossings = [&out](size_t from, size_t to) {
        int n = 0;
        for (size_t i = from; i + 1 < to && i + 1 < out.size(); ++i)
            if ((out[i] < 0.f) != (out[i + 1] < 0.f)) ++n;
        return n;
    };

    // First 50 ms: still climbing out of 0 Hz, so barely any cycles.
    const int early = crossings(0, 2400);
    // 300-350 ms: the ramp has finished and it sits at middle C, 261.6 Hz,
    // which is about 26 crossings in 50 ms.
    const int late = crossings(14400, 16800);

    INFO("crossings early " << early << ", late " << late);
    REQUIRE(early < 8);
    REQUIRE(late > 20);
    REQUIRE(late < 32);
}

TEST_CASE("a noise oscillator ignores the pitch envelope") {
    // Noise has no frequency to sweep, and zyn's render skips the pENV branch
    // for it entirely. Applying one anyway would be silent here but would
    // diverge the moment the envelope was given a different shape.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    sl::Instrument plain;
    plain.oscCount = 1;
    plain.oscs[0].waveform = sl::Waveform::Noise;
    plain.oscs[0].adsrGain = {0.002, 1.0, 0.002, 1.0, 0.002, 1.0, 0.05, 0.0};
    plain.oscs[0].adsrFilter = {0.002, 1.0, 0.002, 1.0, 0.002, 1.0, 0.05, 1.0};
    rack.prewarm(plain);

    sl::Instrument swept = plain;
    swept.oscs[0].pEnv.on = true;
    swept.oscs[0].pEnv.amount = 4.0;
    swept.oscs[0].pEnv.env = {0.2, 1.0, 0.05, 1.0, 0.05, 1.0, 0.05, 1.0};

    REQUIRE(rmsDiff(renderVoice(plain, rack, 12000),
                    renderVoice(swept, rack, 12000)) == 0.0);
}

TEST_CASE("each LFO changes the sound, and only when switched on") {
    // Three LFOs, all reachable from the designer, none previously verified to
    // do anything at the audio level. The second half of each case matters as
    // much as the first: a depth set while `on` is false must be inert, or the
    // switch in the UI is decorative.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    const sl::Instrument plain = twoSines(1.0, 0.0);
    rack.prewarm(plain);
    struct Case { const char* name; double depth; };
    const Case cases[] = {
        {"gain",   0.5},        // adds to the gain
        {"filter", 4000.0},     // hertz of cutoff
        {"pitch",  2.0},        // multiples of the note frequency
    };

    for (int which = 0; which < 3; ++which) {
        auto configure = [&](bool on) {
            sl::Instrument inst = plain;
            if (which == 1) {
                // The filter LFO needs something to filter. Against a sine
                // through a cutoff already at 20 kHz it moves the output by
                // 0.0004 RMS -- real, and indistinguishable from nothing. A
                // sawtooth through a 1 kHz cutoff is what the control is for.
                inst.oscs[0].waveform = sl::Waveform::Sawtooth;
                inst.oscs[0].adsrFilter = {0.005, 0.05, 0.005, 0.05,
                                           0.005, 0.05, 0.05, 0.05};
            }
            sl::Lfo& lfo = (which == 0) ? inst.oscs[0].gLfo
                         : (which == 1) ? inst.oscs[0].fLfo
                                        : inst.oscs[0].pLfo;
            lfo.on = on;
            lfo.type = sl::Waveform::Sine;
            lfo.frequency = 6.0;
            lfo.depth = cases[which].depth;
            return inst;
        };

        // Both renders share the case's own patch, so the filter case compares
        // sawtooth-with-LFO against sawtooth-without rather than against the
        // sine baseline.
        sl::Instrument off = configure(false);
        rack.prewarm(off);
        const auto reference = renderVoice(off, rack, 24000);

        const double active = rmsDiff(reference, renderVoice(configure(true), rack, 24000));
        const double gated = rmsDiff(reference, renderVoice(off, rack, 24000));

        INFO(cases[which].name << " LFO: on " << active << ", off " << gated);
        REQUIRE(active > 0.01);
        REQUIRE(gated == 0.0);
    }
}

TEST_CASE("the gain LFO modulates at the rate it was given") {
    // Not just "something changed": the tremolo has to run at the frequency
    // asked for. A units slip -- radians for hertz, or per-block for
    // per-sample -- would pass the test above and be badly wrong here.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    sl::Instrument inst = twoSines(0.5, 0.0);
    inst.oscs[0].gLfo.on = true;
    inst.oscs[0].gLfo.type = sl::Waveform::Sine;
    inst.oscs[0].gLfo.frequency = 8.0;
    // Small on purpose. The LFO sums into the gain AudioParam with nothing
    // clamping it, exactly as zyn's gLFO gain node does, so a large depth
    // drives the gain negative and the waveform inverts -- which doubles the
    // apparent rate of an amplitude measurement and has nothing to do with the
    // LFO's frequency. The scaled envelope peaks near 0.125 here.
    inst.oscs[0].gLfo.depth = 0.05;
    rack.prewarm(inst);

    const auto out = renderVoice(inst, rack, 48000);   // one second

    // Amplitude envelope in 5 ms blocks, then count how many times it crosses
    // its own mean going upward: that is the tremolo rate.
    constexpr size_t kBlock = 240;
    std::vector<double> env;
    for (size_t i = 0; i + kBlock <= out.size(); i += kBlock) {
        double peak = 0.0;
        for (size_t j = i; j < i + kBlock; ++j) peak = std::max(peak, double(std::fabs(out[j])));
        env.push_back(peak);
    }
    // Skip the attack, which is not part of the modulation.
    const size_t from = env.size() / 10;
    double mean = 0.0;
    for (size_t i = from; i < env.size(); ++i) mean += env[i];
    mean /= double(env.size() - from);

    int rising = 0;
    for (size_t i = from + 1; i < env.size(); ++i)
        if (env[i - 1] <= mean && env[i] > mean) ++rising;

    INFO("upward crossings in ~0.9 s at 8 Hz: " << rising);
    REQUIRE(rising >= 6);
    REQUIRE(rising <= 9);
}

TEST_CASE("distortion reaches the audio path and is gated by its switch") {
    // The shaper itself is unit-tested, but nothing checked that a voice
    // actually routes through it. Distortion was dead code in zyn -- generated
    // and never applied -- so "the parameter exists but changes nothing" is the
    // exact failure this has to rule out.
    sl::SharedFxRack rack;
    rack.prepare(48000.0);

    const sl::Instrument plain = twoSines(1.0, 0.0);
    rack.prewarm(plain);

    sl::Instrument driven = plain;
    driven.oscs[0].dist.on = true;
    driven.oscs[0].dist.amount = 400.0;
    driven.oscs[0].dist.oversample = 1;

    sl::Instrument gated = driven;
    gated.oscs[0].dist.on = false;

    const auto base = renderVoice(plain, rack, 12000);
    INFO("driven vs clean: " << rmsDiff(base, renderVoice(driven, rack, 12000)));
    REQUIRE(rmsDiff(base, renderVoice(driven, rack, 12000)) > 0.01);
    REQUIRE(rmsDiff(base, renderVoice(gated, rack, 12000)) == 0.0);
}
