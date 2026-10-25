// The GUI's analysis (docs/design.md section 8): FFT against a plain DFT, spectrum levels of sines, stereo
// correlation and width of known signals.
#include "analysis.h"
#include "check.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace ap;

namespace {

const double kPi = 3.14159265358979323846;

struct Rng {
    uint64_t s = 88172645463325252ull;
    double uniform() { // [-1, 1)
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0;
    }
};

void fftAgainstDft() {
    std::printf("-- FFT against a plain DFT (64 random points)\n");
    Rng rng;
    std::vector<std::complex<double>> x(64);
    for (auto& v : x) v = {rng.uniform(), rng.uniform()};
    std::vector<std::complex<double>> y = x;
    fft(y);
    double worst = 0;
    for (size_t k = 0; k < x.size(); ++k) {
        std::complex<double> s = 0;
        for (size_t n = 0; n < x.size(); ++n)
            s += x[n] * std::polar(1.0, -2.0 * kPi * static_cast<double>(k * n) / static_cast<double>(x.size()));
        worst = std::max(worst, std::abs(s - y[k]));
    }
    std::printf("   largest difference %.3g\n", worst);
    CHECK(worst < 1e-12);
}

// Feeds `seconds` of a signal and returns the analyzer.
template <class F>
SpectrumAnalyzer spectrumOf(double fs, double seconds, F f) {
    SpectrumAnalyzer a;
    a.reset(fs);
    std::vector<float> x(static_cast<size_t>(seconds * fs));
    for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(f(static_cast<double>(i) / fs));
    a.feed(x.data(), static_cast<int>(x.size()));
    return a;
}

// Highest point and its frequency.
double peak(const SpectrumAnalyzer& a, double& hz) {
    double best = -1e9;
    for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i)
        if (a.points()[static_cast<size_t>(i)] > best) {
            best = a.points()[static_cast<size_t>(i)];
            hz = SpectrumAnalyzer::pointHz(i);
        }
    return best;
}

double at(const SpectrumAnalyzer& a, double hz) {
    int best = 0;
    for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i)
        if (std::fabs(std::log(SpectrumAnalyzer::pointHz(i) / hz)) < std::fabs(std::log(SpectrumAnalyzer::pointHz(best) / hz)))
            best = i;
    return a.points()[static_cast<size_t>(best)];
}

void spectrum() {
    std::printf("-- spectrum: a sine of amplitude A reads 20 log10(A) at its frequency\n");
    const double fs = 48000;
    const double onBin = 85.0 * fs / SpectrumAnalyzer::kSize; // 996.1 Hz, centred on a bin
    {
        const SpectrumAnalyzer a = spectrumOf(fs, 2.0, [&](double t) { return 0.5 * std::sin(2 * kPi * onBin * t); });
        double hz = 0;
        const double p = peak(a, hz);
        std::printf("   %.1f Hz at -6.02 dB: peak %.3f dB at %.0f Hz; at 5 kHz %.1f dB\n", onBin, p, hz, at(a, 5000));
        CHECK_NEAR(p, -6.02, 0.05);
        CHECK(std::fabs(hz / onBin - 1.0) < 0.03);
        CHECK(at(a, 5000) < -90.0);
    }
    for (double f : {50.0, 1000.0, 10000.0, 18000.0}) {
        const SpectrumAnalyzer a = spectrumOf(fs, 2.0, [&](double t) { return 0.1 * std::sin(2 * kPi * f * t); });
        double hz = 0;
        const double p = peak(a, hz);
        std::printf("   %5.0f Hz at -20 dB: peak %.2f dB at %.0f Hz\n", f, p, hz);
        CHECK_RANGE(p, -21.5, -19.9); // up to 1.42 dB less between two bins (Hann)
        CHECK(std::fabs(hz - f) < std::max(0.05 * f, fs / SpectrumAnalyzer::kSize)); // within a bin (11.7 Hz)
    }
    const SpectrumAnalyzer s = spectrumOf(fs, 1.0, [](double) { return 0.0; });
    std::printf("   silence: %.0f dB\n", s.points()[100]);
    CHECK(s.points()[100] <= SpectrumAnalyzer::kFloorDb);
    const SpectrumAnalyzer early = spectrumOf(fs, 0.05, [](double t) { return std::sin(2 * kPi * 1000 * t); });
    CHECK(early.frames() == 0); // less than one frame: nothing shown yet
}

void stereo() {
    std::printf("-- stereo: correlation and width\n");
    const double fs = 48000;
    const int n = 48000;
    std::vector<float> l(n), r(n);
    auto run = [&](const char* what, double wantCorr, double wantWidth, double tolW) {
        StereoAnalyzer a;
        a.reset(fs);
        a.feed(l.data(), r.data(), n);
        std::printf("   %-26s correlation %+.3f, width %.1f %%, %zu dots\n", what, a.correlation(), a.widthPercent(),
                    a.dots().size());
        CHECK_NEAR(a.correlation(), wantCorr, 0.05);
        CHECK_NEAR(a.widthPercent(), wantWidth, tolW);
        CHECK(a.dots().size() == StereoAnalyzer::kDots);
    };
    for (int i = 0; i < n; ++i) l[i] = r[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 440 * i / fs));
    run("mono (L = R)", 1.0, 0.0, 0.01);
    for (int i = 0; i < n; ++i) r[i] = -l[i];
    run("opposite (L = -R)", -1.0, 999.0, 0.01);
    Rng rng;
    for (int i = 0; i < n; ++i) {
        l[i] = static_cast<float>(0.3 * rng.uniform());
        r[i] = static_cast<float>(0.3 * rng.uniform());
    }
    run("unrelated noise", 0.0, 100.0, 8.0);
    for (int i = 0; i < n; ++i) { // side half the mid: width 50 %
        const double m = 0.4 * std::sin(2 * kPi * 300 * i / fs), s = 0.2 * std::sin(2 * kPi * 300 * i / fs + 1.0);
        l[i] = static_cast<float>(m + s);
        r[i] = static_cast<float>(m - s);
    }
    StereoAnalyzer a;
    a.reset(fs);
    a.feed(l.data(), r.data(), n);
    std::printf("   side = half the mid: width %.1f %%\n", a.widthPercent());
    CHECK_NEAR(a.widthPercent(), 50.0, 1.0);
    StereoAnalyzer quiet;
    quiet.reset(fs);
    CHECK(quiet.silent() && quiet.correlation() == 0.0 && quiet.widthPercent() == 0.0);
}

} // namespace

int main() {
    fftAgainstDft();
    spectrum();
    stereo();
    return test::finish("analysis_test");
}
