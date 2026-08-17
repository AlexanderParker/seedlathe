#pragma once
#include <array>
#include <cstddef>

namespace sl {

// Port of Blink's DynamicsCompressorKernel, the algorithm behind
// DynamicsCompressorNode.
//
// Every zyn seed passes through this on the master bus, so an error here does
// not affect one instrument, it offsets all of them. That is why this is a port
// rather than "a compressor with the same knobs": the makeup gain alone is
// about +4.8 dB at zyn's settings, and a generic compressor would miss it.
//
// zyn's settings (Z.init): threshold -12 dB, knee 6 dB, ratio 8,
// attack 3 ms, release 150 ms.
class WaCompressor {
public:
    void prepare(double sampleRate);
    void reset();

    void setParams(double thresholdDb, double kneeDb, double ratio,
                   double attackS, double releaseS);

    void process(double inL, double inR, double& outL, double& outR);

    // Metering, in dB. Negative means gain reduction.
    double reduction() const { return meteringGain_; }

private:
    // Blink's static curve helpers.
    double kneeCurve(double x, double k) const;
    double saturate(double x, double k) const;
    double slopeAt(double x, double k) const;
    double kAtSlope(double desiredSlope) const;
    double updateStaticCurveParameters(double dbThreshold, double dbKnee, double ratio);
    void setPreDelayTime(double preDelayTime);
    void startDivision();

    static constexpr unsigned kMaxPreDelayFrames = 1024;
    static constexpr unsigned kMaxPreDelayFramesMask = kMaxPreDelayFrames - 1;
    static constexpr int kNDivisionFrames = 32;

    double sampleRate_ = 48000.0;

    // Parameters.
    double dbThresholdParam_ = -12.0, dbKneeParam_ = 6.0, ratioParam_ = 8.0;
    double attackTime_ = 0.003, releaseTime_ = 0.15;

    // Cached static curve state.
    double ratio_ = 0.0, slope_ = 0.0, dbThreshold_ = 0.0, dbKnee_ = 0.0;
    double linearThreshold_ = 0.0, knee_ = 0.0;
    double kneeThresholdDb_ = 0.0, kneeThreshold_ = 0.0, ykneeThresholdDb_ = 0.0;

    // Per-division state.
    double k_ = 0.0;
    double linearPostGain_ = 1.0;
    double attackFrames_ = 0.0, releaseFrames_ = 0.0, satReleaseFrames_ = 0.0;
    double envelopeRate_ = 1.0;
    double scaledDesiredGain_ = 0.0;
    int divisionCounter_ = 0;

    // Running state.
    double detectorAverage_ = 0.0;
    double compressorGain_ = 1.0;
    double meteringGain_ = 0.0;
    double maxAttackCompressionDiffDb_ = -1.0;
    double meteringReleaseK_ = 0.0;

    std::array<float, kMaxPreDelayFrames> delayL_{};
    std::array<float, kMaxPreDelayFrames> delayR_{};
    unsigned preDelayReadIndex_ = 0;
    unsigned preDelayWriteIndex_ = 0;
    unsigned lastPreDelayFrames_ = 0xFFFFFFFFu;
};

} // namespace sl
