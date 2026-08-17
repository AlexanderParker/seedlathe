#pragma once
#include <array>

namespace sl {

// zyn's `fx` type tops out at 5 oscillators.
inline constexpr int kMaxOscs = 5;

enum class Waveform { Sine, Square, Sawtooth, Triangle, Noise };
enum class FilterType { Lowpass, Highpass, Bandpass, Lowshelf, Highshelf, Peaking, Allpass };

// zyn envelope: four [time, value] pairs applied as sequential linear ramps.
struct Adsr {
    double aT = 0, aV = 0;
    double dT = 0, dV = 0;
    double sT = 0, sV = 0;
    double rT = 0, rV = 0;
};

struct Lfo {
    bool on = false;
    Waveform type = Waveform::Sine;
    double frequency = 1.0;
    double depth = 0.0;
};

struct Fm {
    bool on = false;
    Waveform type = Waveform::Sine;
    double frequency = 1.0;   // ratio, multiplied by the oscillator frequency
    double depth = 0.0;       // Hz of deviation
};

struct PitchEnv {
    bool on = false;
    double amount = 0.0;
    Adsr env{};
};

struct Dist {
    bool on = false;
    double amount = 0.0;      // drawn from the side PRNG at seed + 7777
    int oversample = 1;       // 1, 2 or 4
};

struct Delay {
    bool on = false;
    double time = 0.0;        // seconds; the generator produces at most 0.5
    double feedback = 0.0;
};

struct Verb {
    bool on = false;
    double duration = 0.0;    // seconds; the generator produces at most 3.1
    double decay = 0.0;
};

struct Osc {
    Waveform waveform = Waveform::Sine;
    Adsr adsrGain{};
    FilterType filterType = FilterType::Lowpass;
    Adsr adsrFilter{};
    // Dead in the audio path -- zyn assigns it then immediately overwrites it
    // with the adsrFilterQ envelope -- but compareInstruments still reads it,
    // so the search scorer depends on it being carried through.
    double filterQ = 0.0;
    Adsr adsrFilterQ{};
    Lfo gLfo{}, fLfo{}, pLfo{};
    Fm fm{};
    PitchEnv pEnv{};
    Dist dist{};
    int oct = 0;
    double detune = 0.0;      // SEMITONES, not cents
    Delay del{};
    Verb verb{};
};

// Fixed size, no heap: safe to generate and copy on the audio thread.
struct Instrument {
    int typeIndex = 0;        // 0..9, the seed's last digit
    int oscCount = 0;
    std::array<Osc, kMaxOscs> oscs{};
    bool hasFmMatrix = false;
    std::array<std::array<double, kMaxOscs>, kMaxOscs> fmMatrix{};
    // Designer-only; zero means "use the 0.001 s default" as zyn's render does.
    std::array<std::array<double, kMaxOscs>, kMaxOscs> fmDelays{};
};

const char* typeName(int typeIndex);

} // namespace sl
