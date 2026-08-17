#pragma once
#include <string>
#include <vector>

namespace sl {

struct WavData {
    std::vector<float> mono;      // mixed down; the search compares timbre, not width
    double sampleRate = 0.0;
    int channels = 0;
    bool ok = false;
    std::string error;
};

// Reads a RIFF/WAVE file and mixes it to mono.
//
// Handles what a user is actually likely to drop in: PCM 8/16/24/32 bit and
// IEEE float 32/64 bit, any channel count, with unknown chunks skipped rather
// than treated as an error. WAVE_FORMAT_EXTENSIBLE is read through its
// sub-format tag. Anything else is refused by name rather than half-decoded.
WavData readWavMono(const std::string& path);

// Linear resampling. Good enough for feature extraction -- the mel bands that
// the sample match compares are far coarser than the interpolation error --
// and deliberately not used anywhere in the audio path.
std::vector<float> resampleLinear(const std::vector<float>& in, double fromRate,
                                  double toRate);

} // namespace sl
