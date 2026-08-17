#pragma once
#include "sl/Instrument.h"
#include <cstdint>

namespace sl {

// Exact port of zyn's Z.getInstrument. Bounded time, zero allocation, so it is
// safe to call on the audio thread when the seed changes.
Instrument generateInstrument(uint32_t seed);

} // namespace sl
