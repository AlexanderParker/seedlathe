#include "Analysis.h"
#include "webaudio/Fft.h"
#include <algorithm>
#include <cmath>

namespace sl {
namespace {

constexpr int kFftSize = 2048;
constexpr int kHop = 512;
constexpr int kBands = 64;
constexpr double kLowHz = 20.0;
constexpr double kFloorDb = -100.0;
constexpr double kPi = 3.14159265358979323846;

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

// Triangular mel filterbank over the FFT's positive-frequency bins.
const std::vector<std::vector<float>>& filterbank(double sampleRate) {
    static std::vector<std::vector<float>> bank;
    static double builtFor = 0.0;
    if (!bank.empty() && builtFor == sampleRate) return bank;

    const int nBins = kFftSize / 2 + 1;
    const double nyquist = sampleRate * 0.5;
    const double melLow = hzToMel(kLowHz);
    const double melHigh = hzToMel(nyquist);

    std::vector<double> edges(static_cast<size_t>(kBands) + 2);
    for (int i = 0; i < kBands + 2; ++i)
        edges[static_cast<size_t>(i)] =
            melToHz(melLow + (melHigh - melLow) * double(i) / double(kBands + 1));

    bank.assign(kBands, std::vector<float>(static_cast<size_t>(nBins), 0.0f));
    for (int b = 0; b < kBands; ++b) {
        const double lo = edges[static_cast<size_t>(b)];
        const double mid = edges[static_cast<size_t>(b) + 1];
        const double hi = edges[static_cast<size_t>(b) + 2];
        for (int k = 0; k < nBins; ++k) {
            const double f = double(k) * sampleRate / double(kFftSize);
            double w = 0.0;
            if (f >= lo && f <= mid && mid > lo) w = (f - lo) / (mid - lo);
            else if (f > mid && f <= hi && hi > mid) w = (hi - f) / (hi - mid);
            bank[static_cast<size_t>(b)][static_cast<size_t>(k)] = static_cast<float>(w);
        }
    }
    builtFor = sampleRate;
    return bank;
}

} // namespace

MelSpectrogram melSpectrogram(const std::vector<float>& signal, double sampleRate) {
    MelSpectrogram out;
    if (signal.size() < static_cast<size_t>(kFftSize)) return out;

    const auto& bank = filterbank(sampleRate);
    const Fft fft(kFftSize);
    const int nBins = kFftSize / 2 + 1;

    std::vector<double> window(static_cast<size_t>(kFftSize));
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<size_t>(i)] =
            0.5 - 0.5 * std::cos(2.0 * kPi * double(i) / double(kFftSize - 1));

    out.frames = static_cast<int>((signal.size() - static_cast<size_t>(kFftSize)) / kHop) + 1;
    out.bands = kBands;
    out.data.assign(static_cast<size_t>(out.frames) * kBands, 0.0f);

    std::vector<float> re(static_cast<size_t>(kFftSize)), im(static_cast<size_t>(kFftSize));
    std::vector<double> power(static_cast<size_t>(nBins));

    for (int f = 0; f < out.frames; ++f) {
        const size_t off = static_cast<size_t>(f) * kHop;
        for (int i = 0; i < kFftSize; ++i) {
            re[static_cast<size_t>(i)] = static_cast<float>(
                double(signal[off + static_cast<size_t>(i)]) * window[static_cast<size_t>(i)]);
            im[static_cast<size_t>(i)] = 0.0f;
        }
        fft.transform(re, im, false);
        for (int k = 0; k < nBins; ++k)
            power[static_cast<size_t>(k)] = double(re[static_cast<size_t>(k)]) * double(re[static_cast<size_t>(k)]) +
                                            double(im[static_cast<size_t>(k)]) * double(im[static_cast<size_t>(k)]);

        for (int b = 0; b < kBands; ++b) {
            double sum = 0.0;
            const auto& w = bank[static_cast<size_t>(b)];
            for (int k = 0; k < nBins; ++k) sum += power[static_cast<size_t>(k)] * double(w[static_cast<size_t>(k)]);
            double db = 10.0 * std::log10(std::max(sum, 1e-20));
            if (db < kFloorDb) db = kFloorDb;
            out.data[static_cast<size_t>(f) * kBands + static_cast<size_t>(b)] =
                static_cast<float>(db);
        }
    }
    return out;
}

double melDistanceDb(const MelSpectrogram& a, const MelSpectrogram& b) {
    const int frames = std::min(a.frames, b.frames);
    const int bands = std::min(a.bands, b.bands);
    if (frames <= 0 || bands <= 0) return 0.0;

    double sum = 0.0;
    size_t count = 0;
    for (int f = 0; f < frames; ++f) {
        for (int k = 0; k < bands; ++k) {
            sum += std::abs(double(a.at(f, k)) - double(b.at(f, k)));
            ++count;
        }
    }
    return count ? sum / double(count) : 0.0;
}

double rmsEnvelopeDistanceDb(const std::vector<float>& a, const std::vector<float>& b,
                             double /*sampleRate*/) {
    const size_t n = std::min(a.size(), b.size());
    if (n < static_cast<size_t>(kHop)) return 0.0;

    auto frameDb = [](const std::vector<float>& v, size_t off) {
        double s = 0.0;
        for (int i = 0; i < kHop; ++i) {
            const double x = v[off + static_cast<size_t>(i)];
            s += x * x;
        }
        return 10.0 * std::log10(std::max(s / kHop, 1e-20));
    };

    // The floor is relative to the loudest frame, not absolute. An absolute
    // floor makes the metric fire on inaudible tail content: one signal sitting
    // at -79 dB while the other is at -140 dB scores a 61 dB "error" that
    // nobody could hear, and it swamps the real result.
    double refPeakDb = -1000.0;
    for (size_t off = 0; off + kHop <= n; off += kHop)
        refPeakDb = std::max(refPeakDb, frameDb(b, off));
    const double floorDb = refPeakDb - 60.0;

    double sum = 0.0;
    size_t used = 0;
    for (size_t off = 0; off + kHop <= n; off += kHop) {
        const double da = frameDb(a, off);
        const double db_ = frameDb(b, off);
        if (da < floorDb && db_ < floorDb) continue;
        sum += std::abs(da - db_);
        ++used;
    }
    return used ? sum / double(used) : 0.0;
}

} // namespace sl
