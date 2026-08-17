// Renders a seed to a WAV file, so a seed can be auditioned and A/B'd against
// the zyn demo page without opening a DAW.
//
//   sl_render <seed> [out.wav] [--note N] [--gain G] [--seconds S] [--rate R]

#include "OfflineRender.h"
#include "PresetIO.h"
#include "sl/InstrumentGen.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "usage: sl_render <seed> [out.wav] [--note N] [--gain G]\n"
            "                        [--seconds S] [--rate R] [--json]\n\n"
            "  seed     32-bit integer, same seed the zyn demo page uses\n"
            "  --note   0 is middle C (default 0)\n"
            "  --gain   0..1, matches the demo's volume (default 1.0)\n"
            "  --json   also print the generated instrument as JSON\n");
        return 1;
    }

    const auto seed = static_cast<uint32_t>(std::strtoull(argv[1], nullptr, 10));
    std::string out = std::string(argv[1]) + ".wav";
    int note = 0;
    double gain = 1.0, seconds = 3.0, rate = 48000.0;
    bool json = false;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--note" && i + 1 < argc) note = std::atoi(argv[++i]);
        else if (a == "--gain" && i + 1 < argc) gain = std::atof(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--json") json = true;
        else if (a.rfind("--", 0) != 0) out = a;
    }

    const auto inst = sl::generateInstrument(seed);

    std::printf("seed %u  type %s  oscillators %d%s\n", seed,
                sl::typeName(inst.typeIndex), inst.oscCount,
                inst.hasFmMatrix ? "  (FM matrix)" : "");

    if (json)
        std::printf("%s\n", sl::instrumentToJson(inst).dump(1).c_str());

    const auto r = sl::renderOffline(inst, note, gain, seconds, rate);

    double peak = 0.0;
    for (float s : r.left) {
        const double v = s < 0.0f ? -double(s) : double(s);
        if (v > peak) peak = v;
    }

    if (!sl::writeWav(out, r, rate)) {
        std::printf("failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s  (%.1fs @ %.0f Hz, peak %.4f)\n",
                out.c_str(), seconds, rate, peak);
    return 0;
}
