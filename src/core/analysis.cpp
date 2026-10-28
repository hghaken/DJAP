// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis.h"

#include <algorithm>
#include <cmath>

namespace ap {

namespace {
const double kPi = 3.14159265358979323846;
}

void fft(std::vector<std::complex<double>>& x) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) { // bit reversal
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double a = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(a), std::sin(a));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer() : window_(kSize), work_(kSize) {
    for (int i = 0; i < kSize; ++i) window_[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kSize);
    reset(48000.0);
}

void SpectrumAnalyzer::reset(double fs) {
    fs_ = fs > 0 ? fs : 48000.0;
    buf_.assign(kSize, 0.0f);
    power_.assign(kSize / 2 + 1, 0.0);
    pos_ = sinceFrame_ = filled_ = 0;
    points_.fill(static_cast<float>(kFloorDb));
    frames_ = 0;
}

double SpectrumAnalyzer::pointHz(int i) {
    return kLowHz * std::pow(kHighHz / kLowHz, static_cast<double>(i) / (kPoints - 1));
}

void SpectrumAnalyzer::feed(const float* x, int n) {
    for (int i = 0; i < n; ++i) {
        buf_[static_cast<size_t>(pos_)] = x[i];
        pos_ = (pos_ + 1) & (kSize - 1);
        if (filled_ < kSize) ++filled_;
        if (++sinceFrame_ >= kHop && filled_ == kSize) {
            sinceFrame_ = 0;
            frame();
        }
    }
}

void SpectrumAnalyzer::frame() {
    for (int i = 0; i < kSize; ++i)
        work_[static_cast<size_t>(i)] = buf_[static_cast<size_t>((pos_ + i) & (kSize - 1))] * window_[static_cast<size_t>(i)];
    fft(work_);
    // |X| of a sine of amplitude A through the Hann window is A N / 4 (on its bin): scaled so that it reads A^2
    const double scale = 16.0 / (static_cast<double>(kSize) * kSize);
    const double a = frames_ == 0 ? 0.0 : std::exp(-(static_cast<double>(kHop) / fs_) / 0.3);
    for (size_t k = 0; k < power_.size(); ++k) power_[k] = a * power_[k] + (1.0 - a) * std::norm(work_[k]) * scale;
    ++frames_;
    // points: the loudest bin within each point's band; between bins (low frequencies) interpolated
    const double binHz = fs_ / kSize, step = std::pow(kHighHz / kLowHz, 0.5 / (kPoints - 1));
    const int last = static_cast<int>(power_.size()) - 1;
    for (int i = 0; i < kPoints; ++i) {
        const double f = pointHz(i);
        const int lo = static_cast<int>(std::ceil(f / step / binHz)), hi = static_cast<int>(std::floor(f * step / binHz));
        double p;
        if (hi >= lo && lo <= last) {
            p = 0.0;
            for (int k = std::max(lo, 0); k <= std::min(hi, last); ++k) p = std::max(p, power_[static_cast<size_t>(k)]);
        } else {
            const double b = std::min(f / binHz, static_cast<double>(last));
            const int k = std::min(static_cast<int>(b), last - 1);
            const double t = b - k;
            p = power_[static_cast<size_t>(k)] + t * (power_[static_cast<size_t>(k) + 1] - power_[static_cast<size_t>(k)]);
        }
        points_[static_cast<size_t>(i)] =
            static_cast<float>(p > 1e-12 ? std::max(kFloorDb, 10.0 * std::log10(p)) : kFloorDb);
    }
}

void StereoAnalyzer::reset(double fs) {
    a_ = std::exp(-1.0 / (0.3 * (fs > 0 ? fs : 48000.0)));
    ll_ = rr_ = lr_ = mm_ = ss_ = 0.0;
    dotPos_ = dotCount_ = decimate_ = 0;
}

void StereoAnalyzer::feed(const float* left, const float* right, int n) {
    const double b = 1.0 - a_;
    for (int i = 0; i < n; ++i) {
        const double l = left[i], r = right[i], m = 0.5 * (l + r), s = 0.5 * (l - r);
        ll_ = a_ * ll_ + b * l * l;
        rr_ = a_ * rr_ + b * r * r;
        lr_ = a_ * lr_ + b * l * r;
        mm_ = a_ * mm_ + b * m * m;
        ss_ = a_ * ss_ + b * s * s;
        if (++decimate_ >= 2) { // 1024 dots = the last 43 ms at 48 kHz
            decimate_ = 0;
            dots_[static_cast<size_t>(dotPos_)] = {left[i], right[i]};
            dotPos_ = (dotPos_ + 1) % kDots;
            dotCount_ = std::min(kDots, dotCount_ + 1);
        }
    }
}

double StereoAnalyzer::correlation() const {
    const double d = ll_ * rr_;
    return d > 1e-20 ? std::max(-1.0, std::min(1.0, lr_ / std::sqrt(d))) : 0.0;
}

double StereoAnalyzer::widthPercent() const {
    if (mm_ < 1e-12) return ss_ < 1e-12 ? 0.0 : 999.0;
    return std::min(999.0, 100.0 * std::sqrt(ss_ / mm_));
}

std::vector<std::array<float, 2>> StereoAnalyzer::dots() const {
    std::vector<std::array<float, 2>> out;
    out.reserve(static_cast<size_t>(dotCount_));
    for (int i = 0; i < dotCount_; ++i)
        out.push_back(dots_[static_cast<size_t>((dotPos_ - dotCount_ + i + kDots) % kDots)]);
    return out;
}

} // namespace ap
