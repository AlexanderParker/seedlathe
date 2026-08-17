#pragma once
#include "TopResults.h"
#include "sl/Instrument.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace sl {

// Port of the demo page's compareInstruments. Returns 0-100.
//
// The weights and the order of accumulation are reproduced exactly, not
// approximated: a score has to mean the same thing in the plugin as it does on
// the demo page, and identical instruments come out at 99.99999999999999 or
// 100.00000000000003 rather than a clean 100 -- which is only reproducible if
// the arithmetic happens in the same sequence.
//
// Note that this reads osc.filterType and osc.filterQ, both of which are dead
// in the audio path. That is why the data model still carries them.
double compareInstruments(const Instrument& a, const Instrument& b);

// A running similarity search. Candidate seeds are drawn from the target's own
// instrument-type digit, exactly as zyn does, because a pad will never score
// well against a drum.
class SeedSearch {
public:
    struct Result {
        uint32_t seed = 0;
        double score = -1.0;
        uint64_t tested = 0;
        bool found = false;

        // The runners-up, best first. A search that reports only its winner
        // throws away the interesting part: the near misses are often the ones
        // worth auditioning. `topRevision` lets a poller copy the list only
        // when it actually changed rather than on every progress report.
        std::vector<Candidate> top;
        uint64_t topRevision = 0;
    };

    // Runs until `shouldStop` returns true or `threshold` is reached. Reports
    // progress through `onProgress` roughly every batch, so a caller can show
    // the best-so-far and let it be auditioned mid-search.
    //
    // Cancelling keeps the best result found so far rather than discarding it.
    static Result run(const Instrument& target,
                      uint32_t rngSeed,
                      double threshold,
                      const std::function<bool()>& shouldStop,
                      const std::function<void(const Result&)>& onProgress = {});

    static constexpr int kBatchSize = 100;   // zyn's batch, and its cancel granularity
};

// Runs a SeedSearch on a background thread so the UI stays responsive.
//
// One thread is enough: the parameter-space scorer manages about 1.6 million
// candidates a second, and typically reaches a 90%+ match within a couple of
// hundred thousand. The sample-matching search will need a pool; this does not.
class SeedSearchRunner {
public:
    ~SeedSearchRunner();

    void start(const Instrument& target, double threshold);
    void cancel();                       // keeps the best result found so far
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Safe to poll at UI rate while the search runs.
    SeedSearch::Result best() const;
    std::vector<Candidate> top() const { return top_.read(); }

private:
    void join();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};

    std::atomic<uint32_t> bestSeed_{0};
    std::atomic<double> bestScore_{-1.0};
    std::atomic<uint64_t> tested_{0};
    std::atomic<bool> found_{false};
    TopSnapshot top_;
};

} // namespace sl
