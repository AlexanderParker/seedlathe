#pragma once
#include "SharedFxRack.h"
#include "Voice.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace sl {

// Owns the FX racks and decides which one may be rebuilt.
//
// Rebuilding a rack frees and reallocates every buffer in it, so it must never
// happen to a rack the audio thread might touch. Dragging the seed control did
// exactly that and segfaulted.
//
// Three conditions gate a rebuild, and all three are needed:
//
//   1. The rack is not the live one. New notes bind to the live rack, so it is
//      always reachable.
//   2. No sounding voice references it.
//   3. At least two audio blocks have elapsed since it stopped being live.
//      Without this there is a window where the audio thread has read the live
//      pointer but has not yet bound a voice to it, so condition 2 reads false
//      for a rack that is about to be used.
//
// When every rack is busy the rebuild is refused rather than forced, and a
// retirement is requested instead: the audio thread releases the oldest
// non-live rack's voices at a block boundary, which frees it for the next
// attempt. Callers simply try again on the next parameter change.
class RackPool {
public:
    void prepare(double sampleRate, int numRacks,
                 size_t nodesPerRack = SharedFxRack::kDelayNodes);

    // Audio thread.
    SharedFxRack* liveRack() { return racks_[static_cast<size_t>(live_.load(std::memory_order_acquire))].get(); }
    void audioBlockStarted(VoicePool& pool);

    // Message thread. True if a rack was rebuilt and published.
    bool rebuild(const Instrument& inst, VoicePool& pool);

    int rackCount() const { return static_cast<int>(racks_.size()); }
    SharedFxRack* rackAt(int i) { return racks_[static_cast<size_t>(i)].get(); }

    // Flat view for the render path: every rack, so tails on retired ones keep
    // sounding after their last voice ends.
    SharedFxRack* const* all() const { return flat_.data(); }
    int allCount() const { return static_cast<int>(flat_.size()); }

private:
    std::vector<std::unique_ptr<SharedFxRack>> racks_;
    std::atomic<int> live_{0};
    std::atomic<uint64_t> blockEpoch_{0};
    std::vector<uint64_t> freeSince_;
    // A rack that has never been live cannot have a note on its way to it, so
    // the quiescence wait does not apply. Without this the very first build is
    // refused -- the epoch is still 0 -- and the plugin runs on a rack that was
    // never prewarmed, with no reverb at all.
    std::vector<bool> wasLive_;
    std::vector<SharedFxRack*> flat_;
    std::atomic<int> retireRequest_{-1};
};

} // namespace sl
