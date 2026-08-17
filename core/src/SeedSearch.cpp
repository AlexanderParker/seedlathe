#include "SeedSearch.h"
#include "sl/InstrumentGen.h"
#include "sl/Mulberry32.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace sl {
namespace {

double proximity(double v1, double v2, double maxDiff) {
    const double diff = std::abs(v1 - v2);
    return std::max(0.0, 1.0 - diff / maxDiff);
}

// ADSR similarity. Times vary over orders of magnitude, so they are compared on
// a log scale; levels are already 0-1.
double envSimilarity(const Adsr* ea, const Adsr* eb) {
    if (!ea || !eb) return (ea == eb) ? 1.0 : 0.0;

    const double t1[4] = {ea->aT, ea->dT, ea->sT, ea->rT};
    const double l1[4] = {ea->aV, ea->dV, ea->sV, ea->rV};
    const double t2[4] = {eb->aT, eb->dT, eb->sT, eb->rT};
    const double l2[4] = {eb->aV, eb->dV, eb->sV, eb->rV};

    double sum = 0.0;
    for (int k = 0; k < 4; ++k) {
        const double timeSim = proximity(std::log(t1[k] + 0.001), std::log(t2[k] + 0.001), 3.0);
        const double levelSim = proximity(l1[k], l2[k], 1.0);
        sum += (timeSim + levelSim) / 2.0;
    }
    return sum / 4.0;
}

// Used for the three LFOs and for FM, which share a shape in zyn.
struct Mod { bool on; int type; double frequency; double depth; };

double lfoSimilarity(const Mod& la, const Mod& lb) {
    if (!la.on && !lb.on) return 1.0;
    if (!la.on || !lb.on) return 0.0;
    double sum = 0.0;
    sum += (la.type == lb.type) ? 1.0 : 0.25;
    sum += proximity(std::log(la.frequency + 0.1), std::log(lb.frequency + 0.1), 4.0);
    sum += proximity(la.depth, lb.depth, std::max(std::max(la.depth, lb.depth), 1.0));
    return sum / 3.0;
}

Mod asMod(const Lfo& l) { return {l.on, static_cast<int>(l.type), l.frequency, l.depth}; }
Mod asMod(const Fm& f) { return {f.on, static_cast<int>(f.type), f.frequency, f.depth}; }

} // namespace

double compareInstruments(const Instrument& a, const Instrument& b) {
    double totalWeight = 0.0;
    double matchScore = 0.0;

    // Oscillator count (weight 10).
    const double maxOscs = static_cast<double>(std::max(a.oscCount, b.oscCount));
    const double minOscsD = static_cast<double>(std::min(a.oscCount, b.oscCount));
    const int minOscs = std::min(a.oscCount, b.oscCount);
    matchScore += (minOscsD / maxOscs) * 10.0;
    totalWeight += 10.0;

    for (int i = 0; i < minOscs; ++i) {
        const Osc& oa = a.oscs[static_cast<size_t>(i)];
        const Osc& ob = b.oscs[static_cast<size_t>(i)];
        const double oscWeight = 90.0 / maxOscs;   // 90 points spread over oscillators

        // Waveform, 15% of the oscillator's share.
        matchScore += (oa.waveform == ob.waveform ? 1.0 : 0.0) * oscWeight * 0.15;
        totalWeight += oscWeight * 0.15;

        // Octave, 8%. Range is -3..3.
        matchScore += proximity(oa.oct, ob.oct, 6.0) * oscWeight * 0.08;
        totalWeight += oscWeight * 0.08;

        // Detune, 5%.
        matchScore += proximity(oa.detune, ob.detune, 100.0) * oscWeight * 0.05;
        totalWeight += oscWeight * 0.05;

        // Filter type, 8%. Dead in the audio path, live here.
        matchScore += (oa.filterType == ob.filterType ? 1.0 : 0.0) * oscWeight * 0.08;
        totalWeight += oscWeight * 0.08;

        // Filter Q, 5%. Also dead in the audio path.
        matchScore += proximity(oa.filterQ, ob.filterQ, 30.0) * oscWeight * 0.05;
        totalWeight += oscWeight * 0.05;

        matchScore += envSimilarity(&oa.adsrGain, &ob.adsrGain) * oscWeight * 0.15;
        totalWeight += oscWeight * 0.15;

        matchScore += envSimilarity(&oa.adsrFilter, &ob.adsrFilter) * oscWeight * 0.10;
        totalWeight += oscWeight * 0.10;

        matchScore += envSimilarity(&oa.adsrFilterQ, &ob.adsrFilterQ) * oscWeight * 0.05;
        totalWeight += oscWeight * 0.05;

        // Pitch envelope, 7%. Only its ADSR is compared; the amount is ignored.
        matchScore += envSimilarity(oa.pEnv.on ? &oa.pEnv.env : nullptr,
                                    ob.pEnv.on ? &ob.pEnv.env : nullptr) * oscWeight * 0.07;
        totalWeight += oscWeight * 0.07;

        // Three LFOs, 4% each.
        matchScore += lfoSimilarity(asMod(oa.gLfo), asMod(ob.gLfo)) * oscWeight * 0.04;
        matchScore += lfoSimilarity(asMod(oa.fLfo), asMod(ob.fLfo)) * oscWeight * 0.04;
        matchScore += lfoSimilarity(asMod(oa.pLfo), asMod(ob.pLfo)) * oscWeight * 0.04;
        totalWeight += oscWeight * 0.12;

        // FM, 5%.
        matchScore += lfoSimilarity(asMod(oa.fm), asMod(ob.fm)) * oscWeight * 0.05;
        totalWeight += oscWeight * 0.05;

        // Effects, 5%. A present/absent mismatch scores zero for that effect
        // but still counts toward the divisor.
        double fxSim = 0.0;
        int fxCount = 0;
        if (!oa.del.on && !ob.del.on) { fxSim += 1.0; ++fxCount; }
        else if (oa.del.on && ob.del.on) {
            fxSim += (proximity(oa.del.time, ob.del.time, 0.5) +
                      proximity(oa.del.feedback, ob.del.feedback, 0.8)) / 2.0;
            ++fxCount;
        } else { ++fxCount; }

        if (!oa.verb.on && !ob.verb.on) { fxSim += 1.0; ++fxCount; }
        else if (oa.verb.on && ob.verb.on) {
            fxSim += (proximity(oa.verb.duration, ob.verb.duration, 3.0) +
                      proximity(oa.verb.decay, ob.verb.decay, 0.5)) / 2.0;
            ++fxCount;
        } else { ++fxCount; }

        matchScore += (fxCount > 0 ? fxSim / fxCount : 0.0) * oscWeight * 0.05;
        totalWeight += oscWeight * 0.05;
    }

    // FM matrix, 3 points. One having a matrix and the other not scores zero.
    if (!a.hasFmMatrix && !b.hasFmMatrix) {
        matchScore += 3.0;
    } else if (a.hasFmMatrix && b.hasFmMatrix) {
        const int maxN = std::max(a.oscCount, b.oscCount);
        const int minN = std::min(a.oscCount, b.oscCount);
        double fmSim = 0.0;
        int fmCells = 0;
        for (int s = 0; s < minN; ++s) {
            for (int t = 0; t < minN; ++t) {
                fmSim += proximity(a.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)],
                                   b.fmMatrix[static_cast<size_t>(s)][static_cast<size_t>(t)], 2.0);
                ++fmCells;
            }
        }
        const double sizePenalty = static_cast<double>(minN) / static_cast<double>(maxN);
        matchScore += (fmCells > 0 ? (fmSim / fmCells) * sizePenalty : 0.0) * 3.0;
    }
    totalWeight += 3.0;

    return totalWeight > 0.0 ? (matchScore / totalWeight) * 100.0 : 0.0;
}

SeedSearch::Result SeedSearch::run(const Instrument& target,
                                   uint32_t rngSeed,
                                   double threshold,
                                   const std::function<bool()>& shouldStop,
                                   const std::function<void(const Result&)>& onProgress) {
    Result best;
    Mulberry32 rng(rngSeed);

    // Candidates are drawn from the target's own type digit, as zyn does. The
    // last digit selects the instrument type, so searching across types would
    // spend almost all its effort on instruments that cannot score well.
    const uint32_t typeDigit = static_cast<uint32_t>(target.typeIndex);

    while (!shouldStop()) {
        for (int i = 0; i < kBatchSize; ++i) {
            const double r = rng(429496729.0);   // floor(2^32 / 10)
            const uint32_t seed =
                static_cast<uint32_t>(std::floor(r)) * 10u + typeDigit;

            const double score = compareInstruments(target, generateInstrument(seed));
            ++best.tested;

            if (score > best.score) {
                best.score = score;
                best.seed = seed;
                best.found = true;
                if (threshold > 0.0 && score >= threshold) {
                    if (onProgress) onProgress(best);
                    return best;
                }
            }
        }
        if (onProgress) onProgress(best);
    }
    return best;
}

// ------------------------------------------------------------ runner

SeedSearchRunner::~SeedSearchRunner() {
    cancel();
    join();
}

void SeedSearchRunner::join() {
    if (thread_.joinable()) thread_.join();
}

void SeedSearchRunner::start(const Instrument& target, double threshold) {
    cancel();
    join();

    cancel_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    bestScore_.store(-1.0, std::memory_order_release);
    tested_.store(0, std::memory_order_release);
    found_.store(false, std::memory_order_release);

    // The target is copied into the thread: it is a fixed-size POD, and the
    // caller's copy may be edited or replaced while the search runs.
    const Instrument copy = target;
    const uint32_t rngSeed =
        static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    thread_ = std::thread([this, copy, threshold, rngSeed] {
        auto publish = [this](const SeedSearch::Result& r) {
            bestSeed_.store(r.seed, std::memory_order_relaxed);
            bestScore_.store(r.score, std::memory_order_relaxed);
            tested_.store(r.tested, std::memory_order_relaxed);
            found_.store(r.found, std::memory_order_release);
        };
        const auto result = SeedSearch::run(
            copy, rngSeed, threshold,
            [this] { return cancel_.load(std::memory_order_acquire); },
            publish);
        publish(result);
        running_.store(false, std::memory_order_release);
    });
}

void SeedSearchRunner::cancel() {
    cancel_.store(true, std::memory_order_release);
}

SeedSearch::Result SeedSearchRunner::best() const {
    SeedSearch::Result r;
    r.found = found_.load(std::memory_order_acquire);
    r.seed = bestSeed_.load(std::memory_order_relaxed);
    r.score = bestScore_.load(std::memory_order_relaxed);
    r.tested = tested_.load(std::memory_order_relaxed);
    return r;
}

} // namespace sl
