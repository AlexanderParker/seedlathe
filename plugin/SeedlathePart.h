#pragma once

// One multitimbral part: an instrument, the voices playing it, and the shared
// FX racks those voices are bound to.
//
// In single mode there is one part and this is exactly the state the plugin
// used to hold directly. In multitimbral mode there are sixteen, one per MIDI
// channel, and every one of them is a complete engine -- delay feedback and
// reverb tails are shared between the voices of an instrument and must not be
// shared between instruments, so parts cannot pool their racks.
//
// That is what makes the sizing here matter. A 0.5 s stereo delay line is
// 384 kB at 48 kHz. Sixteen parts at the single-mode allocation -- four racks
// of eight delays and eight reverbs -- would be well over a hundred megabytes
// of buffer, for instruments that can name at most five distinct delays each.
// Multitimbral parts therefore get fewer racks and the minimum node count,
// which is not a compromise on sound: the pools are a cache, and five slots is
// what a five-oscillator instrument can actually use at once.

#include "RackPool.h"
#include "SharedFxRack.h"
#include "Voice.h"
#include "sl/Instrument.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace seedlathe {

// One entry in a part's undo history: everything that makes up "the sound you
// were on", so going back restores a preset load as completely as it restores
// a dice roll.
//
// The seed alone would not do it. An edited patch is a seed plus a deviation,
// and a preset also carries an octave -- go back to a seed with none of that
// and you land somewhere the user was never at.
struct Snapshot {
    uint32_t seed = 0;
    sl::Instrument inst{};
    bool edited = false;
    int octave = 0;
};

struct Part {
    // Single mode: one part, so it can afford the full allocation.
    static constexpr int kRacksSingle = 4;
    static constexpr int kRacksMulti = 2;

    sl::RackPool racks;
    sl::VoicePool pool;

    // Double buffer: the message thread writes the inactive slot and flips the
    // index, the audio thread only ever reads the published one.
    std::array<sl::Instrument, 2> instruments{};
    std::atomic<int> live{0};

    // The designer's working copy. Edits land here, then the publish step hands
    // a snapshot to the audio thread through the double buffer above.
    sl::Instrument edit{};
    bool edited = false;
    bool pendingPublish = false;
    bool racksBuilt = false;

    uint32_t seed = 0;
    bool hasSeed = false;
    int designOsc = 0;

    // Sounds visited on this part, oldest first. Per part rather than global:
    // going back should undo what happened to the instrument you are looking
    // at, not walk backwards through sixteen of them interleaved.
    //
    // An instrument is about 2 kB, so the depth is what bounds the memory: 64
    // is roughly 128 kB per part and deeper than anyone retraces by hand.
    static constexpr size_t kMaxHistory = 64;
    std::vector<Snapshot> history;

    // Sounds stepped back OVER, newest first, so Next can walk forwards again.
    // Cleared by any new sound: once you branch, the path you came back from
    // is not somewhere "forward" any more.
    std::vector<Snapshot> future;

    // Dragging the seed box emits a new seed per mouse move, and each one
    // replaces the instrument. Recording them all would bury the sound the
    // user actually wants under a hundred positions of one gesture, so pushes
    // that arrive in a burst keep only the FIRST -- which is the state the
    // gesture started from, and the only one worth going back to.
    //
    // Time rather than a drag flag because the seed box is not the only
    // control that streams: the mouse wheel does too, and so would anything
    // added later.
    static constexpr auto kCoalesce = std::chrono::milliseconds(500);
    std::chrono::steady_clock::time_point lastPush{};

    // `now` is a parameter so a test can drive the coalescing window without
    // sleeping half a second per entry.
    void pushHistory(const Snapshot& s,
                     std::chrono::steady_clock::time_point now =
                         std::chrono::steady_clock::now()) {
        if (!history.empty() && now - lastPush < kCoalesce) {
            lastPush = now;
            return;
        }
        lastPush = now;

        // Replacing a sound with the same sound must not fill the history with
        // entries that go nowhere. Only unedited patches are collapsed: for an
        // edited one the seed says nothing about what the instrument holds, so
        // two entries sharing a seed can still be different sounds.
        if (!history.empty() && !s.edited && !history.back().edited &&
            history.back().seed == s.seed && history.back().octave == s.octave)
            return;
        if (history.size() >= kMaxHistory) history.erase(history.begin());
        history.push_back(s);

        // A new sound is a branch, and there is nothing forward of a branch.
        future.clear();
    }

    // Back: the caller hands over the sound being left, which becomes the
    // forward step, and receives the one to restore. Returns false when there
    // is nothing behind.
    bool stepBack(const Snapshot& leaving, Snapshot& restored) {
        if (history.empty()) return false;
        restored = history.back();
        history.pop_back();
        if (future.size() >= kMaxHistory) future.erase(future.begin());
        future.push_back(leaving);
        return true;
    }

    // Next: the mirror of stepBack. The sound being left goes back onto the
    // history, so Back and Next walk the same line in both directions.
    bool stepForward(const Snapshot& leaving, Snapshot& restored) {
        if (future.empty()) return false;
        restored = future.back();
        future.pop_back();
        if (history.size() >= kMaxHistory) history.erase(history.begin());
        history.push_back(leaving);
        return true;
    }

    // Parts other than the first are only allocated once something addresses
    // them, so a two-part session does not pay for sixteen.
    bool allocated = false;

    // Set as soon as anything asks for this part, which can happen before the
    // host has given a sample rate -- UnserializeState routinely runs ahead of
    // OnReset. Allocation then happens when the rate is known.
    bool requested = false;

    void prepare(double sampleRate, bool multi, int voices) {
        racks.prepare(sampleRate, multi ? kRacksMulti : kRacksSingle,
                      multi ? sl::SharedFxRack::kMinNodes
                            : sl::SharedFxRack::kDelayNodes);
        pool.prepare(sampleRate, voices);
        racksBuilt = false;
        allocated = true;
    }

    const sl::Instrument& livePatch() const {
        return instruments[static_cast<size_t>(live.load(std::memory_order_acquire))];
    }

    // True when this part has anything worth rendering: sounding voices, or a
    // rack still ringing a tail out.
    bool sounding() const {
        if (!allocated) return false;
        if (pool.activeCount() > 0) return true;
        for (int i = 0; i < racks.allCount(); ++i)
            if (racks.all()[i]->ringing()) return true;
        return false;
    }
};

} // namespace seedlathe
