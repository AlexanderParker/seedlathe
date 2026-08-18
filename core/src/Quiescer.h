#pragma once
#include <atomic>
#include <chrono>
#include <thread>

namespace sl {

// Lets a control thread reallocate buffers that an audio thread walks.
//
// The audio thread claims each block by incrementing a counter BEFORE reading
// the blocked flag, and increments it again on the way out -- so an odd value
// means a block is genuinely in flight. Claiming after the flag read would
// leave a window where a block had passed the check and not yet announced
// itself, which is the whole bug this exists to avoid.
//
// The control thread raises the flag, which makes every block that starts from
// then on bail out without touching the guarded state, and then waits for the
// counter to go even. Once both hold, nothing is inside and nothing new can get
// in, so freeing and reallocating is safe.
class Quiescer {
public:
    // Audio thread. True when the guarded state may be used; false means the
    // caller must produce silence and return. Pair every call with leave(),
    // including the false one.
    bool enter() {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        return !blocked_.load(std::memory_order_acquire);
    }

    void leave() { seq_.fetch_add(1, std::memory_order_release); }

    // Control thread. Runs `work` with no audio block in flight and none able
    // to start. Returns false WITHOUT running it if the audio thread does not
    // leave its block within the timeout.
    //
    // Refusing rather than proceeding is deliberate. A block outlasting the
    // timeout means the audio thread is wedged or suspended -- some hosts do
    // suspend it -- and it will resume in the middle of the buffers being
    // freed. A dropped reconfiguration is a retry; a reallocation under a
    // sleeping audio thread is a crash whenever it wakes.
    //
    // `work` must not throw: the flag is only lowered on the way out.
    template <class Work>
    bool withQuiescence(Work&& work, int timeoutMs = 400) {
        blocked_.store(true, std::memory_order_release);

        bool idle = false;
        for (int i = 0; i < timeoutMs; ++i) {
            if ((seq_.load(std::memory_order_acquire) & 1u) == 0u) { idle = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (idle) work();

        blocked_.store(false, std::memory_order_release);
        return idle;
    }

    // Test and diagnostic use.
    bool blocked() const { return blocked_.load(std::memory_order_acquire); }
    unsigned sequence() const { return seq_.load(std::memory_order_acquire); }

private:
    std::atomic<unsigned> seq_{0};
    std::atomic<bool> blocked_{false};
};

} // namespace sl
