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
// NaN gives lo (a NaN volume once stuck in the deck's glide for good)
inline double clampd(double v, double lo, double hi) { return v >= lo ? (v <= hi ? v : hi) : lo; }

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

// Oversampling around a nonlinear stage (the soft clipper, the exciter) with polyphase IIR halfbands: two chains of
// first-order allpasses in z^2 (Valenzuela and Constantinides; the design as in the public hiir library). Nearly no
// delay - about 3 frames at low frequencies for 2x, up and down together, a minimum-phase-like response - against tens
// of frames for a linear-phase FIR halfband (DJAP adds no latency but the look-ahead). Without it the clipper's and the
// exciter's overtones above the Nyquist frequency folded back as audible inharmonic tones (an outside review measured
// -16..-44 dBc on 10-15 kHz tones at 48 kHz; client, 2026-10-26).
// The first stage (base rate <-> 2x): kHalfbandCoefs = 8 with a transition of 0.02 - images and aliases 82 dB down,
// flat (+-0.00001 dB) to 0.46 x the base rate (20.3 kHz at 44.1 kHz). Later stages (2x <-> 4x, 4x <-> 8x) see a signal
// that is band-limited already: kHalfband2Coefs = 6 with a transition of 0.11, 108 dB down, about 0.85 base frames of
// delay each. 4x and 8x (client, 2026-10-27: "CPU is not a problem, make the oversampling a user's choice") take the
// 2x rest away: hard-driven 15 kHz tones still folded back about 40 dB under the tone at 2x.
constexpr int kHalfbandCoefs = 8;
constexpr int kHalfband2Coefs = 6;
const double* halfbandCoefs();  // the first stage's coefficients, computed once (design::halfband)
const double* halfband2Coefs(); // the later stages'

// One halfband stage, both channels: up = one sample in, two out; down = two in, one out.
template <int N>
struct StereoHalfband {
    Pair c[N];
    Pair ux[N], uy[N]; // upsampler: each allpass's last input and output
    Pair dx[N], dy[N]; // downsampler

    void init(const double* k) {
        for (int i = 0; i < N; ++i) c[i] = _mm_set1_pd(k[i]);
        reset();
    }
    // one sample in, the two high-rate samples out (in time order)
    inline void up(Pair x, Pair& first, Pair& second) {
        Pair a = x, b = x; // path 0: the even coefficients, path 1: the odd ones
        for (int i = 0; i < N; i += 2) {
            const Pair ta = _mm_add_pd(_mm_mul_pd(_mm_sub_pd(a, uy[i]), c[i]), ux[i]);
            ux[i] = a;
            uy[i] = ta;
            a = ta;
            const Pair tb = _mm_add_pd(_mm_mul_pd(_mm_sub_pd(b, uy[i + 1]), c[i + 1]), ux[i + 1]);
            ux[i + 1] = b;
            uy[i + 1] = tb;
            b = tb;
        }
        first = a;
        second = b;
    }
    // the two high-rate samples in (in time order), one sample out
    inline Pair down(Pair first, Pair second) {
        Pair a = second, b = first;
        for (int i = 0; i < N; i += 2) {
            const Pair ta = _mm_add_pd(_mm_mul_pd(_mm_sub_pd(a, dy[i]), c[i]), dx[i]);
            dx[i] = a;
            dy[i] = ta;
            a = ta;
            const Pair tb = _mm_add_pd(_mm_mul_pd(_mm_sub_pd(b, dy[i + 1]), c[i + 1]), dx[i + 1]);
            dx[i + 1] = b;
            dy[i + 1] = tb;
            b = tb;
        }
        return _mm_mul_pd(_mm_add_pd(a, b), _mm_set1_pd(0.5));
    }
    void reset() {
        for (int i = 0; i < N; ++i) ux[i] = uy[i] = dx[i] = dy[i] = _mm_setzero_pd();
    }
    bool finite() const {
        for (int i = 0; i < N; ++i)
            if (!std::isfinite(leftOf(uy[i])) || !std::isfinite(rightOf(uy[i])) || !std::isfinite(leftOf(dy[i])) ||
                !std::isfinite(rightOf(dy[i])))
                return false;
        return true;
    }
};

// 2x, 4x or 8x: a cascade of halfband stages. up() gives factor() samples per base-rate sample, down() takes them back.
struct StereoOversampler {
    static constexpr int kMaxFactor = 8;
    StereoHalfband<kHalfbandCoefs> s1;
    StereoHalfband<kHalfband2Coefs> s2, s3;
    int f = 2;

    StereoOversampler() {
        s1.init(halfbandCoefs());
        s2.init(halfband2Coefs());
        s3.init(halfband2Coefs());
    }
    int factor() const { return f; }
    // 2, 4 or 8 (anything else: the nearest of them); the filters start clean
    void setFactor(int factor) {
        f = factor >= 6 ? 8 : factor >= 3 ? 4 : 2;
        reset();
    }
    // one base-rate sample in, factor() samples out (in time order)
    inline void up(Pair x, Pair* out) {
        s1.up(x, out[0], out[1]);
        if (f == 2) return;
        Pair t[4];
        s2.up(out[0], t[0], t[1]);
        s2.up(out[1], t[2], t[3]);
        if (f == 4) {
            for (int i = 0; i < 4; ++i) out[i] = t[i];
            return;
        }
        for (int i = 0; i < 4; ++i) s3.up(t[i], out[2 * i], out[2 * i + 1]);
    }
    // factor() samples in (in time order), one base-rate sample out
    inline Pair down(const Pair* in) {
        if (f == 2) return s1.down(in[0], in[1]);
        Pair t[4];
        if (f == 8) {
            for (int i = 0; i < 4; ++i) t[i] = s3.down(in[2 * i], in[2 * i + 1]);
        } else {
            for (int i = 0; i < 4; ++i) t[i] = in[i];
        }
        // one after the other: the filter's state must see them in time order (in one argument list the compiler may
        // call them in any order - MSVC called the second first)
        const Pair first = s2.down(t[0], t[1]);
        const Pair second = s2.down(t[2], t[3]);
        return s1.down(first, second);
    }
    void reset() {
        s1.reset();
        s2.reset();
        s3.reset();
    }
    bool finite() const { return s1.finite() && s2.finite() && s3.finite(); }
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
// The same for a state-variable filter section (its analogue prototype at the pre-warped frequency: the SVF is that
// prototype's bilinear transform, so this is exact).
double magnitudeDb(const SvfCoeffs& c, double fs, double hz);
// The allpass coefficients of a polyphase IIR halfband: `count` of them for a transition band of `transition` (as a
// fraction of the oversampled rate, 0..0.5), written to `out`. Elliptic design, closed form.
void halfband(int count, double transition, double* out);
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
