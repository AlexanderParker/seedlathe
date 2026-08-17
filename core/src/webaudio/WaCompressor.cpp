#include "webaudio/WaCompressor.h"
#include <algorithm>
#include <cmath>

namespace sl {
namespace {

constexpr double kPiOverTwo = 1.57079632679489661923;
constexpr double kMeteringReleaseTimeConstant = 0.325;
constexpr double kSpacingDb = 5.0;
constexpr double kSatReleaseTime = 0.0025;

// DynamicsCompressor's fixed internal parameters. Only threshold, knee, ratio,
// attack and release are exposed by DynamicsCompressorNode; these are not.
constexpr double kPreDelay = 0.006;       // seconds
constexpr double kPostGainDb = 0.0;
constexpr double kEffectBlend = 1.0;
constexpr double kReleaseZone1 = 0.09;
constexpr double kReleaseZone2 = 0.16;
constexpr double kReleaseZone3 = 0.42;
constexpr double kReleaseZone4 = 0.98;

// Blink's audio_utilities: a zero or negative linear value maps to an
// arbitrary large negative, not to -inf.
double linearToDecibels(double linear) {
    if (linear <= 0.0) return -1000.0;
    return 20.0 * std::log10(linear);
}

double decibelsToLinear(double db) { return std::pow(10.0, 0.05 * db); }

double discreteTimeConstantForSampleRate(double timeConstant, double sampleRate) {
    return 1.0 - std::exp(-1.0 / (sampleRate * timeConstant));
}

double clampTo(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

} // namespace

void WaCompressor::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    meteringReleaseK_ = discreteTimeConstantForSampleRate(kMeteringReleaseTimeConstant,
                                                          sampleRate);
    reset();
    setParams(dbThresholdParam_, dbKneeParam_, ratioParam_, attackTime_, releaseTime_);
}

void WaCompressor::reset() {
    detectorAverage_ = 0.0;
    compressorGain_ = 1.0;
    meteringGain_ = 0.0;
    maxAttackCompressionDiffDb_ = -1.0;
    delayL_.fill(0.0f);
    delayR_.fill(0.0f);
    preDelayReadIndex_ = 0;
    preDelayWriteIndex_ = 0;
    lastPreDelayFrames_ = 0xFFFFFFFFu;
    divisionCounter_ = 0;
    envelopeRate_ = 1.0;
    scaledDesiredGain_ = 0.0;
}

double WaCompressor::kneeCurve(double x, double k) const {
    // Linear up to the threshold, then a smooth exponential knee.
    if (x < linearThreshold_) return x;
    return linearThreshold_ + (1.0 - std::exp(-k * (x - linearThreshold_))) / k;
}

double WaCompressor::saturate(double x, double k) const {
    if (x < kneeThreshold_) return kneeCurve(x, k);
    const double xDb = linearToDecibels(x);
    const double yDb = ykneeThresholdDb_ + slope_ * (xDb - kneeThresholdDb_);
    return decibelsToLinear(yDb);
}

double WaCompressor::slopeAt(double x, double k) const {
    if (x < linearThreshold_) return 1.0;
    const double x2 = x * 1.001;
    const double xDb = linearToDecibels(x);
    const double x2Db = linearToDecibels(x2);
    const double yDb = linearToDecibels(kneeCurve(x, k));
    const double y2Db = linearToDecibels(kneeCurve(x2, k));
    return (y2Db - yDb) / (x2Db - xDb);
}

double WaCompressor::kAtSlope(double desiredSlope) const {
    const double xDb = dbThreshold_ + dbKnee_;
    const double x = decibelsToLinear(xDb);

    // Geometric bisection, 15 iterations, exactly as Blink does it.
    double minK = 0.1, maxK = 10000.0, k = 5.0;
    for (int i = 0; i < 15; ++i) {
        if (slopeAt(x, k) < desiredSlope) maxK = k;
        else minK = k;
        k = std::sqrt(minK * maxK);
    }
    return k;
}

double WaCompressor::updateStaticCurveParameters(double dbThreshold, double dbKnee,
                                                 double ratio) {
    if (dbThreshold != dbThreshold_ || dbKnee != dbKnee_ || ratio != ratio_) {
        dbThreshold_ = dbThreshold;
        linearThreshold_ = decibelsToLinear(dbThreshold);
        dbKnee_ = dbKnee;

        ratio_ = ratio;
        slope_ = 1.0 / ratio_;

        knee_ = kAtSlope(1.0 / ratio_);

        kneeThresholdDb_ = dbThreshold + dbKnee;
        kneeThreshold_ = decibelsToLinear(kneeThresholdDb_);
        ykneeThresholdDb_ = linearToDecibels(kneeCurve(kneeThreshold_, knee_));
    }
    return knee_;
}

void WaCompressor::setPreDelayTime(double preDelayTime) {
    unsigned preDelayFrames = static_cast<unsigned>(preDelayTime * sampleRate_);
    if (preDelayFrames > kMaxPreDelayFrames - 1) preDelayFrames = kMaxPreDelayFrames - 1;
    if (lastPreDelayFrames_ != preDelayFrames) {
        lastPreDelayFrames_ = preDelayFrames;
        delayL_.fill(0.0f);
        delayR_.fill(0.0f);
        preDelayReadIndex_ = 0;
        preDelayWriteIndex_ = preDelayFrames;
    }
}

void WaCompressor::setParams(double thresholdDb, double kneeDb, double ratio,
                             double attackS, double releaseS) {
    dbThresholdParam_ = thresholdDb;
    dbKneeParam_ = kneeDb;
    ratioParam_ = ratio;
    attackTime_ = attackS;
    releaseTime_ = releaseS;

    k_ = updateStaticCurveParameters(thresholdDb, kneeDb, ratio);

    // Makeup gain: the curve's own output at full scale, inverted, then
    // perceptually softened by ^0.6. This is why a quiet signal comes out
    // LOUDER than it went in.
    const double fullRangeGain = saturate(1.0, k_);
    const double fullRangeMakeupGain = std::pow(1.0 / fullRangeGain, 0.6);
    linearPostGain_ = decibelsToLinear(kPostGainDb) * fullRangeMakeupGain;

    const double attackTime = std::max(0.001, attackS);
    attackFrames_ = attackTime * sampleRate_;
    releaseFrames_ = sampleRate_ * releaseS;
    satReleaseFrames_ = kSatReleaseTime * sampleRate_;

    setPreDelayTime(kPreDelay);
    divisionCounter_ = 0;
}

void WaCompressor::startDivision() {
    // Fix gremlins.
    if (std::isnan(detectorAverage_)) detectorAverage_ = 1.0;
    if (std::isinf(detectorAverage_)) detectorAverage_ = 1.0;

    const double desiredGain = detectorAverage_;

    // Pre-warp so we get desiredGain back after the sin() warp below.
    scaledDesiredGain_ = std::asin(clampTo(desiredGain, -1.0, 1.0)) / kPiOverTwo;

    const bool isReleasing = scaledDesiredGain_ > compressorGain_;

    double compressionDiffDb;
    if (scaledDesiredGain_ == 0.0) {
        compressionDiffDb = isReleasing ? -1.0 : 1.0;
    } else {
        compressionDiffDb = linearToDecibels(compressorGain_ / scaledDesiredGain_);
    }

    if (isReleasing) {
        maxAttackCompressionDiffDb_ = -1.0;
        if (std::isnan(compressionDiffDb)) compressionDiffDb = -1.0;
        if (std::isinf(compressionDiffDb)) compressionDiffDb = -1.0;

        // Adaptive release: heavier compression releases faster.
        // Contain within -12 -> 0, then scale to 0 -> 3.
        double x = clampTo(compressionDiffDb, -12.0, 0.0);
        x = 0.25 * (x + 12.0);

        const double y1 = releaseFrames_ * kReleaseZone1;
        const double y2 = releaseFrames_ * kReleaseZone2;
        const double y3 = releaseFrames_ * kReleaseZone3;
        const double y4 = releaseFrames_ * kReleaseZone4;

        // 4th-order fit through (0,y1) (1,y2) (2,y3) (3,y4).
        const double a =  0.9999999999999998 * y1 + 1.8432219684323923e-16 * y2 -
                          1.9373394351676423e-16 * y3 + 8.824516011816245e-18 * y4;
        const double b = -1.5788320352845888 * y1 + 2.3305837032074286 * y2 -
                          0.9141194204840429 * y3 + 0.1623677525612032 * y4;
        const double c =  0.5334142869106424 * y1 - 1.272736789213631 * y2 +
                          0.9258856042207512 * y3 - 0.18656310191776226 * y4;
        const double d =  0.08783463138207234 * y1 - 0.1694162967925622 * y2 +
                          0.08588057951595272 * y3 - 0.00429891410546283 * y4;
        const double e = -0.042416883008123074 * y1 + 0.1115693827987602 * y2 -
                          0.09764676325265872 * y3 + 0.028494263462021576 * y4;

        const double x2 = x * x;
        const double x3 = x2 * x;
        const double x4 = x2 * x2;
        const double calcReleaseFrames = a + b * x + c * x2 + d * x3 + e * x4;

        const double dbPerFrame = kSpacingDb / calcReleaseFrames;
        envelopeRate_ = decibelsToLinear(dbPerFrame);
    } else {
        if (std::isnan(compressionDiffDb)) compressionDiffDb = 1.0;
        if (std::isinf(compressionDiffDb)) compressionDiffDb = 1.0;

        // While still attacking, rate follows the largest diff seen so far.
        if (maxAttackCompressionDiffDb_ == -1.0 ||
            maxAttackCompressionDiffDb_ < compressionDiffDb)
            maxAttackCompressionDiffDb_ = compressionDiffDb;

        const double effAttenDiffDb = std::max(0.5, maxAttackCompressionDiffDb_);
        const double x = 0.25 / effAttenDiffDb;
        envelopeRate_ = 1.0 - std::pow(x, 1.0 / attackFrames_);
    }
}

void WaCompressor::process(double inL, double inR, double& outL, double& outR) {
    if (divisionCounter_ == 0) startDivision();

    const double dryMix = 1.0 - kEffectBlend;
    const double wetMix = kEffectBlend;

    // Write the undelayed input, and compute compression from it rather than
    // from the delayed copy -- that lookahead is the point of the pre-delay.
    delayL_[preDelayWriteIndex_] = static_cast<float>(inL);
    delayR_[preDelayWriteIndex_] = static_cast<float>(inR);

    const double absL = std::abs(inL);
    const double absR = std::abs(inR);
    const double compressorInput = std::max(absL, absR);

    const double absInput = compressorInput;
    const double shapedInput = saturate(absInput, k_);
    const double attenuation = absInput <= 0.0001 ? 1.0 : shapedInput / absInput;

    double attenuationDb = -linearToDecibels(attenuation);
    attenuationDb = std::max(2.0, attenuationDb);

    const double dbPerFrame = attenuationDb / satReleaseFrames_;
    const double satReleaseRate = decibelsToLinear(dbPerFrame) - 1.0;

    const bool isRelease = attenuation > detectorAverage_;
    const double rate = isRelease ? satReleaseRate : 1.0;

    detectorAverage_ += (attenuation - detectorAverage_) * rate;
    detectorAverage_ = std::min(1.0, detectorAverage_);
    if (std::isnan(detectorAverage_)) detectorAverage_ = 1.0;
    if (std::isinf(detectorAverage_)) detectorAverage_ = 1.0;

    // Exponential approach to the desired gain.
    if (envelopeRate_ < 1.0) {
        compressorGain_ += (scaledDesiredGain_ - compressorGain_) * envelopeRate_;
    } else {
        compressorGain_ *= envelopeRate_;
        compressorGain_ = std::min(1.0, compressorGain_);
    }

    // Warp to smooth the sharp exponential transition points.
    const double postWarpCompressorGain = std::sin(kPiOverTwo * compressorGain_);

    const double totalGain = dryMix + wetMix * linearPostGain_ * postWarpCompressorGain;

    const double dbRealGain = 20.0 * std::log10(std::max(postWarpCompressorGain, 1e-12));
    if (dbRealGain < meteringGain_) meteringGain_ = dbRealGain;
    else meteringGain_ += (dbRealGain - meteringGain_) * meteringReleaseK_;

    outL = delayL_[preDelayReadIndex_] * totalGain;
    outR = delayR_[preDelayReadIndex_] * totalGain;

    preDelayReadIndex_ = (preDelayReadIndex_ + 1) & kMaxPreDelayFramesMask;
    preDelayWriteIndex_ = (preDelayWriteIndex_ + 1) & kMaxPreDelayFramesMask;

    if (++divisionCounter_ >= kNDivisionFrames) divisionCounter_ = 0;
}

} // namespace sl
