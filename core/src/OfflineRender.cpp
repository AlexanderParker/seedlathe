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

namespace {

// Shared body. `holdFrames` < 0 means the one-shot path, where the envelope
// carries its own release and nothing is ever let go of.
RenderResult renderCommon(const Instrument& inst, int note, double gain,
                          double seconds, double sampleRate, long long holdFrames) {
    const size_t frames = static_cast<size_t>(seconds * sampleRate);

    SharedFxRack rack;
    rack.prepare(sampleRate);

    Voice voice;
    voice.prepare(sampleRate);

    WaCompressor comp;
    comp.prepare(sampleRate);
    comp.setParams(-12.0, 6.0, 8.0, 0.003, 0.15);   // zyn's Z.init settings

    rack.beginRender();
    // Z.play is a one-shot: full ADSR including release, then stop. Z.noteOn
    // holds at the sustain level until noteOff releases it.
    const bool sustained = holdFrames >= 0;
    voice.noteOn(&rack, inst, note, gain, sustained);
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
    bool releasedYet = false;
    while (done < frames) {
        const int n = static_cast<int>(
            std::min<size_t>(frames - done, SharedFxRack::kMaxBlock));

        // Release exactly on the frame the caller asked for, which means
        // splitting the block there rather than at the next boundary.
        int limited = n;
        if (sustained && !releasedYet) {
            const long long remaining = holdFrames - static_cast<long long>(done);
            if (remaining <= 0) {
                voice.noteOff();
                releasedYet = true;
            } else if (remaining < limited) {
                limited = static_cast<int>(remaining);
            }
        }
        const int nn = limited;

        rack.beginBlock(nn);
        voice.processBlock(nn);
        rack.mixBlock(bufL.data(), bufR.data(), nn);

        for (int i = 0; i < nn; ++i) {
            // Z.masterGain is left at unity; zyn never assigns it.
            double cl = 0.0, cr = 0.0;
            comp.process(bufL[static_cast<size_t>(i)], bufR[static_cast<size_t>(i)], cl, cr);
            out.left[done + static_cast<size_t>(i)] = static_cast<float>(cl);
            out.right[done + static_cast<size_t>(i)] = static_cast<float>(cr);
        }
        done += static_cast<size_t>(nn);
    }
    return out;
}

} // namespace

RenderResult renderOffline(const Instrument& inst, int note, double gain,
                           double seconds, double sampleRate) {
    return renderCommon(inst, note, gain, seconds, sampleRate, -1);
}

RenderResult renderOfflineSustained(const Instrument& inst, int note, double gain,
                                    double holdSeconds, double seconds,
                                    double sampleRate) {
    return renderCommon(inst, note, gain, seconds, sampleRate,
                        static_cast<long long>(holdSeconds * sampleRate));
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
