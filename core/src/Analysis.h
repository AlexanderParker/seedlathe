#pragma once
#include <cstddef>
#include <vector>

namespace sl {

// Log-mel spectrogram used only by the fidelity test. Comparing raw samples
// would fail on an inaudible sub-sample phase shift; comparing spectra asks the
// question that actually matters -- does it sound the same.
struct MelSpectrogram {
    int frames = 0;
    int bands = 0;
    std::vector<float> data;   // frames * bands, dB with a -100 floor

    float at(int frame, int band) const {
        return data[static_cast<size_t>(frame) * static_cast<size_t>(bands) +
                    static_cast<size_t>(band)];
    }
};

// FFT 2048, hop 512, 64 mel bands from 20 Hz to Nyquist, Hann window.
MelSpectrogram melSpectrogram(const std::vector<float>& signal, double sampleRate);

// Mean absolute difference in dB across every time-frequency bin.
double melDistanceDb(const MelSpectrogram& a, const MelSpectrogram& b);

// Mean absolute difference of per-frame RMS, in dB. Frames where both signals
// are below the silence floor are skipped, so a long silent tail cannot dilute
// the score.
double rmsEnvelopeDistanceDb(const std::vector<float>& a, const std::vector<float>& b,
                             double sampleRate);

} // namespace sl
