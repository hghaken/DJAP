#include "truepeak.h"

#include "dsp.h"

#include <algorithm>

namespace ap {

namespace {
// Zeroth-order modified Bessel function of the first kind (series).
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
} // namespace

void TruePeakDetector::configure(int factor, int tapsPerPhase, double kaiserBeta) {
    factor_ = std::max(1, factor);
    taps_ = std::max(4, tapsPerPhase & ~3);
    const int d = taps_ / 2;
    // rows: phase 0 (unused) + phases 1..F-1, padded with zero rows to whole groups of 4 for pushPair()
    const int rows = 1 + (factor_ - 1 + 3) / 4 * 4;
    coeffs_.assign(static_cast<size_t>(rows) * taps_, 0.0f);
    const double norm = besselI0(kaiserBeta);
    for (int p = 1; p < factor_; ++p) {
        double sum = 0.0;
        std::vector<double> w(static_cast<size_t>(taps_));
        for (int j = 0; j < taps_; ++j) {
            const double t = (d - 1 - j) + static_cast<double>(p) / factor_; // distance from the point, samples
            const double sinc = std::sin(kPi * t) / (kPi * t);
            const double r = t / d;
            const double win = r * r < 1.0 ? besselI0(kaiserBeta * std::sqrt(1.0 - r * r)) / norm : 0.0;
            w[static_cast<size_t>(j)] = sinc * win;
            sum += w[static_cast<size_t>(j)];
        }
        for (int j = 0; j < taps_; ++j)
            coeffs_[static_cast<size_t>(p) * taps_ + j] = static_cast<float>(w[static_cast<size_t>(j)] / sum);
    }
    hist_.assign(static_cast<size_t>(2 * taps_), 0.0f);
    pos_ = 0;
}

void TruePeakDetector::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    pos_ = 0;
}

} // namespace ap
