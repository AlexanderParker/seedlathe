#pragma once
#include <cmath>
#include <cstdint>

namespace sl {

// JS ToUint32: truncate toward zero, then mod 2^32.
inline uint32_t toUint32(double v) {
    if (!std::isfinite(v)) return 0u;
    const double t = std::trunc(v);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0.0) m += 4294967296.0;
    return static_cast<uint32_t>(m);
}

// Exact port of zyn's Z.m32 (Tommy Ettinger's Mulberry32):
//
//   m32: (a) => (f = 1) => {
//     let t = (a += 0x6d2b79f5);
//     t = Math.imul(t ^ (t >>> 15), t | 1);
//     t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
//     return (((t ^ (t >>> 14)) >>> 0) / 4294967296) * f;
//   }
//
// JS holds `a` as a double, but every read of it passes through ToUint32 --
// i.e. mod 2^32 -- and addition is homomorphic over that modulus, so a uint32_t
// accumulator is bit-identical for as long as the JS double stays exact. That
// is about 2^53 / 0x6d2b79f5 ~= 4.8e6 calls; instrument generation uses a few
// hundred.
//
// Math.imul truncates to 32 bits, matching uint32_t multiplication. The
// `t + Math.imul(...)` term is a double addition in JS followed by ToInt32,
// which is again mod 2^32 and matches uint32_t wraparound.
class Mulberry32 {
public:
    explicit Mulberry32(uint32_t seed) : a_(seed) {}

    // Equivalent to r(f) in zyn. Default f = 1 gives [0, 1).
    double operator()(double f = 1.0) {
        a_ += 0x6D2B79F5u;
        uint32_t t = a_;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return static_cast<double>(t ^ (t >> 14)) / 4294967296.0 * f;
    }

private:
    uint32_t a_;
};

} // namespace sl
