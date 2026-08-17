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
#include <cstdint>

namespace seedlathe {

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
