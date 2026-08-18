#include "WavIO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace sl {
namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint32_t rd32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}
uint16_t rd16(const unsigned char* p) {
    return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

// One sample, normalised to [-1, 1]. 24-bit needs the sign extended by hand;
// everything else falls out of the integer width.
float decode(const unsigned char* p, uint16_t format, uint16_t bits) {
    if (format == kFormatFloat) {
        if (bits == 64) {
            double d;
            std::memcpy(&d, p, sizeof(d));
            return static_cast<float>(d);
        }
        float f;
        std::memcpy(&f, p, sizeof(f));
        return f;
    }
    switch (bits) {
        case 8:
            // 8-bit PCM in WAV is unsigned, unlike every other width.
            return (static_cast<float>(p[0]) - 128.f) / 128.f;
        case 16: {
            const int16_t v = static_cast<int16_t>(rd16(p));
            return static_cast<float>(v) / 32768.f;
        }
        case 24: {
            int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;   // sign-extend from 24 bits
            return static_cast<float>(v) / 8388608.f;
        }
        case 32: {
            const int32_t v = static_cast<int32_t>(rd32(p));
            return static_cast<float>(v) / 2147483648.f;
        }
        default:
            return 0.f;
    }
}

WavData fail(const char* message) {
    WavData d;
    d.error = message;
    return d;
}

} // namespace

WavData readWavMono(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("could not open the file");

    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < 44) return fail("too short to be a WAV file");
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return fail("not a RIFF/WAVE file");

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;

    // Walk the chunk list rather than assuming fmt is followed by data: real
    // files carry LIST, fact, bext and others in between.
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char* id = bytes.data() + pos;
        const uint32_t size = rd32(bytes.data() + pos + 4);
        const size_t body = pos + 8;
        if (body > bytes.size()) break;
        const size_t avail = std::min<size_t>(size, bytes.size() - body);

        if (std::memcmp(id, "fmt ", 4) == 0 && avail >= 16) {
            const unsigned char* p = bytes.data() + body;
            format = rd16(p);
            channels = rd16(p + 2);
            rate = rd32(p + 4);
            bits = rd16(p + 14);
            // EXTENSIBLE hides the real format in the first two bytes of its
            // sub-format GUID.
            if (format == kFormatExtensible && avail >= 26)
                format = rd16(p + 24);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = bytes.data() + body;
            dataSize = avail;
        }

        pos = body + size + (size & 1);   // chunks are word-aligned
    }

    if (!data || dataSize == 0) return fail("no audio data in the file");
    if (channels == 0 || rate == 0) return fail("missing or unreadable format chunk");
    // Bounded before it reaches the resampler, which scales its output length
    // by the ratio of the rates.
    if (double(rate) < kMinWavRate || double(rate) > kMaxWavRate)
        return fail("implausible sample rate in the file");
    if (format != kFormatPcm && format != kFormatFloat)
        return fail("unsupported WAV encoding (only PCM and IEEE float)");
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64)
        return fail("unsupported bit depth");
    if (format == kFormatFloat && bits != 32 && bits != 64)
        return fail("float WAV must be 32 or 64 bit");

    const size_t bytesPerSample = bits / 8u;
    const size_t frameBytes = bytesPerSample * channels;
    const size_t frames = dataSize / frameBytes;
    if (frames == 0) return fail("no complete audio frames in the file");

    WavData out;
    out.mono.resize(frames);
    const float inv = 1.f / static_cast<float>(channels);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.f;
        const unsigned char* p = data + i * frameBytes;
        for (uint16_t c = 0; c < channels; ++c)
            sum += decode(p + c * bytesPerSample, format, bits);
        out.mono[i] = sum * inv;
    }

    out.sampleRate = static_cast<double>(rate);
    out.channels = channels;
    out.ok = true;
    return out;
}

std::vector<float> resampleLinear(const std::vector<float>& in, double fromRate,
                                  double toRate) {
    if (in.empty() || fromRate <= 0.0 || toRate <= 0.0) return {};
    if (fromRate == toRate) return in;

    const double ratio = fromRate / toRate;
    const double wanted = static_cast<double>(in.size()) / ratio;

    // Belt and braces alongside the rate check in readWavMono: this is public,
    // and an extreme ratio asks for an allocation that would fail anyway. An
    // hour at 768 kHz is about 2.8 billion samples, so the cap is far above
    // anything real and still finite.
    constexpr double kMaxOut = 3.0e9;
    if (!(wanted > 0.0) || wanted > kMaxOut) return {};

    const size_t n = static_cast<size_t>(wanted);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        const double src = static_cast<double>(i) * ratio;
        const size_t i0 = static_cast<size_t>(src);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const float t = static_cast<float>(src - static_cast<double>(i0));
        out[i] = in[i0] * (1.f - t) + in[i1] * t;
    }
    return out;
}

} // namespace sl
