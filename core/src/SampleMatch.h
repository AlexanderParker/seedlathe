#pragma once
#include "TopResults.h"
#include "sl/Instrument.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sl {

// Matching a seed to a recording cannot work the way the instrument-similarity
// search does: there is no parameter vector to compare against, only audio. So
// each candidate is rendered offline and compared on audio features.
//
// That is four orders of magnitude slower -- roughly sixty candidates a second
// per thread against 1.6 million for compareInstruments -- which sets the whole
// design. Renders are short, mono and at a reduced rate, the features are
// coarse, and the search runs on a thread pool reporting its best so far, so
// the user can stop the moment something sounds close enough.
inline constexpr double kMatchRate = 22050.0;
inline constexpr double kMatchSeconds = 1.5;
inline constexpr int kMatchBands = 64;      // mel bands, matching Analysis
inline constexpr int kMatchFrames = 32;     // envelope and centroid resolution

// What the matcher actually compares. Every field is normalised so that
// loudness differences between a recording and a render cannot dominate.
struct SoundFeatures {
    std::vector<float> mel;        // kMatchBands, mean-removed and L2-normalised
    std::vector<float> env;        // kMatchFrames RMS envelope, peak-normalised
    std::vector<float> centroid;   // kMatchFrames spectral centroid over Nyquist
    int rootNote = 0;              // detected fundamental, as a zyn note number
    bool ok = false;

    bool empty() const { return !ok || mel.empty(); }
};

// Features of an arbitrary mono signal at an arbitrary rate. Resamples and
// trims or pads to kMatchSeconds internally.
//
// detectPitch is off for candidate renders: the caller already chose the note
// they were rendered at, and the autocorrelation costs more than the render.
SoundFeatures featuresOf(const std::vector<float>& mono, double sampleRate,
                         bool detectPitch = true);

// Features of a seed, rendered offline at `note`.
SoundFeatures featuresOfInstrument(const Instrument& inst, int note);

// Fundamental of a signal, as a zyn note number (0 = middle C). Autocorrelation
// over the loudest window: this only has to be close enough to pick a sensible
// note to render candidates at, not to be a pitch tracker.
int detectRootNote(const std::vector<float>& mono, double sampleRate);

// 0-100. Weighted across timbre, amplitude shape and brightness, in that order
// of importance -- two sounds with the same spectrum but different attacks are
// far more alike than two with the same attack and different spectra.
double sampleSimilarity(const SoundFeatures& a, const SoundFeatures& b);

// Searches seeds for the closest render to a target sound.
class SampleSearch {
public:
    struct Result {
        uint32_t seed = 0;
        double score = -1.0;
        uint64_t tested = 0;
        bool found = false;

        // The candidate just scored, whatever it scored. Unlike the
        // parameter-space search this reports every candidate -- there are only
        // a few dozen a second -- so the runner can merge them from several
        // workers into one shared list of runners-up.
        uint32_t lastSeed = 0;
        double lastScore = -1.0;
    };

    // typeFilter: 0 for any instrument type, or 1-10 to fix the seed's last
    // digit. Unlike compareInstruments there is no target instrument to take
    // the type from, so the caller has to say.
    static Result run(const SoundFeatures& target,
                      int typeFilter,
                      uint32_t rngSeed,
                      double threshold,
                      const std::function<bool()>& shouldStop,
                      const std::function<void(const Result&)>& onProgress = {});
};

// Runs SampleSearch across a thread pool. A pool, unlike SeedSearchRunner's
// single thread, because each candidate costs a full offline render.
class SampleSearchRunner {
public:
    ~SampleSearchRunner();

    // Loads a WAV, analyses it and keeps it as the target. Returns false and
    // fills error() when the file cannot be read.
    bool loadTarget(const std::string& path);

    // Or supply audio directly, for tests and for future drag-and-drop.
    void setTarget(const std::vector<float>& mono, double sampleRate);

    bool hasTarget() const { return target_.ok; }
    const SoundFeatures& target() const { return target_; }
    const std::string& error() const { return error_; }

    void start(int typeFilter, double threshold);
    void cancel();
    bool running() const { return running_.load(std::memory_order_acquire); }

    SampleSearch::Result best() const;
    std::vector<Candidate> top() const { return top_.read(); }

private:
    void join();

    std::vector<std::thread> threads_;
    SoundFeatures target_;
    std::string error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<int> live_{0};

    // One published best across every worker, taken under a spin on a CAS of
    // the score: results arrive a few dozen times a second per thread, so the
    // contention is nothing and a mutex would only add a dependency.
    mutable std::atomic<uint32_t> bestSeed_{0};
    mutable std::atomic<double> bestScore_{-1.0};
    std::atomic<uint64_t> tested_{0};
    std::atomic<bool> found_{false};
    TopSnapshot top_;
};

} // namespace sl
