// SPDX-License-Identifier: GPL-3.0-or-later
// Analysis for the GUI's graphs (docs/design.md section 8): spectrum (FFT, log-frequency points) and stereo
// statistics (correlation, width, goniometer points). Fed on the GUI thread with samples the deck copied into its
// analysis rings; nothing here runs on the audio thread.
#pragma once

#include <array>
#include <complex>
#include <vector>

namespace ap {

// In-place radix-2 FFT of n = 2^k complex values.
void fft(std::vector<std::complex<double>>& x);

// Spectrum of a mono signal: 4096-point FFT, Hann window, a new frame every 2048 samples, power averaged over about
// 0.3 s, shown at kPoints log-spaced frequencies from kLowHz to kHighHz. A sine of amplitude A reads 20 log10(A) dB.
class SpectrumAnalyzer {
public:
    static constexpr int kSize = 4096;
    static constexpr int kHop = 2048;
    static constexpr int kPoints = 256;
    static constexpr double kLowHz = 20.0, kHighHz = 20000.0;
    static constexpr double kFloorDb = -120.0;

    SpectrumAnalyzer();
    void reset(double fs);
    void feed(const float* mono, int n);
    // dB at point i (frequency pointHz(i)); kFloorDb until the first frame.
    const std::array<float, kPoints>& points() const { return points_; }
    static double pointHz(int i);
    long long frames() const { return frames_; }

private:
    void frame();

    double fs_ = 48000.0;
    std::vector<float> buf_;               // the last kSize samples (ring)
    int pos_ = 0, sinceFrame_ = 0, filled_ = 0;
    std::vector<double> window_, power_;   // Hann, averaged power per bin
    std::vector<std::complex<double>> work_;
    std::array<float, kPoints> points_{};
    long long frames_ = 0;
};

// Correlation and width of a stereo signal, averaged over about 0.3 s, and the last points for a goniometer.
class StereoAnalyzer {
public:
    static constexpr int kDots = 1024;

    void reset(double fs);
    void feed(const float* left, const float* right, int n);
    // +1 = the same on both channels (mono), 0 = unrelated, -1 = opposite; 0 when silent.
    double correlation() const;
    // side level / mid level in % (0 = mono; 100 = as much side as mid); 0 when silent.
    double widthPercent() const;
    bool silent() const { return ll_ + rr_ < 1e-10; }
    // goniometer points (left, right), oldest first.
    std::vector<std::array<float, 2>> dots() const;

private:
    double a_ = 0.99;
    double ll_ = 0, rr_ = 0, lr_ = 0, mm_ = 0, ss_ = 0;
    std::array<std::array<float, 2>, kDots> dots_{};
    int dotPos_ = 0, dotCount_ = 0, decimate_ = 0;
};

} // namespace ap
