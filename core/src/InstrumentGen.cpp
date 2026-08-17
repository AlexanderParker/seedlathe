#include "sl/InstrumentGen.h"
#include "sl/Mulberry32.h"
#include <algorithm>
#include <cmath>
#include <string>

// Exact port of zyn's Z.getInstrument. The order of r() calls IS the
// specification: move one and every seed downstream of it changes.

namespace sl {
namespace {

struct TypeDef {
    double gT[4], gV[4];   // gain envelope: times, then values
    double fT[4], fV[4];   // filter envelope
    double pT[4], pV[4];   // pitch envelope
    double pe;             // pitch-envelope probability
    double o;              // oscillator-count scale
    const Waveform* w;
    int wn;
};

constexpr Waveform kAll[]   = {Waveform::Sine, Waveform::Square,
                               Waveform::Sawtooth, Waveform::Triangle};
constexpr Waveform kLead[]  = {Waveform::Sawtooth, Waveform::Square, Waveform::Triangle};
constexpr Waveform kBass[]  = {Waveform::Sine, Waveform::Sawtooth,
                               Waveform::Square, Waveform::Triangle};
constexpr Waveform kPluck[] = {Waveform::Triangle, Waveform::Sawtooth, Waveform::Square};
constexpr Waveform kBell[]  = {Waveform::Sine, Waveform::Triangle};
constexpr Waveform kStr[]   = {Waveform::Sawtooth, Waveform::Triangle};
constexpr Waveform kDrum[]  = {Waveform::Noise, Waveform::Sine, Waveform::Triangle};
constexpr Waveform kFx[]    = {Waveform::Noise, Waveform::Sine, Waveform::Square,
                               Waveform::Sawtooth, Waveform::Triangle};

const TypeDef kTypes[10] = {
  // 0 pad
  {{0.8,0.5,1,0.8},{1,0.8,0.6,0},{0.5,0.3,0.5,0.5},{1,0.8,0.6,0.6},
   {0.3,0.2,0.2,0.3},{0.1,0.8,0.6,0.2},0.05,4,kAll,4},
  // 1 lead
  {{0.05,0.2,0.4,0.3},{1,0.8,0.6,0},{0.1,0.3,0.3,0.2},{0.5,1,0.7,0.3},
   {0.05,0.1,0.2,0.1},{0.2,0.8,0.5,0.15},0.05,3,kLead,3},
  // 2 bass
  {{0.01,0.1,0.3,0.2},{1,0.7,0.4,0},{0.02,0.15,0.2,0.1},{1,0.5,0.3,0.2},
   {0.01,0.05,0.1,0.05},{1,0.5,0.5,0.2},0.1,2,kBass,4},
  // 3 key
  {{0.01,0.3,0.4,0.3},{1,0.6,0.3,0},{0.01,0.2,0.3,0.2},{1,0.6,0.3,0.2},
   {0.01,0.1,0.1,0.1},{0.15,0.6,0.3,0.1},0.05,3,kAll,4},
  // 4 pluck
  {{0.005,0.15,0.05,0.1},{1,0.3,0.1,0},{0.005,0.1,0.05,0.05},{1,0.3,0.1,0.1},
   {0.005,0.05,0.02,0.02},{0.2,0.3,0.1,0.1},0.2,2,kPluck,3},
  // 5 bell
  {{0.001,0.8,0.5,0.5},{1,0.5,0.2,0},{0.001,0.5,0.4,0.3},{1,0.8,0.5,0.3},
   {0.001,0.3,0.2,0.2},{0.15,0.5,0.2,0.1},0.2,4,kBell,2},
  // 6 string
  {{0.4,0.2,0.8,0.4},{1,0.9,0.7,0},{0.3,0.2,0.5,0.3},{0.3,1,0.8,0.5},
   {0.2,0.1,0.3,0.2},{0.1,0.9,0.7,0.3},0.15,4,kStr,2},
  // 7 drum
  {{0.005,0.05,0.1,0.01},{1,0.3,0,0},{0.005,0.08,0.05,0.01},{1,0.5,0,0},
   {0.005,0.03,0.02,0.01},{1,0.5,0,0},0.5,2,kDrum,3},
  // 8 perc
  {{0.001,0.1,0.15,0.1},{1,0.5,0.1,0},{0.001,0.12,0.1,0.08},{1,0.6,0.2,0.1},
   {0.001,0.08,0.05,0.05},{1,0.8,0.5,0.2},0.4,3,kAll,4},
  // 9 fx
  {{0.5,0.5,0.5,0.5},{0.5,1,0.5,0},{0.3,0.4,0.4,0.3},{0.5,1,0.5,0.5},
   {0.2,0.3,0.3,0.2},{0.5,1,0.5,0.5},0.3,5,kFx,5},
};

constexpr FilterType kFilters[7] = {
    FilterType::Lowpass, FilterType::Highpass, FilterType::Bandpass,
    FilterType::Lowshelf, FilterType::Highshelf, FilterType::Peaking,
    FilterType::Allpass};

// gEnv(t, v): four value draws normalised by their maximum, then four time
// draws. The value-before-time order is load-bearing.
Adsr gEnv(Mulberry32& r, const double t[4], const double v[4]) {
    double n[4] = {r(v[0]), r(v[1]), r(v[2]), r(v[3])};
    const double mx = std::max({n[0], n[1], n[2], n[3]});
    for (double& x : n) x /= mx;   // matches JS: 0/0 yields NaN, deliberately

    Adsr a;
    a.aT = r(t[0]); a.aV = n[0];
    a.dT = r(t[1]); a.dV = n[1];
    a.sT = r(t[2]); a.sV = n[2];
    a.rT = r(t[3]); a.rV = n[3];
    return a;
}

// hl(): r(r() < 0.1 ? 100 : r() < 0.1 ? 10 : 1) + 0.1
// Consumes two draws on the first branch, three otherwise.
double hl(Mulberry32& r) {
    double scale;
    if (r() < 0.1) scale = 100.0;
    else if (r() < 0.1) scale = 10.0;
    else scale = 1.0;
    return r(scale) + 0.1;
}

Waveform pickWave(Mulberry32& r, const Waveform* w, int n) {
    return w[static_cast<int>(std::floor(r(static_cast<double>(n))))];
}

int firstDecimalDigit(uint32_t seed) {
    const std::string s = std::to_string(seed);
    return s.empty() ? 0 : (s[0] - '0');
}

} // namespace

Instrument generateInstrument(uint32_t seed) {
    Mulberry32 r(seed);
    // Side stream for distortion, mirroring the upstream zyn fix. Drawing the
    // curve amount from `r` would shift every oscillator generated afterwards.
    Mulberry32 dR(seed + 7777u);

    Instrument inst;
    inst.typeIndex = static_cast<int>(seed % 10u);
    const TypeDef& T = kTypes[inst.typeIndex];

    // zyn re-evaluates the loop bound on every iteration, drawing a fresh
    // random number each pass. Do NOT hoist this out of the condition.
    int i = 0;
    while (i < static_cast<int>(std::floor(r(T.o) + 1.0))) {
        if (i >= kMaxOscs) break;   // fx tops out at 5; guards the fixed array
        Osc& d = inst.oscs[static_cast<size_t>(i)];

        d.waveform = pickWave(r, T.w, T.wn);
        const bool nN = (d.waveform != Waveform::Noise);

        d.adsrGain    = gEnv(r, T.gT, T.gV);
        d.filterType  = kFilters[static_cast<int>(std::floor(r(7.0)))];
        d.adsrFilter  = gEnv(r, T.fT, T.fV);
        d.filterQ     = r(30.0);
        d.adsrFilterQ = gEnv(r, T.fT, T.fV);

        if (r() < 0.1) {
            d.gLfo.on = true;
            d.gLfo.type = pickWave(r, kAll, 4);
            d.gLfo.frequency = hl(r);
            d.gLfo.depth = r(1.0);
        }
        // `r() < 0.1 && nN` short-circuits: when the draw fails, nN is never
        // consulted and no further draws happen.
        if (r() < 0.1 && nN) {
            d.fLfo.on = true;
            d.fLfo.type = pickWave(r, kAll, 4);
            d.fLfo.frequency = hl(r);
            d.fLfo.depth = r(8000.0);
        }
        if (r() < 0.1 && nN) {
            d.pLfo.on = true;
            d.pLfo.type = pickWave(r, kAll, 4);
            d.pLfo.frequency = hl(r);
            d.pLfo.depth = r(10.0) + 1.0;
        }
        if (r() < 0.3 && nN) {
            d.fm.on = true;
            d.fm.type = pickWave(r, kAll, 4);
            d.fm.frequency = hl(r);
            d.fm.depth = r(100.0) + 1.0;
        }
        if (r() < T.pe && nN) {
            d.pEnv.on = true;
            d.pEnv.amount = r();               // amount is drawn before gEnv
            d.pEnv.env = gEnv(r, T.pT, T.pV);
        }
        if (r() < 0.05) {
            d.dist.on = true;
            d.dist.amount = dR(500.0);         // side stream, not r
            const int k = static_cast<int>(std::floor(r(3.0)));
            d.dist.oversample = (k == 2) ? 4 : (k == 1) ? 2 : 1;
        }

        d.oct = static_cast<int>(std::floor(r(4.0))) - 3;
        d.detune = (r() < 0.2) ? 5.0 : 0.0;    // SEMITONES

        if (r() > 0.5) {
            d.del.on = true;
            d.del.time = r(0.5);
            d.del.feedback = r(0.8);
        }
        if (r() > 0.5) {
            d.verb.on = true;
            d.verb.duration = r(3.0) + 0.1;
            d.verb.decay = r() * 0.5 + 0.5;
        }
        ++i;
    }
    inst.oscCount = i;

    // FM matrix uses a separate PRNG at seed + 9999, which is how it was added
    // upstream without disturbing pre-existing seeds.
    if (inst.oscCount > 1) {
        Mulberry32 fmR(seed + 9999u);
        const double fmProb = firstDecimalDigit(seed) / 10.0;
        if (fmR() < fmProb) {
            inst.hasFmMatrix = true;
            const double n = static_cast<double>(inst.oscCount);
            do {
                const int src = static_cast<int>(std::floor(fmR(n)));
                const int tgt = static_cast<int>(std::floor(fmR(n)));
                // The value draw only happens when the slot is free, so a
                // repeated pick changes the stream. Keep the branch.
                if (inst.fmMatrix[static_cast<size_t>(src)][static_cast<size_t>(tgt)] == 0.0)
                    inst.fmMatrix[static_cast<size_t>(src)][static_cast<size_t>(tgt)] =
                        fmR(2.0) - 1.0;
            } while (fmR() < 0.5);
        }
    }

    return inst;
}

} // namespace sl
