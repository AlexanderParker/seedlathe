#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

namespace sl {

// Minimal iterative radix-2 complex FFT. Exists to serve the partitioned
// convolver: a 3.1 s impulse response is ~149k taps at 48 kHz, so direct
// convolution would cost that many multiplies per sample.
class Fft {
public:
    explicit Fft(size_t n) : n_(n) {
        // Bit-reversal permutation table.
        rev_.resize(n_);
        size_t bits = 0;
        while ((size_t(1) << bits) < n_) ++bits;
        for (size_t i = 0; i < n_; ++i) {
            size_t r = 0;
            for (size_t b = 0; b < bits; ++b)
                if (i & (size_t(1) << b)) r |= size_t(1) << (bits - 1 - b);
            rev_[i] = r;
        }
        // Twiddles for each stage.
        cos_.resize(n_ / 2);
        sin_.resize(n_ / 2);
        for (size_t i = 0; i < n_ / 2; ++i) {
            const double a = -2.0 * 3.14159265358979323846 * double(i) / double(n_);
            cos_[i] = std::cos(a);
            sin_[i] = std::sin(a);
        }
    }

    size_t size() const { return n_; }

    // In-place. inverse == true conjugates the twiddles and scales by 1/n.
    void transform(std::vector<double>& re, std::vector<double>& im, bool inverse) const {
        for (size_t i = 0; i < n_; ++i) {
            const size_t j = rev_[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t half = len / 2;
            const size_t step = n_ / len;
            for (size_t i = 0; i < n_; i += len) {
                for (size_t k = 0; k < half; ++k) {
                    const double wr = cos_[k * step];
                    const double wi = inverse ? -sin_[k * step] : sin_[k * step];
                    const size_t a = i + k, b = i + k + half;
                    const double tr = re[b] * wr - im[b] * wi;
                    const double ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }
        if (inverse) {
            const double s = 1.0 / double(n_);
            for (size_t i = 0; i < n_; ++i) { re[i] *= s; im[i] *= s; }
        }
    }

private:
    size_t n_;
    std::vector<size_t> rev_;
    std::vector<double> cos_, sin_;
};

} // namespace sl
