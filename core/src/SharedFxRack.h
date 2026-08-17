#pragma once
#include "sl/Instrument.h"
#include "webaudio/WaNodes.h"
#include <cstdint>
#include <vector>

namespace sl {

// Reproduces zyn's global Z.fxNodes cache and the graph hanging off it.
//
// Delay and reverb nodes are SHARED between every voice and every note with an
// identical configuration. This is not an oversight to tidy up: delay feedback
// accumulates across notes and the reverb tail is common to all of them, and
// per-voice effects would sound like a different instrument entirely.
//
// The graph per oscillator, from zyn's render():
//
//   pan -> delay ---\
//      \             +--> verb -> master
//       \-- dry ----/
//
// with the dry leg present ONLY when a delay exists (zyn connects nPan to both
// nDel and nDry in that branch), and delay or verb replaced by a pass-through
// when absent.
//
// Two documented divergences from zyn, both unreachable in the fidelity test:
//
//  1. zyn keys the cache on Z.id(config), a 32-bit Java-style hash of the
//     JSON text. This keys on a hash of the config VALUES instead. The
//     equivalence classes are identical; only hash collisions would differ,
//     and collisions are accident rather than design.
//  2. cleanupFxNodes drops "the oldest half" in Object.keys() order, which in
//     V8 enumerates non-negative integer-like keys in ascending numeric order
//     before insertion-ordered ones. Reproducing that would require porting
//     Z.id and V8's key ordering. Eviction here is by insertion order.
class SharedFxRack {
public:
    // What one oscillator needs in order to push audio into the graph.
    struct Route {
        int delaySlot = -1;   // -1: no delay node, signal goes straight on
        int verbSlot = -1;    // -1: no reverb node, signal goes to master
    };

    void prepare(double sampleRate);

    // zyn calls cleanupFxNodes at the top of every render().
    void beginRender();

    // Called once per oscillator at note-on. Allocation-free: any reverb it
    // newly encounters is queued for buildPending rather than generated here.
    Route acquireRoute(const Osc& osc);

    // Acquires every route an instrument needs and generates its reverb
    // impulses up front. Allocates, so this belongs on the message thread --
    // call it when the instrument changes, and note-on then finds everything
    // already cached and never queues work from the audio thread.
    void prewarm(const Instrument& inst);

    // Called once per sample per oscillator, with the post-pan signal.
    void push(const Route& route, double l, double r);

    // Called once per sample after every voice has pushed. Advances the whole
    // graph and returns the summed master signal.
    void mixAndAdvance(double& outL, double& outR);

    // Generates any outstanding reverb impulses. Allocates: never call from
    // the audio thread. A reverb is silent until this has run for it.
    void buildPending();

    size_t nodeCount() const { return entries_.size(); }
    bool hasPending() const { return !pending_.empty(); }
    void reset();

    static constexpr size_t kMaxFxNodes = 50;   // zyn's Z.maxFxNodes

private:
    enum class Kind { Delay, Verb, Passthrough };
    struct Entry {
        uint64_t key = 0;
        Kind kind = Kind::Passthrough;
        int slot = -1;
    };
    struct Edge { int delaySlot; int verbSlot; };

    const Entry* find(uint64_t key) const;
    void addEdge(int delaySlot, int verbSlot);
    void evictOldestHalf();

    double sampleRate_ = 48000.0;
    std::vector<Entry> entries_;
    std::vector<WaDelay> delays_;
    std::vector<WaConvolver> verbs_;
    std::vector<Edge> edges_;
    std::vector<bool> delayLive_, verbLive_;
    size_t nextDelay_ = 0;
    size_t nextVerb_ = 0;

    double masterL_ = 0.0, masterR_ = 0.0;

    struct Pending { int slot; double duration; double decay; uint32_t seed; };
    std::vector<Pending> pending_;
};

} // namespace sl
