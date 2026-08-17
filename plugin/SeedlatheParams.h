#pragma once
#include <cstdint>

namespace sl {

enum ParamIdx {
    kSeedHi = 0,
    kSeedLo,
    kVolume,
    kOctave,
    kVoices,
    kTypeFilter,   // which instrument type the dice rolls, 0 = any
    kOversample,   // 0 = off, 1 = 2x, 2 = 4x; the engine rate multiplier
    kNumParams
};

// A 32-bit seed cannot survive a single host parameter. Automation values are
// float32 in practice -- a 24-bit mantissa, about 16.7M distinct values -- so
// 4.29 billion seeds do not round-trip. The failure is silent: save seed
// 3703184240, reload, get a neighbour, and the instrument is unrecognisable.
// It presents to the user as "presets are randomly broken".
//
// Two 16-bit stepped parameters instead. 65536 < 2^24, so both round-trip
// exactly in every host. These ARE the authoritative seed -- it is deliberately
// not duplicated in the state chunk, so there is one source of truth and no
// reload ordering race. The UI shows the combined value so nobody has to think
// in halves.
inline int seedHi(uint32_t seed) { return static_cast<int>(seed >> 16); }
inline int seedLo(uint32_t seed) { return static_cast<int>(seed & 0xFFFFu); }

inline uint32_t seedFrom(int hi, int lo) {
    return (static_cast<uint32_t>(hi) << 16) | (static_cast<uint32_t>(lo) & 0xFFFFu);
}

} // namespace sl
