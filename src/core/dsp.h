// Basic DSP building blocks: level conversions, one-pole smoothing and RBJ biquads.
// Sources: R. Bristow-Johnson, Audio EQ Cookbook (biquads); handbook 10.1.
#pragma once

#include <cmath>
#include <cstring>
#include <emmintrin.h>

namespace ap {

constexpr double kPi = 3.14159265358979323846;

// Level floor used instead of -infinity in dB arithmetic.
constexpr double kFloorDb = -200.0;

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double a) { return a > 1e-10 ? 20.0 * std::log10(a) : kFloorDb; }

// BS.1770 loudness of a (channel-summed) mean square, and back.
inline double powerToLufs(double p) { return p > 1e-20 ? -0.691 + 10.0 * std::log10(p) : kFloorDb; }
inline double lufsToPower(double lufs) { return std::pow(10.0, (lufs + 0.691) / 10.0); }

// Coefficient of a one-pole smoother that reaches 63 % of a step after `seconds` at `rate` updates per second.
inline double onePoleCoeff(double seconds, double rate) {
    return seconds > 0.0 ? std::exp(-1.0 / (seconds * rate)) : 0.0;
}
// One step of the one-pole smoother with coefficient a.
inline double toward(double now, double target, double a) { return target + a * (now - target); }
// Linear ramp of at most `step` towards target.
inline double moveToward(double now, double target, double step) {
    return now < target ? (now + step < target ? now + step : target)
                        : (now - step > target ? now - step : target);
}
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Fast versions for per-sample gain computers (std::pow / log10 / tanh cost tens of cycles each, per sample).
// fastExp2: relative error < 2e-7 (round to nearest, Taylor series of 2^f on [-0.5, 0.5]).
inline double fastExp2(double x) {
    if (!(x > -1000.0)) return 0.0; // also NaN
    if (x > 1000.0) x = 1000.0;
    const int i = static_cast<int>(x < 0.0 ? x - 0.5 : x + 0.5);
    const double f = (x - i) * 0.69314718055994531; // in [-0.347, 0.347]
    const double p =
        1.0 + f * (1.0 + f * (0.5 + f * (1.0 / 6 + f * (1.0 / 24 + f * (1.0 / 120 + f * (1.0 / 720 + f / 5040))))));
    double scale;
    const unsigned long long bits = static_cast<unsigned long long>(i + 1023) << 52;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}
// fastLog2: absolute error < 1e-9 for normal x > 0; x <= 1e-300 gives -1000.
inline double fastLog2(double x) {
    if (!(x > 1e-300)) return -1000.0;
    unsigned long long bits;
    std::memcpy(&bits, &x, sizeof bits);
    int e = static_cast<int>((bits >> 52) & 0x7ff) - 1023;
    bits = (bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL; // mantissa m in [1, 2)
    double m;
    std::memcpy(&m, &bits, sizeof m);
    if (m > 1.4142135623730951) {
        m *= 0.5;
        ++e;
    }
    const double t = (m - 1.0) / (m + 1.0), t2 = t * t; // |t| <= 0.1716: ln m = 2 atanh t
    const double ln = 2.0 * t * (1.0 + t2 * (1.0 / 3 + t2 * (1.0 / 5 + t2 * (1.0 / 7 + t2 * (1.0 / 9 + t2 / 11)))));
    return e + ln * 1.4426950408889634;
}
inline double fastDbToLin(double db) { return fastExp2(db * 0.16609640474436813); } // log2(10) / 20
inline double fastPowerToLufs(double p) { return p > 1e-20 ? -0.691 + 3.0102999566398120 * fastLog2(p) : kFloorDb; }
// tanh with the error of fastExp2 (about 1e-7 absolute).
inline double fastTanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    return 1.0 - 2.0 / (fastExp2(x * 2.8853900817779268) + 1.0); // 2 / ln 2
}

struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// Transposed direct form II biquad in double precision.
struct Biquad {
    BiquadCoeffs c;
    double s1 = 0.0, s2 = 0.0;

    inline double process(double x) {
        const double y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { s1 = s2 = 0.0; }
    bool finite() const { return std::isfinite(s1) && std::isfinite(s2); }
};

// Two biquads in a row (a 4th-order filter, e.g. Linkwitz-Riley).
struct Biquad2 {
    Biquad a, b;
    inline double process(double x) { return b.process(a.process(x)); }
    void set(const BiquadCoeffs& c) { a.c = c; b.c = c; }
    void reset() { a.reset(); b.reset(); }
    bool finite() const { return a.finite() && b.finite(); }
};

// Two doubles in one SSE2 register: lane 0 = left, lane 1 = right.
using Pair = __m128d;
inline Pair makePair(double l, double r) { return _mm_set_pd(r, l); }
inline double leftOf(Pair p) { return _mm_cvtsd_f64(p); }
inline double rightOf(Pair p) { return _mm_cvtsd_f64(_mm_unpackhi_pd(p, p)); }

// The same biquad on both channels of a stereo pair. Same operations in the same order as Biquad, so each lane is
// bit-identical to a Biquad (baseline x64 has no FMA to contract them); half the instructions.
struct StereoBiquad {
    Pair b0 = _mm_set1_pd(1.0), b1 = _mm_setzero_pd(), b2 = _mm_setzero_pd(), a1 = _mm_setzero_pd(),
         a2 = _mm_setzero_pd();
    Pair s1 = _mm_setzero_pd(), s2 = _mm_setzero_pd();

    void set(const BiquadCoeffs& c) {
        b0 = _mm_set1_pd(c.b0);
        b1 = _mm_set1_pd(c.b1);
        b2 = _mm_set1_pd(c.b2);
        a1 = _mm_set1_pd(c.a1);
        a2 = _mm_set1_pd(c.a2);
    }
    inline Pair process(Pair x) {
        const Pair y = _mm_add_pd(_mm_mul_pd(b0, x), s1);
        s1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(b1, x), _mm_mul_pd(a1, y)), s2);
        s2 = _mm_sub_pd(_mm_mul_pd(b2, x), _mm_mul_pd(a2, y));
        return y;
    }
    void reset() { s1 = s2 = _mm_setzero_pd(); }
    bool finite() const {
        return std::isfinite(leftOf(s1)) && std::isfinite(rightOf(s1)) && std::isfinite(leftOf(s2)) &&
               std::isfinite(rightOf(s2));
    }
};

// Two stereo biquads in a row (4th order).
struct StereoBiquad2 {
    StereoBiquad a, b;
    inline Pair process(Pair x) { return b.process(a.process(x)); }
    void set(const BiquadCoeffs& c) {
        a.set(c);
        b.set(c);
    }
    void reset() {
        a.reset();
        b.reset();
    }
    bool finite() const { return a.finite() && b.finite(); }
};

// State-variable filter, linear trapezoidal integration (A. Simper): its coefficients may change every sample - a
// gain or frequency glides without a click. The coefficients are shared; each channel has its own SvfState.
struct SvfCoeffs {
    enum Mode { LowShelf, Bell, HighShelf, LowPass };
    double a1 = 1.0, a2 = 0.0, a3 = 0.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
    // shelves and bell: gain in dB (0 = the input exactly); q: 0.707 for shelves and low-pass
    static SvfCoeffs design(Mode mode, double fs, double hz, double gainDb, double q);
};
struct SvfState {
    double ic1 = 0.0, ic2 = 0.0;
    inline double process(const SvfCoeffs& c, double x) {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return c.m0 * x + c.m1 * v1 + c.m2 * v2;
    }
    void reset() { ic1 = ic2 = 0.0; }
    bool finite() const { return std::isfinite(ic1) && std::isfinite(ic2); }
};

namespace design {
constexpr double kButterworthQ = 0.70710678118654752;
// Band-pass with 0 dB at its centre (RBJ, constant peak gain).
BiquadCoeffs bandPass(double fs, double hz, double q);
BiquadCoeffs lowPass(double fs, double hz, double q = kButterworthQ);
BiquadCoeffs highPass(double fs, double hz, double q = kButterworthQ);
BiquadCoeffs allPass(double fs, double hz, double q = kButterworthQ);
BiquadCoeffs highShelf(double fs, double hz, double gainDb, double slope = 1.0);
// Re-rates a biquad designed at `from` Hz to `to` Hz through its analogue prototype (no pre-warping).
BiquadCoeffs rerate(const BiquadCoeffs& d, double from, double to);
// Magnitude response in dB at frequency hz.
double magnitudeDb(const BiquadCoeffs& c, double fs, double hz);
} // namespace design

// Flushes denormals (FTZ/DAZ) for the lifetime of the object and restores the thread's own mode afterwards.
class DenormalGuard {
public:
    DenormalGuard();
    ~DenormalGuard();
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
    unsigned int saved_;
};

} // namespace ap
