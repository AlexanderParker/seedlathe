#include "RackPool.h"
#include <algorithm>

namespace sl {

void RackPool::prepare(double sampleRate, int numRacks) {
    const int n = std::max(2, numRacks);
    racks_.clear();
    racks_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        racks_.push_back(std::make_unique<SharedFxRack>());
        racks_.back()->prepare(sampleRate);
    }
    flat_.clear();
    for (auto& r : racks_) flat_.push_back(r.get());
    freeSince_.assign(static_cast<size_t>(n), 0);
    wasLive_.assign(static_cast<size_t>(n), false);
    wasLive_[0] = true;   // rack 0 starts live
    live_.store(0, std::memory_order_release);
    blockEpoch_.store(0, std::memory_order_release);
    retireRequest_.store(-1, std::memory_order_release);
}

void RackPool::audioBlockStarted(VoicePool& pool) {
    blockEpoch_.fetch_add(1, std::memory_order_acq_rel);

    // Service a retirement request. Releasing the voices happens here, on the
    // audio thread, rather than from the builder -- a voice's active flag is
    // read every block and must not be written from another thread.
    const int retire = retireRequest_.load(std::memory_order_acquire);
    if (retire >= 0) {
        SharedFxRack* r = racks_[static_cast<size_t>(retire)].get();
        if (r != liveRack()) pool.killVoicesUsing(r);
        retireRequest_.store(-1, std::memory_order_release);
    }
}

bool RackPool::rebuild(const Instrument& inst, VoicePool& pool) {
    if (racks_.empty()) return false;

    const int n = static_cast<int>(racks_.size());
    const int live = live_.load(std::memory_order_acquire);
    const uint64_t epoch = blockEpoch_.load(std::memory_order_acquire);

    // Never consider the live rack: new notes bind to it at any moment.
    int target = -1;
    for (int i = 1; i < n; ++i) {
        const int c = (live + i) % n;
        if (c == live) continue;
        if (pool.rackInUse(racks_[static_cast<size_t>(c)].get())) continue;
        if (wasLive_[static_cast<size_t>(c)] &&
            freeSince_[static_cast<size_t>(c)] + 2 > epoch) continue;
        target = c;
        break;
    }

    if (target < 0) {
        // Everything is busy. Ask the audio thread to release the oldest
        // non-live rack so the next attempt can proceed, and refuse for now
        // rather than rebuilding something in use.
        int oldest = -1;
        uint64_t oldestEpoch = UINT64_MAX;
        for (int c = 0; c < n; ++c) {
            if (c == live) continue;
            if (freeSince_[static_cast<size_t>(c)] < oldestEpoch) {
                oldestEpoch = freeSince_[static_cast<size_t>(c)];
                oldest = c;
            }
        }
        if (oldest >= 0) retireRequest_.store(oldest, std::memory_order_release);
        return false;
    }

    racks_[static_cast<size_t>(target)]->reset();
    racks_[static_cast<size_t>(target)]->prewarm(inst);

    // The outgoing rack becomes eligible only once two more blocks have run.
    freeSince_[static_cast<size_t>(live)] = epoch;
    wasLive_[static_cast<size_t>(target)] = true;
    live_.store(target, std::memory_order_release);
    return true;
}

} // namespace sl
