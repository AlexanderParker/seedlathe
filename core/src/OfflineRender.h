#pragma once
#include "sl/Instrument.h"
#include <string>
#include <vector>

namespace sl {

struct RenderResult {
    std::vector<float> left;
    std::vector<float> right;
};

// Renders one note through the whole chain, exactly as zyn's Z.play does:
//   voices -> masterGain (unity) -> DynamicsCompressor -> output
//
// A fresh SharedFxRack per call, so cache state cannot leak between renders --
// that is what makes two identical calls produce identical audio, which the
// fidelity test depends on.
RenderResult renderOffline(const Instrument& inst, int note, double gain,
                           double seconds, double sampleRate);

// The sustained path: the note is held past its attack, decay and sustain
// ramps, then released at `holdSeconds`, exactly as a key press does.
//
// This is a separate function rather than a flag because it is the only way to
// cover a whole class of behaviour the one-shot render never reaches -- the
// sustain plateau and the release ramp off it. A per-oscillator release-time
// bug lived there unnoticed precisely because every reference render was a
// one-shot.
RenderResult renderOfflineSustained(const Instrument& inst, int note, double gain,
                                    double holdSeconds, double seconds,
                                    double sampleRate);

// 32-bit float stereo WAV.
bool writeWav(const std::string& path, const RenderResult& r, double sampleRate);

} // namespace sl
