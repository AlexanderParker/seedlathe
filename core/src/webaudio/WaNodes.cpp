#include "webaudio/WaNodes.h"
#include "webaudio/Fft.h"
#include "sl/Mulberry32.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace sl {
namespace {
constexpr double kPi = 3.14159265358979323846;

// One FFT plan per size, shared. Built off the audio thread during prepare.
const Fft& fftOfSize(size_t n) {
    static std::mutex mtx;
    static std::vector<std::unique_ptr<Fft>> plans;
    std::lock_guard<std::mutex> lock(mtx);
    for (const auto& p : plans)
        if (p->size() == n) return *p;
    plans.push_back(std::make_unique<Fft>(n));
    return *plans.back();
}
} // namespace

// ---------------------------------------------------------------- WaDelay

void WaDelay::prepare(double sampleRate, double maxSeconds) {
    sampleRate_ = sampleRate;
    const size_t n = static_cast<size_t>(sampleRate * maxSeconds) + 4;
    bufferL_.assign(n, 0.0);
    bufferR_.assign(n, 0.0);
    writePos_ = 0;
    accL_ = accR_ = outL_ = outR_ = 0.0;
}

void WaDelay::setDelayTime(double seconds) {
    delaySamples_ = seconds * sampleRate_;
    const double maxD = double(bufferL_.size()) - 2.0;
    if (delaySamples_ > maxD) delaySamples_ = maxD;
    if (delaySamples_ < 0.0) delaySamples_ = 0.0;
}

void WaDelay::reset() {
    std::fill(bufferL_.begin(), bufferL_.end(), 0.0);
    std::fill(bufferR_.begin(), bufferR_.end(), 0.0);
    writePos_ = 0;
    accL_ = accR_ = outL_ = outR_ = 0.0;
}

double WaDelay::tap(std::vector<double>& buf, double in) {
    const size_t n = buf.size();

    // Read first, then write. A DelayNode sitting inside its own feedback loop
    // always has at least one sample of delay, and reading before writing is
    // what stops a near-zero delay time becoming an infinite-gain algebraic
    // loop.
    const double readPos = double(writePos_) + double(n) - delaySamples_;
    const size_t i0 = static_cast<size_t>(readPos) % n;
    const size_t i1 = (i0 + 1) % n;
    const double frac = readPos - std::floor(readPos);
    const double out = buf[i0] + (buf[i1] - buf[i0]) * frac;

    buf[writePos_] = in + out * feedback_;
    return out;
}

void WaDelay::advance() {
    if (bufferL_.empty()) {
        outL_ = accL_; outR_ = accR_;
        accL_ = accR_ = 0.0;
        return;
    }
    outL_ = tap(bufferL_, accL_);
    outR_ = tap(bufferR_, accR_);
    accL_ = accR_ = 0.0;
    if (++writePos_ >= bufferL_.size()) writePos_ = 0;
}

// -------------------------------------------------------------- WaPanner

void WaPanner::gains(double p, double& gainL, double& gainR) {
    if (p < -1.0) p = -1.0;
    if (p > 1.0) p = 1.0;
    const double x = (p + 1.0) * kPi / 4.0;
    gainL = std::cos(x);
    gainR = std::sin(x);
}

// -------------------------------------------------------------- WaShaper

std::vector<float> getDistCurve(double k, double sampleRate) {
    const size_t n = static_cast<size_t>(sampleRate);
    std::vector<float> curve(n);
    const double deg = kPi / 180.0;
    for (size_t i = 0; i < n; ++i) {
        const double x = (double(i) * 2.0) / double(n) - 1.0;
        curve[i] = static_cast<float>(((3.0 + k) * x * 20.0 * deg) /
                                      (kPi + k * std::abs(x)));
    }
    return curve;
}

void WaShaper::setCurve(double amount, double sampleRate) {
    curve_ = getDistCurve(amount, sampleRate);
}

void WaShaper::setOversample(int factor) {
    oversample_ = (factor == 4) ? 4 : (factor == 2) ? 2 : 1;
}

double WaShaper::process(double x) {
    if (curve_.empty()) return x;
    const size_t n = curve_.size();

    // WaveShaperNode's mapping: index = (x + 1) / 2 * (n - 1), clamped, with
    // linear interpolation between neighbours.
    double pos = (x + 1.0) * 0.5 * double(n - 1);
    if (pos < 0.0) pos = 0.0;
    if (pos > double(n - 1)) pos = double(n - 1);
    const size_t i0 = static_cast<size_t>(pos);
    const size_t i1 = std::min(i0 + 1, n - 1);
    const double frac = pos - double(i0);
    return curve_[i0] + (curve_[i1] - curve_[i0]) * frac;
}

} // namespace sl
