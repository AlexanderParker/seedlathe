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

void WaBiquad::setCoefficients(FilterType type, double freqHz, double q, double gainDb) {
    const double nyquist = sampleRate_ * 0.5;

    // Blink clamps the normalised frequency to [0, 1].
    double nf = freqHz / nyquist;
    nf = nf < 0.0 ? 0.0 : (nf > 1.0 ? 1.0 : nf);

    // Degenerate endpoints. These are reached on every note, not rarely: the
    // filter envelope starts at 0 Hz and ramps to 20 kHz.
    if (nf <= 0.0) {
        switch (type) {
        case FilterType::Lowpass:
        case FilterType::Bandpass:
            setSilence();
            return;
        default:
            // Highpass, allpass, shelves and peaking all pass through at DC.
            setPassthrough();
            return;
        }
    }
    if (nf >= 1.0) {
        switch (type) {
        case FilterType::Highpass:
        case FilterType::Bandpass:
            setSilence();
            return;
        default:
            setPassthrough();
            return;
        }
    }

    const double w0 = kPi * nf;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double qClamped = std::max(q, 1e-4);
    const double alpha = sinw0 / (2.0 * qClamped);
    const double A = std::pow(10.0, gainDb / 40.0);   // shelf and peaking only

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (type) {
    case FilterType::Lowpass:
        b0 = (1.0 - cosw0) * 0.5; b1 = 1.0 - cosw0; b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    case FilterType::Highpass:
        b0 = (1.0 + cosw0) * 0.5; b1 = -(1.0 + cosw0); b2 = b0;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    case FilterType::Bandpass:                        // constant 0 dB peak gain
        b0 = alpha; b1 = 0.0; b2 = -alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    case FilterType::Lowshelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =       A * ((A + 1.0) - (A - 1.0) * cosw0 + s);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
        b2 =       A * ((A + 1.0) - (A - 1.0) * cosw0 - s);
        a0 =            (A + 1.0) + (A - 1.0) * cosw0 + s;
        a1 = -2.0 *    ((A - 1.0) + (A + 1.0) * cosw0);
        a2 =            (A + 1.0) + (A - 1.0) * cosw0 - s;
        break;
    }
    case FilterType::Highshelf: {
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 =        A * ((A + 1.0) + (A - 1.0) * cosw0 + s);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
        b2 =        A * ((A + 1.0) + (A - 1.0) * cosw0 - s);
        a0 =             (A + 1.0) - (A - 1.0) * cosw0 + s;
        a1 =  2.0 *     ((A - 1.0) - (A + 1.0) * cosw0);
        a2 =             (A + 1.0) - (A - 1.0) * cosw0 - s;
        break;
    }
    case FilterType::Peaking:
        b0 = 1.0 + alpha * A; b1 = -2.0 * cosw0; b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A; a1 = -2.0 * cosw0; a2 = 1.0 - alpha / A;
        break;
    case FilterType::Allpass:
        b0 = 1.0 - alpha; b1 = -2.0 * cosw0; b2 = 1.0 + alpha;
        a0 = 1.0 + alpha; a1 = -2.0 * cosw0; a2 = 1.0 - alpha;
        break;
    }

    if (a0 == 0.0 || !std::isfinite(a0)) { setPassthrough(); return; }
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0;
    a1_ = a1 / a0; a2_ = a2 / a0;
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
