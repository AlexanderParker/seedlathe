#include "OfflineRender.h"
#include "SharedFxRack.h"
#include "Voice.h"
#include "webaudio/WaCompressor.h"
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <vector>

namespace sl {

RenderResult renderOffline(const Instrument& inst, int note, double gain,
                           double seconds, double sampleRate) {
    const size_t frames = static_cast<size_t>(seconds * sampleRate);

    SharedFxRack rack;
    rack.prepare(sampleRate);

    Voice voice;
    voice.prepare(sampleRate, &rack);

    WaCompressor comp;
    comp.prepare(sampleRate);
    comp.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);   // zyn's Z.init settings

    rack.beginRender();
    // Z.play is a one-shot: full ADSR including release, then stop.
    voice.noteOn(inst, note, gain, /*sustained=*/false);
    // Offline, so impulses can be generated immediately -- Chrome's convolver
    // is ready from the first sample too.
    rack.buildPending();

    RenderResult out;
    out.left.resize(frames);
    out.right.resize(frames);

    // Same block path the plugin uses, so the fidelity test exercises the code
    // that actually ships rather than a parallel per-sample one.
    std::vector<float> bufL(SharedFxRack::kMaxBlock);
    std::vector<float> bufR(SharedFxRack::kMaxBlock);

    size_t done = 0;
    while (done < frames) {
        const int n = static_cast<int>(
            std::min<size_t>(frames - done, SharedFxRack::kMaxBlock));

        rack.beginBlock(n);
        voice.processBlock(n);
        rack.mixBlock(bufL.data(), bufR.data(), n);

        for (int i = 0; i < n; ++i) {
            // Z.masterGain is left at unity; zyn never assigns it.
            double cl = 0.0, cr = 0.0;
            comp.process(bufL[static_cast<size_t>(i)], bufR[static_cast<size_t>(i)], cl, cr);
            out.left[done + static_cast<size_t>(i)] = static_cast<float>(cl);
            out.right[done + static_cast<size_t>(i)] = static_cast<float>(cr);
        }
        done += static_cast<size_t>(n);
    }
    return out;
}

bool writeWav(const std::string& path, const RenderResult& r, double sampleRate) {
    const uint32_t n = static_cast<uint32_t>(r.left.size());
    const uint32_t dataBytes = n * 8;   // stereo float32
    const uint32_t sr = static_cast<uint32_t>(sampleRate);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    u32(16);
    u16(3);              // IEEE float
    u16(2);              // channels
    u32(sr);
    u32(sr * 8);         // byte rate
    u16(8);              // block align
    u16(32);             // bits
    f.write("data", 4);
    u32(dataBytes);

    for (uint32_t i = 0; i < n; ++i) {
        f.write(reinterpret_cast<const char*>(&r.left[i]), 4);
        f.write(reinterpret_cast<const char*>(&r.right[i]), 4);
    }
    return f.good();
}

} // namespace sl
