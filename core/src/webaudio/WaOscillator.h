#pragma once
#include "sl/Instrument.h"
#include <cstddef>
#include <vector>

namespace sl {

// Band-limited wavetable oscillator following Blink's PeriodicWave: three
// tables per octave, partials culled geometrically per range, linear
// interpolation both in phase and between adjacent ranges.
//
// This matters because Web Audio oscillators do not alias. A naive ramp or
// pulse would fold every partial above Nyquist back onto non-harmonic
// frequencies and no amount of downstream filtering would recover it.
class WaOscillator {
public:
    void prepare(double sampleRate);
    void setType(Waveform w);
    void resetPhase() { phase_ = 0.0; }
    void setPhase(double cycles) { phase_ = cycles - static_cast<int>(cycles); }

    // freqHz may change every sample: zyn drives it from pitch envelopes, pitch
    // LFOs, FM oscillators and the FM matrix all summing into one value.
    double render(double freqHz);

private:
    Waveform type_ = Waveform::Sine;
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
};

// The 2 s noise buffer zyn creates once and shares across every voice and
// instrument. Deterministic here, unlike zyn's Math.random, which is the one
// deliberate divergence -- without it no fidelity threshold could hold.
const std::vector<float>& globalNoiseBuffer(double sampleRate);

// AudioBufferSourceNode with loop = true, playing the shared noise buffer at
// unity rate. Noise oscillators ignore pitch entirely, as in zyn.
class NoiseSource {
public:
    void prepare(double sampleRate);
    double render();
    void reset() { pos_ = 0; }

private:
    const std::vector<float>* buffer_ = nullptr;
    size_t pos_ = 0;
};

} // namespace sl
