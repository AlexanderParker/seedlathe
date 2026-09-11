#include <catch2/catch_test_macros.hpp>

#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/Instrument.h"
#include "sl/InstrumentGen.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Live filter modulation: the only thing in the engine that reaches a note
// which has already started. Everything else about a seed is scheduled at
// note-on, so these tests are about WHEN the change lands as much as what it
// does.

namespace {

// A deliberately plain instrument rather than a seed: one sawtooth, a filter
// envelope pinned wide open, no resonance, no effects. A generated seed would
// bring an LFO or a reverb tail along and make the measurement about those.
sl::Instrument brightSaw() {
    sl::Instrument inst;
    inst.typeIndex = 0;
    inst.oscCount = 1;

    sl::Osc& o = inst.oscs[0];
    o.waveform = sl::Waveform::Sawtooth;
    // Attack to full immediately and stay there: sustained notes hold at S.
    o.adsrGain = {0.005, 1.0, 0.0, 1.0, 0.0, 1.0, 0.05, 0.0};
    // Cutoff envelope at 1.0 is 20 kHz, so the unmodulated note is unfiltered
    // and every dB the modulation removes is unambiguous.
    o.adsrFilter = {0.001, 1.0, 0.0, 1.0, 0.0, 1.0, 0.05, 1.0};
    o.adsrFilterQ = {0.001, 0.0, 0.0, 0.0, 0.0, 0.0, 0.05, 0.0};
    return inst;
}

struct Rig {
    sl::SharedFxRack rack;
    sl::VoicePool pool;
    std::vector<float> l, r;

    explicit Rig(double sr = 48000.0) : l(256), r(256) {
        rack.prepare(sr);
        pool.prepare(sr, 8);
    }

    // Renders `blocks` of 256 frames and reports the mean square of the
    // output and of its first difference. The difference is a crude high-pass:
    // a lowpass sweeping down kills it far faster than it kills the total.
    void measure(int blocks, double& rms, double& hf) {
        double sum = 0.0, dsum = 0.0;
        double prev = 0.0;
        size_t n = 0;
        for (int b = 0; b < blocks; ++b) {
            pool.render(l.data(), r.data(), 256);
            for (int i = 0; i < 256; ++i) {
                const double x = l[static_cast<size_t>(i)];
                sum += x * x;
                const double d = x - prev;
                dsum += d * d;
                prev = x;
                ++n;
            }
        }
        const double inv = 1.0 / double(std::max<size_t>(n, 1));
        rms = std::sqrt(sum * inv);
        hf = std::sqrt(dsum * inv);
    }
};

double dB(double a, double b) { return 20.0 * std::log10(std::max(a, 1e-12) /
                                                         std::max(b, 1e-12)); }

} // namespace

TEST_CASE("filter modulation reaches a note that is already sounding") {
    const sl::Instrument inst = brightSaw();

    Rig rig;
    rig.rack.prewarm(inst);
    rig.rack.buildPending();
    rig.pool.noteOn(&rig.rack, inst, 0, 1.0, /*sustained=*/true);

    // Settle past the attack, then measure the unmodulated note.
    double warmRms = 0.0, warmHf = 0.0;
    rig.measure(20, warmRms, warmHf);
    double openRms = 0.0, openHf = 0.0;
    rig.measure(40, openRms, openHf);
    REQUIRE(openRms > 1e-4);

    // Close the filter WITHOUT retriggering. Five octaves down from 20 kHz is
    // 625 Hz, well under the sawtooth's harmonics at middle C.
    rig.pool.setFilterMod(-60.0, 0.0);

    // Skip the smoothing glide, then measure again.
    double glideRms = 0.0, glideHf = 0.0;
    rig.measure(10, glideRms, glideHf);
    double closedRms = 0.0, closedHf = 0.0;
    rig.measure(40, closedRms, closedHf);

    INFO("hf open " << openHf << " closed " << closedHf
                    << " (" << dB(closedHf, openHf) << " dB)");
    REQUIRE(dB(closedHf, openHf) < -12.0);
    // The fundamental survives, so this is a filter and not a mute.
    REQUIRE(closedRms > openRms * 0.05);
}

TEST_CASE("filter modulation at its default changes nothing at all") {
    // The fidelity suite compares this engine sample for sample against
    // zyn.js, which has no modulation path. x1.0 and +0.0 are exact in IEEE
    // arithmetic, so an unmodulated render must be bit-identical -- not close.
    const auto inst = sl::generateInstrument(3703184240u);

    const auto run = [&inst](bool touchMod) {
        Rig rig;
        rig.rack.prewarm(inst);
        rig.rack.buildPending();
        if (touchMod) rig.pool.setFilterMod(0.0, 0.0);
        rig.pool.noteOn(&rig.rack, inst, 0, 1.0, true);
        std::vector<float> out;
        for (int b = 0; b < 120; ++b) {
            rig.pool.render(rig.l.data(), rig.r.data(), 256);
            out.insert(out.end(), rig.l.begin(), rig.l.end());
        }
        return out;
    };

    const auto plain = run(false);
    const auto zeroed = run(true);
    REQUIRE(plain.size() == zeroed.size());
    for (size_t i = 0; i < plain.size(); ++i)
        REQUIRE(plain[i] == zeroed[i]);
}

TEST_CASE("resonance modulation adds a peak rather than level") {
    sl::Instrument inst = brightSaw();
    // Park the cutoff well inside the harmonic series so a resonant peak has
    // something to ring on: 20 kHz x 0.05 is 1 kHz.
    inst.oscs[0].adsrFilter = {0.001, 0.05, 0.0, 0.05, 0.0, 0.05, 0.05, 0.05};

    const auto peakOf = [&inst](double resDb) {
        Rig rig;
        rig.rack.prewarm(inst);
        rig.rack.buildPending();
        rig.pool.setFilterMod(0.0, resDb);
        rig.pool.noteOn(&rig.rack, inst, 0, 1.0, true);
        double rms = 0.0, hf = 0.0;
        rig.measure(30, rms, hf);      // settle
        rig.measure(40, rms, hf);
        return rms;
    };

    const double flat = peakOf(0.0);
    const double resonant = peakOf(20.0);
    INFO("flat " << flat << ", resonant " << resonant);
    REQUIRE(resonant > flat * 1.5);
}

TEST_CASE("a recycled voice starts on the current modulation, not the old one") {
    // An idle voice's smoother is frozen where its last note left it, because
    // only a sounding voice advances it. Without the snap at note-on, a note
    // struck after the filter was reopened would glide up from the closed
    // value -- a filter swell on every attack, for 12 ms, forever after one
    // sweep.
    //
    // The measurement is against the same note struck on a pool that was
    // never modulated, so the attack ramp cancels out and what is left is the
    // filter.
    const sl::Instrument inst = brightSaw();

    const auto earlyBrightness = [&inst](bool closeFirst) {
        Rig rig;
        rig.rack.prewarm(inst);
        rig.rack.buildPending();

        if (closeFirst) {
            rig.pool.setFilterMod(-60.0, 0.0);
            rig.pool.noteOn(&rig.rack, inst, 0, 1.0, true);
            double rms = 0.0, hf = 0.0;
            rig.measure(40, rms, hf);
            rig.pool.allNotesOff();
            rig.pool.setFilterMod(0.0, 0.0);
        }

        rig.pool.noteOn(&rig.rack, inst, 0, 1.0, true);
        double rms = 0.0, hf = 0.0;
        rig.measure(1, rms, hf);      // 5.3 ms, well inside the 12 ms glide
        return hf;
    };

    const double fresh = earlyBrightness(false);
    const double recycled = earlyBrightness(true);
    INFO("fresh " << fresh << ", recycled " << recycled);
    REQUIRE(fresh > 0.0);
    REQUIRE(recycled > fresh * 0.9);
}
