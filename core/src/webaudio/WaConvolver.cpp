#include "webaudio/WaConvolver.h"
#include "webaudio/Fft.h"
#include "sl/Mulberry32.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace sl {
namespace {

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

// Segment sizes grow by 8 so that each segment's first tap equals its block
// size: 64 covers [64,512), 512 covers [512,4096), and so on. Capped at 8192
// because the frequency-domain delay line for a segment costs
// partitions * 2B floats per channel, and beyond this the memory outweighs the
// arithmetic saved.
constexpr size_t kFirstBlock = 64;
constexpr size_t kGrowth = 8;
constexpr size_t kMaxBlock = 8192;

} // namespace

void WaConvolver::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    ready_ = false;
}

std::vector<float> makeReverbImpulse(double duration, double decay,
                                     uint32_t rngSeed, double sampleRate) {
    const size_t length = std::max<size_t>(1, static_cast<size_t>(sampleRate * duration));

    // zyn: impL[i] = impR[i] = randSample() * pow(1 - i/length, decay)
    std::vector<double> ir(length);
    Mulberry32 r(rngSeed);
    for (size_t i = 0; i < length; ++i) {
        const double env = std::pow(1.0 - double(i) / double(length), decay);
        ir[i] = (r(2.0) - 1.0) * env;
    }

    // ConvolverNode normalises its impulse response unless `normalize` is set
    // false, and zyn never touches it. Skipping this makes every reverb about
    // sqrt(length) too loud. Blink's Reverb::CalculateNormalizationScale:
    //
    //   power = sqrt(sum(x^2) / (channels * length))   [>= kMinPower]
    //   scale = (1 / power) * 10^(kGainCalibration/20) * (44100 / sampleRate)
    //
    // zyn writes identical left and right channels, so the two-channel power
    // reduces to one channel's RMS.
    constexpr double kGainCalibration = -58.0;
    constexpr double kGainCalibrationSampleRate = 44100.0;
    constexpr double kMinPower = 0.000125;

    double sumSq = 0.0;
    for (double v : ir) sumSq += v * v;
    double power = std::sqrt(sumSq / double(length));
    if (!std::isfinite(power) || power < kMinPower) power = kMinPower;

    double scale = 1.0 / power;
    scale *= std::pow(10.0, kGainCalibration * 0.05);
    if (sampleRate > 0.0) scale *= kGainCalibrationSampleRate / sampleRate;

    std::vector<float> out(length);
    for (size_t i = 0; i < length; ++i) out[i] = static_cast<float>(ir[i] * scale);
    return out;
}

void WaConvolver::buildImpulse(double duration, double decay, uint32_t rngSeed) {
    const std::vector<float> ir = makeReverbImpulse(duration, decay, rngSeed, sampleRate_);
    const size_t length = ir.size();

    // Direct head: the first kDirectTaps taps, convolved in the time domain so
    // the node has no latency at all.
    const size_t directLen = std::min(kDirectTaps, length);
    directIr_.assign(kDirectTaps, 0.0f);
    for (size_t i = 0; i < directLen; ++i) directIr_[i] = ir[i];
    histL_.assign(kDirectTaps * 2, 0.0f);
    histR_.assign(kDirectTaps * 2, 0.0f);
    histPos_ = 0;

    // FFT segments over the remainder.
    segments_.clear();
    size_t firstTap = kDirectTaps;
    size_t block = kFirstBlock;
    while (firstTap < length) {
        Segment seg;
        seg.blockSize = block;

        // Each segment normally reaches to firstTap * growth, which is the next
        // segment's block size. The last one takes whatever remains.
        const size_t naturalEnd = firstTap * kGrowth;
        const size_t end = (block >= kMaxBlock) ? length : std::min(naturalEnd, length);
        seg.partitions = (end - firstTap + block - 1) / block;
        if (seg.partitions == 0) seg.partitions = 1;

        const size_t fftSize = block * 2;
        const Fft& fft = fftOfSize(fftSize);

        seg.irRe.assign(seg.partitions, std::vector<float>(fftSize, 0.0f));
        seg.irIm.assign(seg.partitions, std::vector<float>(fftSize, 0.0f));
        for (size_t p = 0; p < seg.partitions; ++p) {
            for (size_t i = 0; i < block; ++i) {
                const size_t src = firstTap + p * block + i;
                seg.irRe[p][i] = src < length ? ir[src] : 0.0f;
            }
            fft.transform(seg.irRe[p], seg.irIm[p], false);
        }

        for (int c = 0; c < 2; ++c) {
            Chan& ch = seg.ch[c];
            ch.fdlRe.assign(seg.partitions, std::vector<float>(fftSize, 0.0f));
            ch.fdlIm.assign(seg.partitions, std::vector<float>(fftSize, 0.0f));
            ch.inBlock.assign(block, 0.0f);
            ch.outBlock.assign(block, 0.0f);
            ch.overlap.assign(block, 0.0f);
            ch.fill = 0;
            ch.fdlPos = 0;
            ch.outDelay.assign(firstTap - block, 0.0f);
            ch.delayPos = 0;
        }
        seg.scratchRe.assign(fftSize, 0.0f);
        seg.scratchIm.assign(fftSize, 0.0f);
        seg.accRe.assign(fftSize, 0.0f);
        seg.accIm.assign(fftSize, 0.0f);

        segments_.push_back(std::move(seg));

        firstTap = firstTap + segments_.back().partitions * block;
        if (block < kMaxBlock) block = std::min(block * kGrowth, kMaxBlock);
    }

    irLength_ = length;
    silentSamples_ = 0;
    ready_ = true;
}

void WaConvolver::reset() {
    std::fill(histL_.begin(), histL_.end(), 0.0f);
    std::fill(histR_.begin(), histR_.end(), 0.0f);
    histPos_ = 0;
    for (auto& seg : segments_) {
        for (int c = 0; c < 2; ++c) {
            Chan& ch = seg.ch[c];
            for (auto& v : ch.fdlRe) std::fill(v.begin(), v.end(), 0.0f);
            for (auto& v : ch.fdlIm) std::fill(v.begin(), v.end(), 0.0f);
            std::fill(ch.inBlock.begin(), ch.inBlock.end(), 0.0f);
            std::fill(ch.outBlock.begin(), ch.outBlock.end(), 0.0f);
            std::fill(ch.overlap.begin(), ch.overlap.end(), 0.0f);
            std::fill(ch.outDelay.begin(), ch.outDelay.end(), 0.0f);
            ch.fill = 0;
            ch.fdlPos = 0;
            ch.delayPos = 0;
        }
    }
    silentSamples_ = 0;
}

void WaConvolver::processSegment(Segment& seg, int channel) {
    Chan& c = seg.ch[channel];
    const size_t block = seg.blockSize;
    const size_t fftSize = block * 2;
    const Fft& fft = fftOfSize(fftSize);

    auto& re = seg.scratchRe;
    auto& im = seg.scratchIm;
    std::fill(re.begin(), re.end(), 0.0f);
    std::fill(im.begin(), im.end(), 0.0f);
    for (size_t i = 0; i < block; ++i) re[i] = c.inBlock[i];
    fft.transform(re, im, false);
    c.fdlRe[c.fdlPos] = re;
    c.fdlIm[c.fdlPos] = im;

    // Only the lower half of the spectrum is computed. Input and impulse are
    // both real, so the product is conjugate-symmetric and mirroring is exact.
    auto& accRe = seg.accRe;
    auto& accIm = seg.accIm;
    const size_t half = fftSize / 2;
    std::fill(accRe.begin(), accRe.end(), 0.0f);
    std::fill(accIm.begin(), accIm.end(), 0.0f);
    float* __restrict ar = accRe.data();
    float* __restrict ai = accIm.data();
    for (size_t p = 0; p < seg.partitions; ++p) {
        const size_t idx = (c.fdlPos + seg.partitions - p) % seg.partitions;
        const float* __restrict xr = c.fdlRe[idx].data();
        const float* __restrict xi = c.fdlIm[idx].data();
        const float* __restrict hr = seg.irRe[p].data();
        const float* __restrict hi = seg.irIm[p].data();
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

    for (size_t i = 0; i < block; ++i) {
        c.outBlock[i] = accRe[i] + c.overlap[i];
        c.overlap[i] = accRe[block + i];
    }
    c.fdlPos = (c.fdlPos + 1) % seg.partitions;
}

void WaConvolver::advance() {
    const double inL = accL_, inR = accR_;
    accL_ = accR_ = 0.0;

    if (!ready_) { outL_ = 0.0; outR_ = 0.0; return; }

    // Once the input has been silent for longer than the impulse, the tail has
    // fully flushed and the output is exactly zero. Without this every reverb
    // an instrument owns keeps convolving forever after the last note.
    if (inL == 0.0 && inR == 0.0) {
        if (silentSamples_ > irLength_ + 2 * kMaxBlock) { outL_ = 0.0; outR_ = 0.0; return; }
        ++silentSamples_;
    } else {
        silentSamples_ = 0;
    }

    const float fl = static_cast<float>(inL);
    const float fr = static_cast<float>(inR);

    // Direct head. The history is stored twice so the inner loop reads a
    // contiguous run with no wraparound test.
    histL_[histPos_] = fl;
    histL_[histPos_ + kDirectTaps] = fl;
    histR_[histPos_] = fr;
    histR_[histPos_ + kDirectTaps] = fr;

    float dl = 0.0f, dr = 0.0f;
    {
        const float* __restrict h = directIr_.data();
        const float* __restrict xl = histL_.data() + histPos_ + kDirectTaps;
        const float* __restrict xr = histR_.data() + histPos_ + kDirectTaps;
        for (size_t i = 0; i < kDirectTaps; ++i) {
            dl += h[i] * xl[-static_cast<ptrdiff_t>(i)];
            dr += h[i] * xr[-static_cast<ptrdiff_t>(i)];
        }
    }
    histPos_ = (histPos_ + 1) % kDirectTaps;

    double sumL = dl, sumR = dr;

    for (auto& seg : segments_) {
        Chan& cl = seg.ch[0];
        Chan& cr = seg.ch[1];

        cl.inBlock[cl.fill] = fl;
        cr.inBlock[cr.fill] = fr;

        if (cl.outDelay.empty()) {
            sumL += cl.outBlock[cl.fill];
            sumR += cr.outBlock[cr.fill];
        } else {
            sumL += cl.outDelay[cl.delayPos];
            sumR += cr.outDelay[cr.delayPos];
            cl.outDelay[cl.delayPos] = cl.outBlock[cl.fill];
            cr.outDelay[cr.delayPos] = cr.outBlock[cr.fill];
            cl.delayPos = (cl.delayPos + 1) % cl.outDelay.size();
            cr.delayPos = cl.delayPos;
        }

        if (++cl.fill >= seg.blockSize) {
            // zyn pans every oscillator centre, so both channels almost always
            // carry identical samples. Detecting that costs blockSize compares
            // and saves a whole partition accumulation. Copying the state as
            // well as the output keeps the channels exactly in step, so this
            // stays correct if a later block genuinely differs.
            bool identical = true;
            for (size_t i = 0; i < seg.blockSize; ++i) {
                if (cl.inBlock[i] != cr.inBlock[i]) { identical = false; break; }
            }

            processSegment(seg, 0);
            if (identical) {
                const size_t justWritten =
                    cl.fdlPos == 0 ? seg.partitions - 1 : cl.fdlPos - 1;
                cr.fdlRe[justWritten] = cl.fdlRe[justWritten];
                cr.fdlIm[justWritten] = cl.fdlIm[justWritten];
                cr.overlap = cl.overlap;
                cr.outBlock = cl.outBlock;
                cr.fdlPos = cl.fdlPos;
            } else {
                processSegment(seg, 1);
            }
            cl.fill = 0;
            cr.fill = 0;
        } else {
            ++cr.fill;
        }
    }

    outL_ = sumL;
    outR_ = sumR;
}

} // namespace sl
