#include "webaudio/WaBiquad.h"
#include <algorithm>
#include <cmath>

namespace sl {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

void WaBiquad::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
    setPassthrough();
}

void WaBiquad::reset() {
    x1_ = x2_ = y1_ = y2_ = 0.0;
}

void WaBiquad::setPassthrough() {
    b0_ = 1.0; b1_ = 0.0; b2_ = 0.0; a1_ = 0.0; a2_ = 0.0;
}

void WaBiquad::setSilence() {
    b0_ = 0.0; b1_ = 0.0; b2_ = 0.0; a1_ = 0.0; a2_ = 0.0;
}

// Port of Blink's Biquad::Set*Params. These are NOT the textbook RBJ
// formulas, and three of the differences change the sound materially:
//
//  1. For lowpass and highpass, Web Audio's Q is in DECIBELS -- Blink applies
//     pow10(Q/20) before using it. It is linear only for bandpass, peaking,
//     notch and allpass. zyn drives Q from an envelope scaled to 30, so
//     treating that as linear gets the resonance badly wrong in the mid-range.
//  2. The shelf filters ignore Q entirely: with Blink's fixed S = 1, alpha
//     reduces to 0.5*sin(w0)*sqrt(2).
//  3. Every type has its own Q == 0 and edge-frequency behaviour -- passthrough
//     for bandpass, A^2 for peaking, -1 for allpass, silence for notch. zyn's Q
//     envelope starts every note at 0, so these are hit constantly rather than
//     being corner cases.
void WaBiquad::setCoefficients(FilterType type, double freqHz, double q, double gainDb) {
    const double nyquist = sampleRate_ * 0.5;
    double nf = freqHz / nyquist;
    nf = nf < 0.0 ? 0.0 : (nf > 1.0 ? 1.0 : nf);

    const double A = std::pow(10.0, gainDb / 40.0);   // shelf and peaking only
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    auto normalise = [&]() {
        if (a0 == 0.0 || !std::isfinite(a0)) { setPassthrough(); return; }
        b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0;
        a1_ = a1 / a0; a2_ = a2 / a0;
    };

    switch (type) {
    case FilterType::Lowpass: {
        if (nf == 1.0) { setPassthrough(); return; }
        if (nf <= 0.0) { setSilence(); return; }
        const double res = std::pow(10.0, q / 20.0);       // Q is in dB here
        const double theta = kPi * nf;
        const double alpha = std::sin(theta) / (2.0 * res);
        const double cosw = std::cos(theta);
        const double beta = (1.0 - cosw) * 0.5;
        b0 = beta; b1 = 2.0 * beta; b2 = beta;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw; a2 = 1.0 - alpha;
        break;
    }
    case FilterType::Highpass: {
        if (nf == 1.0) { setSilence(); return; }
        if (nf <= 0.0) { setPassthrough(); return; }
        const double res = std::pow(10.0, q / 20.0);       // Q is in dB here
        const double theta = kPi * nf;
        const double alpha = std::sin(theta) / (2.0 * res);
        const double cosw = std::cos(theta);
        const double beta = (1.0 + cosw) * 0.5;
        b0 = beta; b1 = -2.0 * beta; b2 = beta;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw; a2 = 1.0 - alpha;
        break;
    }
    case FilterType::Bandpass: {
        const double freq = std::max(0.0, nf);
        const double qq = std::max(0.0, q);
        if (!(freq > 0.0 && freq < 1.0)) { setSilence(); return; }
        if (qq <= 0.0) { setPassthrough(); return; }       // NOT a narrow band
        const double w0 = kPi * freq;
        const double alpha = std::sin(w0) / (2.0 * qq);
        const double k = std::cos(w0);
        b0 = alpha; b1 = 0.0; b2 = -alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * k; a2 = 1.0 - alpha;
        break;
    }
    case FilterType::Lowshelf: {
        if (nf == 1.0) { b0_ = A * A; b1_ = b2_ = a1_ = a2_ = 0.0; return; }
        if (nf <= 0.0) { setPassthrough(); return; }
        const double w0 = kPi * nf;
        // Blink fixes S = 1, so this reduces to 0.5*sin(w0)*sqrt(2) and Q is
        // not involved at all.
        const double alpha = 0.5 * std::sin(w0) * std::sqrt(2.0);
        const double k = std::cos(w0);
        const double k2 = 2.0 * std::sqrt(A) * alpha;
        const double ap1 = A + 1.0, am1 = A - 1.0;
        b0 = A * (ap1 - am1 * k + k2);
        b1 = 2.0 * A * (am1 - ap1 * k);
        b2 = A * (ap1 - am1 * k - k2);
        a0 = ap1 + am1 * k + k2;
        a1 = -2.0 * (am1 + ap1 * k);
        a2 = ap1 + am1 * k - k2;
        break;
    }
    case FilterType::Highshelf: {
        if (nf == 1.0) { setPassthrough(); return; }
        if (nf <= 0.0) { b0_ = A * A; b1_ = b2_ = a1_ = a2_ = 0.0; return; }
        const double w0 = kPi * nf;
        const double alpha = 0.5 * std::sin(w0) * std::sqrt(2.0);
        const double k = std::cos(w0);
        const double k2 = 2.0 * std::sqrt(A) * alpha;
        const double ap1 = A + 1.0, am1 = A - 1.0;
        b0 = A * (ap1 + am1 * k + k2);
        b1 = -2.0 * A * (am1 + ap1 * k);
        b2 = A * (ap1 + am1 * k - k2);
        a0 = ap1 - am1 * k + k2;
        a1 = 2.0 * (am1 - ap1 * k);
        a2 = ap1 - am1 * k - k2;
        break;
    }
    case FilterType::Peaking: {
        const double qq = std::max(0.0, q);
        if (!(nf > 0.0 && nf < 1.0)) { setPassthrough(); return; }
        if (qq <= 0.0) { b0_ = A * A; b1_ = b2_ = a1_ = a2_ = 0.0; return; }
        const double w0 = kPi * nf;
        const double alpha = std::sin(w0) / (2.0 * qq);
        const double k = std::cos(w0);
        b0 = 1.0 + alpha * A; b1 = -2.0 * k; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a1 = -2.0 * k; a2 = 1.0 - alpha / A;
        break;
    }
    case FilterType::Allpass: {
        const double qq = std::max(0.0, q);
        if (!(nf > 0.0 && nf < 1.0)) { setPassthrough(); return; }
        if (qq <= 0.0) { b0_ = -1.0; b1_ = b2_ = a1_ = a2_ = 0.0; return; }
        const double w0 = kPi * nf;
        const double alpha = std::sin(w0) / (2.0 * qq);
        const double k = std::cos(w0);
        b0 = 1.0 - alpha; b1 = -2.0 * k; b2 = 1.0 + alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * k; a2 = 1.0 - alpha;
        break;
    }
    }

    normalise();
}

double WaBiquad::process(double x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_; x1_ = x;
    y2_ = y1_; y1_ = y;
    return y;
}

double WaBiquad::magnitudeAt(double freqHz) const {
    const double w = 2.0 * kPi * freqHz / sampleRate_;
    const double cw = std::cos(w), sw = std::sin(w);
    const double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);

    const double numRe = b0_ + b1_ * cw + b2_ * c2w;
    const double numIm = -(b1_ * sw + b2_ * s2w);
    const double denRe = 1.0 + a1_ * cw + a2_ * c2w;
    const double denIm = -(a1_ * sw + a2_ * s2w);

    const double num = std::sqrt(numRe * numRe + numIm * numIm);
    const double den = std::sqrt(denRe * denRe + denIm * denIm);
    return den > 0.0 ? num / den : 0.0;
}

} // namespace sl
