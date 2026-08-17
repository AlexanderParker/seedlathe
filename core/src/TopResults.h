#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

namespace sl {

struct Candidate {
    uint32_t seed = 0;
    double score = 0.0;
};

// The best N seeds a search has seen, descending, deduplicated by seed.
//
// Deliberately not thread safe. The parameter-space search offers a candidate
// over a million times a second, so this has to cost a compare and a branch in
// the common case; a lock there would dominate the search itself. The search
// that owns one is single-threaded, and it publishes snapshots for other
// threads to read.
class TopList {
public:
    static constexpr size_t kCapacity = 20;

    // True when the list changed, which is the caller's cue to republish.
    bool offer(uint32_t seed, double score) {
        // The early-out that makes this affordable at a million offers a
        // second: once full, anything not better than the worst kept entry is
        // rejected without touching memory beyond the last element.
        if (v_.size() >= kCapacity && score <= v_.back().score)
            return false;

        for (auto& e : v_) {
            if (e.seed != seed) continue;
            if (score <= e.score) return false;
            e.score = score;
            sort();
            return true;
        }

        v_.push_back({seed, score});
        sort();
        if (v_.size() > kCapacity) v_.pop_back();
        return true;
    }

    const std::vector<Candidate>& entries() const { return v_; }
    void clear() { v_.clear(); }

private:
    void sort() {
        // Insertion into a list of at most twenty, almost always near the end.
        for (size_t i = v_.size(); i > 1; --i) {
            if (v_[i - 1].score <= v_[i - 2].score) break;
            std::swap(v_[i - 1], v_[i - 2]);
        }
    }

    std::vector<Candidate> v_;
};

// A copy of a TopList that another thread may read while the search runs.
class TopSnapshot {
public:
    void publish(const std::vector<Candidate>& v) {
        std::lock_guard<std::mutex> lock(m_);
        v_ = v;
    }
    std::vector<Candidate> read() const {
        std::lock_guard<std::mutex> lock(m_);
        return v_;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(m_);
        v_.clear();
    }

    // For searches whose workers each find candidates independently: merge one
    // in under the lock rather than publishing a whole list.
    bool offer(uint32_t seed, double score) {
        std::lock_guard<std::mutex> lock(m_);
        TopList merged;
        for (const auto& e : v_) merged.offer(e.seed, e.score);
        const bool changed = merged.offer(seed, score);
        if (changed) v_ = merged.entries();
        return changed;
    }

private:
    mutable std::mutex m_;
    std::vector<Candidate> v_;
};

} // namespace sl
