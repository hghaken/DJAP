#include "dsp.h"

#include <complex>
#include <xmmintrin.h>

namespace ap {
namespace design {

namespace {
double safeHz(double fs, double hz) { return clampd(hz, 1.0, 0.49 * fs); }

BiquadCoeffs normalise(double b0, double b1, double b2, double a0, double a1, double a2) {
    BiquadCoeffs c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}
} // namespace

BiquadCoeffs lowPass(double fs, double hz, double q) {
    const double w = 2.0 * kPi * safeHz(fs, hz) / fs, cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    return normalise((1.0 - cw) / 2.0, 1.0 - cw, (1.0 - cw) / 2.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs highPass(double fs, double hz, double q) {
    const double w = 2.0 * kPi * safeHz(fs, hz) / fs, cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    return normalise((1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs allPass(double fs, double hz, double q) {
    const double w = 2.0 * kPi * safeHz(fs, hz) / fs, cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    return normalise(1.0 - alpha, -2.0 * cw, 1.0 + alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs bandPass(double fs, double hz, double q) {
    const double w = 2.0 * kPi * safeHz(fs, hz) / fs, cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    return normalise(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

BiquadCoeffs highShelf(double fs, double hz, double gainDb, double slope) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * safeHz(fs, hz) / fs, cw = std::cos(w);
    const double alpha = std::sin(w) / 2.0 * std::sqrt((a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0);
    const double sa = 2.0 * std::sqrt(a) * alpha;
    return normalise(a * ((a + 1.0) + (a - 1.0) * cw + sa), -2.0 * a * ((a - 1.0) + (a + 1.0) * cw),
                     a * ((a + 1.0) + (a - 1.0) * cw - sa), (a + 1.0) - (a - 1.0) * cw + sa,
                     2.0 * ((a - 1.0) - (a + 1.0) * cw), (a + 1.0) - (a - 1.0) * cw - sa);
}

BiquadCoeffs rerate(const BiquadCoeffs& d, double from, double to) {
    const double k = 2.0 * from, k2 = k * k, q = 2.0 * to, q2 = q * q;
    // z = (k + s) / (k - s): coefficients of 1, s, s^2 of the analogue numerator / denominator.
    auto toS = [&](double c0, double c1, double c2, double out[3]) {
        out[0] = c0 + c1 + c2;
        out[1] = (c0 - c2) * 2.0 / k;
        out[2] = (c0 - c1 + c2) / k2;
    };
    // s = q (z - 1) / (z + 1): coefficients of z^2, z, 1.
    auto toZ = [&](const double a[3], double out[3]) {
        out[0] = a[0] + a[1] * q + a[2] * q2;
        out[1] = 2.0 * (a[0] - a[2] * q2);
        out[2] = a[0] - a[1] * q + a[2] * q2;
    };
    double n[3], m[3], bz[3], az[3];
    toS(d.b0, d.b1, d.b2, n);
    toS(1.0, d.a1, d.a2, m);
    toZ(n, bz);
    toZ(m, az);
    return normalise(bz[0], bz[1], bz[2], az[0], az[1], az[2]);
}

double magnitudeDb(const BiquadCoeffs& c, double fs, double hz) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / fs);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10(std::abs(h));
}

double magnitudeDb(const SvfCoeffs& c, double fs, double hz) {
    // a1 = 1 / (1 + g (g + k)), a2 = g a1: the section's g and k back; H(s) = m0 + m1 s / D + m2 / D with
    // D = s^2 + k s + 1 and s = j tan(pi f / fs) / g
    const double g = c.a2 / c.a1, k = (1.0 / c.a1 - 1.0) / g - g;
    const std::complex<double> s(0.0, std::tan(kPi * std::min(hz, 0.4999 * fs) / fs) / g);
    const std::complex<double> d = s * s + k * s + 1.0;
    return 20.0 * std::log10(std::abs(c.m0 + c.m1 * s / d + c.m2 / d));
}

} // namespace design

SvfCoeffs SvfCoeffs::design(Mode mode, double fs, double hz, double gainDb, double q) {
    SvfCoeffs c;
    const double A = std::pow(10.0, gainDb / 40.0);
    double g = std::tan(kPi * clampd(hz, 1.0, 0.45 * fs) / fs), k = 1.0 / q;
    switch (mode) {
    case LowShelf: // DC gain A^2, 1 at high frequencies
        g /= std::sqrt(A);
        c.m0 = 1.0;
        c.m1 = k * (A - 1.0);
        c.m2 = A * A - 1.0;
        break;
    case Bell: // A^2 at the centre
        k = 1.0 / (q * A);
        c.m0 = 1.0;
        c.m1 = k * (A * A - 1.0);
        c.m2 = 0.0;
        break;
    case HighShelf: // A^2 at high frequencies
        g *= std::sqrt(A);
        c.m0 = A * A;
        c.m1 = k * (1.0 - A) * A;
        c.m2 = 1.0 - A * A;
        break;
    default: // low-pass
        c.m0 = 0.0;
        c.m1 = 0.0;
        c.m2 = 1.0;
        break;
    }
    c.a1 = 1.0 / (1.0 + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

DenormalGuard::DenormalGuard() : saved_(_mm_getcsr()) {
    _mm_setcsr(saved_ | 0x8040u); // FTZ (bit 15) and DAZ (bit 6)
}

DenormalGuard::~DenormalGuard() { _mm_setcsr(saved_); }

} // namespace ap
