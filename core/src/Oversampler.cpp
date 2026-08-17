#include "Oversampler.h"

#include <algorithm>
#include <cmath>

namespace sl {
namespace {

constexpr int kTaps = 63;      // odd, so the group delay is a whole sample
constexpr double kBeta = 8.0;  // Kaiser beta: about 70 dB of stopband

double besselI0(double x) {
    // Series expansion. It converges quickly for the range a Kaiser window
    // needs, and pulling in a special-function library for one call would be
    // the larger cost.
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x * 0.5) / k;
        const double add = term * term;
        sum += add;
        if (add < 1e-18 * sum) break;
    }
    return sum;
}

std::vector<float> halfBandTaps() {
    std::vector<float> h(kTaps);
    const int mid = kTaps / 2;
    const double denom = besselI0(kBeta);

    double sum = 0.0;
    for (int n = 0; n < kTaps; ++n) {
        const int k = n - mid;
        // Cutoff at a quarter of the input rate: the output Nyquist.
        const double sinc = (k == 0) ? 0.5
                                     : std::sin(3.14159265358979323846 * 0.5 * k) /
                                           (3.14159265358979323846 * k);
        const double r = double(k) / double(mid);
        const double win = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom;
        const double v = sinc * win;
        h[static_cast<size_t>(n)] = static_cast<float>(v);
        sum += v;
    }
    // Unity at DC, so decimating cannot change the level.
    const float inv = static_cast<float>(1.0 / sum);
    for (float& v : h) v *= inv;
    return h;
}

} // namespace

void Decimator::prepare(int factor) {
    factor_ = (factor >= 4) ? 4 : (factor >= 2 ? 2 : 1);
    stages_.clear();
    latency_ = 0;

    const int numStages = (factor_ == 4) ? 2 : (factor_ == 2 ? 1 : 0);
    const std::vector<float> taps = numStages ? halfBandTaps() : std::vector<float>();

    for (int i = 0; i < numStages; ++i) {
        Stage s;
        s.taps = taps;
        s.hist.assign(taps.size(), 0.f);
        stages_.push_back(std::move(s));
    }

    // Each stage delays by half its length, measured at that stage's INPUT
    // rate; converting to output samples halves it again per remaining stage.
    // For 2x: 31 input samples = 15.5 output, reported as 16. For 4x the first
    // stage's 31 samples are a quarter of that at the output rate.
    if (factor_ == 2) latency_ = (kTaps / 2 + 1) / 2;
    else if (factor_ == 4) latency_ = (kTaps / 2) / 4 + (kTaps / 2 + 1) / 2;

    scratch_.clear();   // sized on the first process() call
    reset();
}

void Decimator::reset() {
    for (auto& s : stages_) {
        std::fill(s.hist.begin(), s.hist.end(), 0.f);
        s.pos = 0;
    }
}

void Decimator::process(const float* in, float* out, int frames) {
    if (frames <= 0) return;
    if (factor_ == 1 || stages_.empty()) {
        std::copy(in, in + frames, out);
        return;
    }

    // 4x runs two halvings, so the first writes into scratch. Sized here rather
    // than in prepare because the block size is not known until the host hands
    // one over, and growing it once is cheaper than capping it.
    const size_t needed = static_cast<size_t>(frames) * 2u;
    if (scratch_.size() < needed) scratch_.assign(needed, 0.f);

    const float* src = in;
    int n = frames * factor_;

    for (size_t si = 0; si < stages_.size(); ++si) {
        Stage& s = stages_[si];
        const size_t len = s.taps.size();
        const float* h = s.taps.data();
        float* hist = s.hist.data();

        // The last stage writes straight to the caller's buffer.
        float* dst = (si + 1 == stages_.size()) ? out : scratch_.data();

        const int outN = n / 2;
        for (int i = 0; i < outN; ++i) {
            // Two input samples in, one output sample out. Both are pushed
            // through the delay line; only the second position is evaluated.
            for (int k = 0; k < 2; ++k) {
                hist[s.pos] = src[i * 2 + k];
                s.pos = (s.pos + 1 == len) ? 0 : s.pos + 1;
            }
            double acc = 0.0;
            size_t idx = s.pos;
            for (size_t t = 0; t < len; ++t) {
                idx = (idx == 0) ? len - 1 : idx - 1;
                acc += double(h[t]) * double(hist[idx]);
            }
            dst[i] = static_cast<float>(acc);
        }

        src = dst;
        n = outN;
    }
}

} // namespace sl
