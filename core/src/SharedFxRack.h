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

    // Block interface. Voices render a whole block into per-route buffers and
    // the graph then consumes them, which keeps a voice's state in L1 for the
    // length of a block instead of being evicted by every other voice on every
    // sample.
    static constexpr int kMaxBlock = 1024;
    void beginBlock(int frames);
    void pushBlock(const Route& route, const double* l, const double* r, int frames);

    // Mono source with constant pan gains -- what a voice actually produces,
    // since zyn pans every oscillator centre for the life of a note.
    void pushBlockMono(const Route& route, const double* mono,
                       double gainL, double gainR, int frames);
    void mixBlock(float* outL, float* outR, int frames);

    // Generates any outstanding reverb impulses. Allocates: never call from
    // the audio thread. A reverb is silent until this has run for it.
    void buildPending();

    size_t nodeCount() const { return entries_.size(); }
    bool hasPending() const { return !pending_.empty(); }
    void reset();

    static constexpr size_t kMaxFxNodes = 50;   // zyn's Z.maxFxNodes

    // Node pools are far smaller than the entry cache. An instrument has at
    // most 5 oscillators, so it can name at most 5 distinct delays and 5
    // distinct reverbs; 8 of each gives headroom. This matters because racks
    // are now multi-instance (see the plugin) and a 50-deep delay pool is 19 MB
    // of buffer on its own.
    static constexpr size_t kDelayNodes = 8;
    static constexpr size_t kVerbNodes = 8;

    // True while any node still holds state worth mixing.
    bool inUse() const { return anyLive_; }

    // True while the graph is still producing sound of its own. A reverb tail
    // outlives the note that caused it by seconds, so a rack must keep being
    // mixed after its last voice ends -- otherwise releasing a key chops the
    // tail off instantly.
    bool ringing() const { return anyLive_ && silentBlocks_ < kSilentBlocksToIdle; }

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
    bool anyLive_ = false;

    // Generated reverbs run to 3.1 s and delays feed back for a while beyond
    // that, so the idle threshold is deliberately generous: 4 s of silence at
    // the smallest sensible block.
    static constexpr int kSilentBlocksToIdle = 4 * 48000 / 64;
    int silentBlocks_ = 0;

    // Per-node input buffers for the block interface, sized in prepare().
    std::vector<std::vector<double>> delayInL_, delayInR_;
    std::vector<std::vector<double>> verbInL_, verbInR_;
    std::vector<double> blockMasterL_, blockMasterR_;

    struct Pending { int slot; double duration; double decay; uint32_t seed; };
    std::vector<Pending> pending_;
};

} // namespace sl
