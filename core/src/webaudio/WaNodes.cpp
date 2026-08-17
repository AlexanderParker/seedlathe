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

// ------------------------------------------------------------ WaConvolver

void WaConvolver::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    ready_ = false;
}

void WaConvolver::buildImpulse(double duration, double decay, uint32_t rngSeed) {
    const size_t length = std::max<size_t>(1, static_cast<size_t>(sampleRate_ * duration));

    // zyn: impL[i] = impR[i] = randSample() * pow(1 - i/length, decay)
    std::vector<double> ir(length);
    Mulberry32 r(rngSeed);
    for (size_t i = 0; i < length; ++i) {
        const double env = std::pow(1.0 - double(i) / double(length), decay);
        ir[i] = (r(2.0) - 1.0) * env;
    }

    // ConvolverNode normalises its impulse response unless `normalize` is set
    // false, and zyn never touches it. Skipping this makes every reverb about
    // sqrt(length) too loud -- roughly 400x for a 3 s tail, which swamps the
    // whole master bus. Blink's Reverb::CalculateNormalizationScale:
    //
    //   power = sqrt(sum(x^2) / (channels * length))   [>= kMinPower]
    //   scale = (1 / power) * 10^(kGainCalibration/20) * (44100 / sampleRate)
    //
    // zyn writes identical left and right channels, so the two-channel power
    // reduces to one channel's RMS.
    {
        constexpr double kGainCalibration = -58.0;
        constexpr double kGainCalibrationSampleRate = 44100.0;
        constexpr double kMinPower = 0.000125;

        double sumSq = 0.0;
        for (double v : ir) sumSq += v * v;
        double power = std::sqrt(sumSq / double(length));
        if (!std::isfinite(power) || power < kMinPower) power = kMinPower;

        double scale = 1.0 / power;
        scale *= std::pow(10.0, kGainCalibration * 0.05);
        if (sampleRate_ > 0.0) scale *= kGainCalibrationSampleRate / sampleRate_;

        for (double& v : ir) v *= scale;
    }

    blockSize_ = 256;
    const size_t fftSize = blockSize_ * 2;
    partitions_ = (length + blockSize_ - 1) / blockSize_;

    const Fft& fft = fftOfSize(fftSize);
    irRe_.assign(partitions_, std::vector<float>(fftSize, 0.0f));
    irIm_.assign(partitions_, std::vector<float>(fftSize, 0.0f));
    for (size_t p = 0; p < partitions_; ++p) {
        for (size_t i = 0; i < blockSize_; ++i) {
            const size_t src = p * blockSize_ + i;
            irRe_[p][i] = src < length ? static_cast<float>(ir[src]) : 0.0f;
        }
        fft.transform(irRe_[p], irIm_[p], false);
    }

    for (Channel* c : {&left_, &right_}) {
        c->fdlRe.assign(partitions_, std::vector<float>(fftSize, 0.0f));
        c->fdlIm.assign(partitions_, std::vector<float>(fftSize, 0.0f));
        c->overlap.assign(blockSize_, 0.0f);
        c->inBlock.assign(blockSize_, 0.0f);
        c->outBlock.assign(blockSize_, 0.0f);
        c->fill = 0;
        c->fdlPos = 0;
        c->outPos = 0;
    }

    scratchRe_.assign(fftSize, 0.0f);
    scratchIm_.assign(fftSize, 0.0f);
    accRe_.assign(fftSize, 0.0f);
    accIm_.assign(fftSize, 0.0f);

    irLength_ = length;
    silentBlocks_ = 0;

    ready_ = true;
}

void WaConvolver::reset() {
    for (Channel* c : {&left_, &right_}) {
        for (auto& v : c->fdlRe) std::fill(v.begin(), v.end(), 0.0f);
        for (auto& v : c->fdlIm) std::fill(v.begin(), v.end(), 0.0f);
        std::fill(c->overlap.begin(), c->overlap.end(), 0.0f);
        std::fill(c->inBlock.begin(), c->inBlock.end(), 0.0f);
        std::fill(c->outBlock.begin(), c->outBlock.end(), 0.0f);
        c->fill = 0;
        c->fdlPos = 0;
        c->outPos = 0;
    }
}

void WaConvolver::processBlock(Channel& c) {
    const size_t fftSize = blockSize_ * 2;
    const Fft& fft = fftOfSize(fftSize);

    // Newest input block into the frequency-domain delay line. All four
    // buffers are members: constructing them here cost 16 allocations per
    // audio callback, which tests/test_realtime.cpp now forbids.
    auto& re = scratchRe_;
    auto& im = scratchIm_;
    std::fill(re.begin(), re.end(), 0.0f);
    std::fill(im.begin(), im.end(), 0.0f);
    for (size_t i = 0; i < blockSize_; ++i) re[i] = c.inBlock[i];
    fft.transform(re, im, false);
    c.fdlRe[c.fdlPos] = re;
    c.fdlIm[c.fdlPos] = im;

    // Accumulate sum over partitions of input[k - p] * ir[p].
    //
    // Only the lower half of the spectrum is computed. The input block and the
    // impulse response are both real, so their spectra are conjugate-symmetric
    // and so is the product: Y[N-k] = conj(Y[k]). Mirroring afterwards is exact
    // and halves the hot loop, which is where nearly all of this node's time
    // goes.
    auto& accRe = accRe_;
    auto& accIm = accIm_;
    const size_t half = fftSize / 2;
    std::fill(accRe.begin(), accRe.end(), 0.0f);
    std::fill(accIm.begin(), accIm.end(), 0.0f);
    float* __restrict ar = accRe.data();
    float* __restrict ai = accIm.data();
    for (size_t p = 0; p < partitions_; ++p) {
        const size_t idx = (c.fdlPos + partitions_ - p) % partitions_;
        const float* __restrict xr = c.fdlRe[idx].data();
        const float* __restrict xi = c.fdlIm[idx].data();
        const float* __restrict hr = irRe_[p].data();
        const float* __restrict hi = irIm_[p].data();
        for (size_t k = 0; k <= half; ++k) {
            ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
            ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
        }
    }
    for (size_t k = 1; k < half; ++k) {
        accRe[fftSize - k] = accRe[k];
        accIm[fftSize - k] = -accIm[k];
    }
    fft.transform(accRe, accIm, true);

    // Overlap-add.
    for (size_t i = 0; i < blockSize_; ++i) {
        c.outBlock[i] = accRe[i] + c.overlap[i];
        c.overlap[i] = accRe[blockSize_ + i];
    }

    c.fdlPos = (c.fdlPos + 1) % partitions_;
    c.outPos = 0;
}

void WaConvolver::advance() {
    const double inL = accL_, inR = accR_;
    accL_ = accR_ = 0.0;

    if (!ready_) { outL_ = 0.0; outR_ = 0.0; return; }

    left_.inBlock[left_.fill] = static_cast<float>(inL);
    right_.inBlock[right_.fill] = static_cast<float>(inR);
    outL_ = left_.outBlock[left_.fill];
    outR_ = right_.outBlock[right_.fill];

    if (++left_.fill >= blockSize_) {
        // zyn pans every oscillator centre, so both channels almost always
        // carry identical samples. Detecting that costs blockSize_ compares
        // and saves a full partition accumulation -- hundreds of thousands of
        // multiply-adds. Copying the state as well as the output keeps the two
        // channels exactly in step, so this stays correct even if a later block
        // genuinely differs.
        bool identical = true;
        bool silent = true;
        for (size_t i = 0; i < blockSize_; ++i) {
            if (left_.inBlock[i] != right_.inBlock[i]) identical = false;
            if (left_.inBlock[i] != 0.0f || right_.inBlock[i] != 0.0f) silent = false;
        }

        // Once the input has been silent for longer than the impulse, the tail
        // has fully flushed and the output is exactly zero -- so there is
        // nothing to compute. Without this, every reverb an instrument owns
        // keeps running a full convolution forever after the last note ends,
        // which for a five-reverb instrument is most of the CPU cost of sitting
        // idle.
        const size_t flushBlocks = irLength_ / blockSize_ + 2;
        silentBlocks_ = silent ? silentBlocks_ + 1 : 0;

        if (silentBlocks_ > flushBlocks) {
            std::fill(left_.outBlock.begin(), left_.outBlock.end(), 0.0f);
            std::fill(right_.outBlock.begin(), right_.outBlock.end(), 0.0f);
            left_.fill = 0;
            right_.fill = 0;
            return;
        }

        processBlock(left_);
        if (identical) {
            right_.fdlRe[left_.fdlPos == 0 ? partitions_ - 1 : left_.fdlPos - 1] =
                left_.fdlRe[left_.fdlPos == 0 ? partitions_ - 1 : left_.fdlPos - 1];
            right_.fdlIm[left_.fdlPos == 0 ? partitions_ - 1 : left_.fdlPos - 1] =
                left_.fdlIm[left_.fdlPos == 0 ? partitions_ - 1 : left_.fdlPos - 1];
            right_.overlap = left_.overlap;
            right_.outBlock = left_.outBlock;
            right_.fdlPos = left_.fdlPos;
        } else {
            processBlock(right_);
        }
        left_.fill = 0;
        right_.fill = 0;
    }
}

// -------------------------------------------------------------- WaPanner

void WaPanner::pan(double in, double p, double& l, double& r) {
    if (p < -1.0) p = -1.0;
    if (p > 1.0) p = 1.0;
    const double x = (p + 1.0) * kPi / 4.0;
    l = in * std::cos(x);
    r = in * std::sin(x);
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
