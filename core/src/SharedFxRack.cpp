#include "SharedFxRack.h"
#include <cstring>

namespace sl {
namespace {

// FNV-1a over the raw bits of the values that identify a configuration.
// Hashing rather than exact comparison is the closer analogue of zyn, which
// keys on a 32-bit hash of the JSON text.
inline void hashBits(uint64_t& h, const void* data, size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}

inline void hashDouble(uint64_t& h, double v) {
    if (v == 0.0) v = 0.0;   // collapse -0.0 so it hashes with +0.0
    hashBits(h, &v, sizeof(v));
}

inline void hashInt(uint64_t& h, int v) { hashBits(h, &v, sizeof(v)); }

inline void hashAdsr(uint64_t& h, const Adsr& a) {
    hashDouble(h, a.aT); hashDouble(h, a.aV);
    hashDouble(h, a.dT); hashDouble(h, a.dV);
    hashDouble(h, a.sT); hashDouble(h, a.sV);
    hashDouble(h, a.rT); hashDouble(h, a.rV);
}

inline void hashLfo(uint64_t& h, const Lfo& l) {
    hashInt(h, l.on ? 1 : 0);
    hashInt(h, static_cast<int>(l.type));
    hashDouble(h, l.frequency);
    hashDouble(h, l.depth);
}

constexpr uint64_t kFnvOffset = 14695981039346656037ull;

uint64_t delayKey(const Delay& d) {
    uint64_t h = kFnvOffset;
    hashInt(h, 1);
    hashDouble(h, d.time);
    hashDouble(h, d.feedback);
    return h;
}

uint64_t verbKey(const Verb& v) {
    uint64_t h = kFnvOffset;
    hashInt(h, 2);
    hashDouble(h, v.duration);
    hashDouble(h, v.decay);
    return h;
}

uint64_t oscKey(const Osc& o) {
    uint64_t h = kFnvOffset;
    hashInt(h, 3);
    hashInt(h, static_cast<int>(o.waveform));
    hashAdsr(h, o.adsrGain);
    hashInt(h, static_cast<int>(o.filterType));
    hashAdsr(h, o.adsrFilter);
    hashDouble(h, o.filterQ);
    hashAdsr(h, o.adsrFilterQ);
    hashLfo(h, o.gLfo);
    hashLfo(h, o.fLfo);
    hashLfo(h, o.pLfo);
    hashInt(h, o.fm.on ? 1 : 0);
    hashDouble(h, o.fm.frequency);
    hashDouble(h, o.fm.depth);
    hashInt(h, o.pEnv.on ? 1 : 0);
    hashDouble(h, o.pEnv.amount);
    hashInt(h, o.dist.on ? 1 : 0);
    hashDouble(h, o.dist.amount);
    hashInt(h, o.oct);
    hashDouble(h, o.detune);
    hashInt(h, o.del.on ? 1 : 0);
    hashDouble(h, o.del.time);
    hashDouble(h, o.del.feedback);
    hashInt(h, o.verb.on ? 1 : 0);
    hashDouble(h, o.verb.duration);
    hashDouble(h, o.verb.decay);
    return h;
}

} // namespace

void SharedFxRack::prepare(double sampleRate, size_t nodeCount) {
    sampleRate_ = sampleRate;
    nodeCount = std::max(kMinNodes, nodeCount);

    // Pre-allocated pools so acquireRoute() never allocates. The generator's
    // delay time tops out at 0.5 s, which sets the buffer length.
    delays_.resize(nodeCount);
    for (auto& d : delays_) d.prepare(sampleRate, 0.5);

    verbs_.resize(nodeCount);
    for (auto& v : verbs_) v.prepare(sampleRate);

    delayLive_.assign(delays_.size(), false);
    verbLive_.assign(verbs_.size(), false);

    entries_.clear();
    entries_.reserve(kMaxFxNodes * 2);
    edges_.clear();
    edges_.reserve(kMaxFxNodes);
    pending_.clear();
    pending_.reserve(16);
    nextDelay_ = 0;
    nextVerb_ = 0;
    masterL_ = masterR_ = 0.0;
    anyLive_.store(false, std::memory_order_release);
    silentBlocks_.store(kSilentBlocksToIdle, std::memory_order_release);

    delayInL_.assign(delays_.size(), std::vector<double>(kMaxBlock, 0.0));
    delayInR_.assign(delays_.size(), std::vector<double>(kMaxBlock, 0.0));
    verbInL_.assign(verbs_.size(), std::vector<double>(kMaxBlock, 0.0));
    verbInR_.assign(verbs_.size(), std::vector<double>(kMaxBlock, 0.0));
    blockMasterL_.assign(kMaxBlock, 0.0);
    blockMasterR_.assign(kMaxBlock, 0.0);
}

void SharedFxRack::reset() {
    for (auto& d : delays_) d.reset();
    for (auto& v : verbs_) v.reset();
    std::fill(delayLive_.begin(), delayLive_.end(), false);
    std::fill(verbLive_.begin(), verbLive_.end(), false);
    entries_.clear();
    edges_.clear();
    pending_.clear();
    nextDelay_ = 0;
    nextVerb_ = 0;
    masterL_ = masterR_ = 0.0;
    anyLive_.store(false, std::memory_order_release);
    silentBlocks_.store(kSilentBlocksToIdle, std::memory_order_release);
}

const SharedFxRack::Entry* SharedFxRack::find(uint64_t key) const {
    for (const auto& e : entries_)
        if (e.key == key) return &e;
    return nullptr;
}

void SharedFxRack::evictOldestHalf() {
    const size_t drop = entries_.size() / 2;
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<long>(drop));
}

void SharedFxRack::beginRender() {
    if (entries_.size() > kMaxFxNodes) evictOldestHalf();
}

void SharedFxRack::addEdge(int delaySlot, int verbSlot) {
    for (const auto& e : edges_)
        if (e.delaySlot == delaySlot && e.verbSlot == verbSlot) return;
    edges_.push_back({delaySlot, verbSlot});
}

SharedFxRack::Route SharedFxRack::acquireRoute(const Osc& osc) {
    Route route;

    // A host can drive parameter changes before prepare() runs -- iPlug2 fires
    // OnParamChange during construction, ahead of OnReset. Without this the
    // pool-index modulo divides by zero and the plugin dies at startup.
    if (delays_.empty() || verbs_.empty()) return route;

    if (osc.del.on) {
        const uint64_t key = delayKey(osc.del);
        if (const Entry* e = find(key)) {
            route.delaySlot = e->slot;
        } else {
            const size_t slot = nextDelay_ % delays_.size();
            ++nextDelay_;
            WaDelay& d = delays_[slot];
            d.reset();
            d.setDelayTime(osc.del.time);
            d.setFeedback(osc.del.feedback);
            entries_.push_back({key, Kind::Delay, static_cast<int>(slot)});
            route.delaySlot = static_cast<int>(slot);
        }
        delayLive_[static_cast<size_t>(route.delaySlot)] = true;
        anyLive_.store(true, std::memory_order_release);
    } else {
        // zyn still caches a no-op gain node here, keyed on the whole
        // oscillator config. It is inaudible, but it occupies a cache slot and
        // therefore changes WHEN eviction fires.
        const uint64_t key = oscKey(osc);
        if (!find(key)) entries_.push_back({key, Kind::Passthrough, -1});
    }

    if (osc.verb.on) {
        const uint64_t key = verbKey(osc.verb);
        if (const Entry* e = find(key)) {
            route.verbSlot = e->slot;
        } else {
            const size_t slot = nextVerb_ % verbs_.size();
            ++nextVerb_;
            entries_.push_back({key, Kind::Verb, static_cast<int>(slot)});
            // Seeded from the config, so the same reverb is bit-identical
            // across runs and across processes.
            pending_.push_back({static_cast<int>(slot), osc.verb.duration,
                                osc.verb.decay,
                                static_cast<uint32_t>(key ^ (key >> 32))});
            route.verbSlot = static_cast<int>(slot);
        }
        verbLive_[static_cast<size_t>(route.verbSlot)] = true;
        anyLive_.store(true, std::memory_order_release);
    }

    if (route.delaySlot >= 0) addEdge(route.delaySlot, route.verbSlot);
    return route;
}

void SharedFxRack::push(const Route& route, double l, double r) {
    if (route.delaySlot >= 0) {
        delays_[static_cast<size_t>(route.delaySlot)].addInput(l, r);
        // zyn connects the panner to BOTH the delay and a dry gain of 1.0 when
        // a delay exists, and both feed the reverb. Dropping the dry leg would
        // make every delayed oscillator sound purely wet.
        if (route.verbSlot >= 0)
            verbs_[static_cast<size_t>(route.verbSlot)].addInput(l, r);
        else { masterL_ += l; masterR_ += r; }
    } else if (route.verbSlot >= 0) {
        verbs_[static_cast<size_t>(route.verbSlot)].addInput(l, r);
    } else {
        masterL_ += l;
        masterR_ += r;
    }
}

void SharedFxRack::mixAndAdvance(double& outL, double& outR) {
    // 1. Advance every live delay.
    for (size_t i = 0; i < delays_.size(); ++i)
        if (delayLive_[i]) delays_[i].advance();

    // 2. Route delay outputs onward, to a reverb or straight to master.
    for (const auto& e : edges_) {
        const WaDelay& d = delays_[static_cast<size_t>(e.delaySlot)];
        if (e.verbSlot >= 0)
            verbs_[static_cast<size_t>(e.verbSlot)].addInput(d.outL(), d.outR());
        else { masterL_ += d.outL(); masterR_ += d.outR(); }
    }

    // 3. Advance every live reverb and sum it into master.
    for (size_t i = 0; i < verbs_.size(); ++i) {
        if (!verbLive_[i]) continue;
        verbs_[i].advance();
        masterL_ += verbs_[i].outL();
        masterR_ += verbs_[i].outR();
    }

    outL = masterL_;
    outR = masterR_;
    masterL_ = masterR_ = 0.0;
}

void SharedFxRack::prewarm(const Instrument& inst) {
    for (int i = 0; i < inst.oscCount; ++i)
        acquireRoute(inst.oscs[static_cast<size_t>(i)]);
    buildPending();
}

void SharedFxRack::beginBlock(int frames) {
    const size_t n = static_cast<size_t>(frames);
    for (size_t i = 0; i < delays_.size(); ++i) {
        if (!delayLive_[i]) continue;
        std::fill_n(delayInL_[i].begin(), n, 0.0);
        std::fill_n(delayInR_[i].begin(), n, 0.0);
    }
    for (size_t i = 0; i < verbs_.size(); ++i) {
        if (!verbLive_[i]) continue;
        std::fill_n(verbInL_[i].begin(), n, 0.0);
        std::fill_n(verbInR_[i].begin(), n, 0.0);
    }
    std::fill_n(blockMasterL_.begin(), n, 0.0);
    std::fill_n(blockMasterR_.begin(), n, 0.0);
}

void SharedFxRack::pushBlock(const Route& route, const double* l, const double* r,
                             int frames) {
    const size_t n = static_cast<size_t>(frames);

    if (route.delaySlot >= 0) {
        double* __restrict dl = delayInL_[static_cast<size_t>(route.delaySlot)].data();
        double* __restrict dr = delayInR_[static_cast<size_t>(route.delaySlot)].data();
        for (size_t i = 0; i < n; ++i) { dl[i] += l[i]; dr[i] += r[i]; }

        // zyn connects the panner to BOTH the delay and a dry gain of 1.0 when
        // a delay exists, and both feed the reverb.
        if (route.verbSlot >= 0) {
            double* __restrict vl = verbInL_[static_cast<size_t>(route.verbSlot)].data();
            double* __restrict vr = verbInR_[static_cast<size_t>(route.verbSlot)].data();
            for (size_t i = 0; i < n; ++i) { vl[i] += l[i]; vr[i] += r[i]; }
        } else {
            for (size_t i = 0; i < n; ++i) { blockMasterL_[i] += l[i]; blockMasterR_[i] += r[i]; }
        }
    } else if (route.verbSlot >= 0) {
        double* __restrict vl = verbInL_[static_cast<size_t>(route.verbSlot)].data();
        double* __restrict vr = verbInR_[static_cast<size_t>(route.verbSlot)].data();
        for (size_t i = 0; i < n; ++i) { vl[i] += l[i]; vr[i] += r[i]; }
    } else {
        for (size_t i = 0; i < n; ++i) { blockMasterL_[i] += l[i]; blockMasterR_[i] += r[i]; }
    }
}

void SharedFxRack::pushBlockMono(const Route& route, const double* mono,
                                 double gainL, double gainR, int frames) {
    const size_t n = static_cast<size_t>(frames);

    auto addTo = [&](double* __restrict dl, double* __restrict dr) {
        for (size_t i = 0; i < n; ++i) {
            dl[i] += mono[i] * gainL;
            dr[i] += mono[i] * gainR;
        }
    };

    if (route.delaySlot >= 0) {
        addTo(delayInL_[static_cast<size_t>(route.delaySlot)].data(),
              delayInR_[static_cast<size_t>(route.delaySlot)].data());
        // zyn connects the panner to BOTH the delay and a dry gain of 1.0 when
        // a delay exists, and both feed the reverb.
        if (route.verbSlot >= 0)
            addTo(verbInL_[static_cast<size_t>(route.verbSlot)].data(),
                  verbInR_[static_cast<size_t>(route.verbSlot)].data());
        else
            addTo(blockMasterL_.data(), blockMasterR_.data());
    } else if (route.verbSlot >= 0) {
        addTo(verbInL_[static_cast<size_t>(route.verbSlot)].data(),
              verbInR_[static_cast<size_t>(route.verbSlot)].data());
    } else {
        addTo(blockMasterL_.data(), blockMasterR_.data());
    }
}

void SharedFxRack::mixBlock(float* outL, float* outR, int frames) {
    const size_t n = static_cast<size_t>(frames);

    // Delays first, then their outputs feed the reverbs, then the reverbs sum
    // to master -- the same ordering as the per-sample path, just walked over
    // contiguous buffers one node at a time.
    for (size_t d = 0; d < delays_.size(); ++d) {
        if (!delayLive_[d]) continue;
        WaDelay& node = delays_[d];
        const double* __restrict il = delayInL_[d].data();
        const double* __restrict ir = delayInR_[d].data();

        for (size_t i = 0; i < n; ++i) {
            node.addInput(il[i], ir[i]);
            node.advance();
            const double ol = node.outL(), orr = node.outR();
            bool routed = false;
            for (const auto& e : edges_) {
                if (e.delaySlot != static_cast<int>(d)) continue;
                routed = true;
                if (e.verbSlot >= 0) {
                    verbInL_[static_cast<size_t>(e.verbSlot)][i] += ol;
                    verbInR_[static_cast<size_t>(e.verbSlot)][i] += orr;
                } else {
                    blockMasterL_[i] += ol;
                    blockMasterR_[i] += orr;
                }
            }
            if (!routed) { blockMasterL_[i] += ol; blockMasterR_[i] += orr; }
        }
    }

    for (size_t v = 0; v < verbs_.size(); ++v) {
        if (!verbLive_[v]) continue;
        WaConvolver& node = verbs_[v];
        const double* __restrict il = verbInL_[v].data();
        const double* __restrict ir = verbInR_[v].data();
        for (size_t i = 0; i < n; ++i) {
            node.addInput(il[i], ir[i]);
            node.advance();
            blockMasterL_[i] += node.outL();
            blockMasterR_[i] += node.outR();
        }
    }

    bool silent = true;
    for (size_t i = 0; i < n; ++i) {
        const double l = blockMasterL_[i], r = blockMasterR_[i];
        outL[i] = static_cast<float>(l);
        outR[i] = static_cast<float>(r);
        if (l > 1e-7 || l < -1e-7 || r > 1e-7 || r < -1e-7) silent = false;
    }
    // Saturating, so a rack that has been idle for hours cannot overflow the
    // counter back below the threshold and claim to be ringing again.
    const int prev = silentBlocks_.load(std::memory_order_relaxed);
    silentBlocks_.store(silent ? std::min(prev + 1, kSilentBlocksToIdle) : 0,
                        std::memory_order_release);
}

void SharedFxRack::buildPending() {
    for (const auto& p : pending_)
        verbs_[static_cast<size_t>(p.slot)].buildImpulse(p.duration, p.decay, p.seed);
    pending_.clear();
}

} // namespace sl
