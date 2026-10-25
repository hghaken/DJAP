// Loudness and true-peak measurement against EBU Tech 3341 / 3342 (cases synthesized at the test's rate,
// handbook 3.4 / 4.3) and the K-weighting re-rating.
#include "check.h"
#include "loudness.h"
#include "signals.h"
#include "truepeak.h"

using namespace ap;
using sig::Buffer;

namespace {

LoudnessMeter measure(double fs, const Buffer& b) {
    LoudnessMeter m;
    measureStereo(m, fs, b.data(), b.data(), b.size()); // the same signal in L and R
    return m;
}

Buffer steps(double fs, std::initializer_list<std::pair<double, double>> parts) { // (dBFS, seconds)
    Buffer b;
    for (auto& p : parts) sig::append(b, p.first < -200 ? sig::silence(fs, p.second) : sig::sine(fs, 1000, p.first, p.second));
    return b;
}

void ebuLoudness(double fs) {
    std::printf("-- EBU Tech 3341 loudness at %.0f Hz\n", fs);
    // case 1 / 2: steady tones
    {
        LoudnessMeter m = measure(fs, sig::sine(fs, 1000, -23, 20));
        CHECK_NEAR(m.momentary(), -23.0, 0.1);
        CHECK_NEAR(m.shortTerm(), -23.0, 0.1);
        CHECK_NEAR(m.integrated(), -23.0, 0.1);
        m = measure(fs, sig::sine(fs, 1000, -33, 20));
        CHECK_NEAR(m.integrated(), -33.0, 0.1);
        CHECK_NEAR(m.momentary(), -33.0, 0.1);
        CHECK_NEAR(m.shortTerm(), -33.0, 0.1);
    }
    // case 3: relative gate
    CHECK_NEAR(measure(fs, steps(fs, {{-36, 10}, {-23, 60}, {-36, 10}})).integrated(), -23.0, 0.1);
    // case 4: both gates
    CHECK_NEAR(measure(fs, steps(fs, {{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}})).integrated(), -23.0, 0.1);
    // case 5
    CHECK_NEAR(measure(fs, steps(fs, {{-26, 20}, {-20, 20.1}, {-26, 20}})).integrated(), -23.0, 0.1);
    // case 9: short-term after 3 s
    {
        Buffer b;
        for (int i = 0; i < 5; ++i) sig::append(b, steps(fs, {{-20, 1.34}, {-30, 1.66}}));
        CHECK_NEAR(measure(fs, b).shortTerm(), -23.0, 0.1);
    }
    // case 10: maximum short-term of a 3 s tone after 0.15 x i s silence
    for (int i = 0; i < 20; ++i) {
        Buffer b = steps(fs, {{-300, 0.15 * i}, {-23, 3.0}, {-300, 1.0}});
        CHECK_NEAR(measure(fs, b).maxShortTerm(), -23.0, 0.1);
    }
    // case 12: momentary after 1 s
    {
        Buffer b;
        for (int i = 0; i < 25; ++i) sig::append(b, steps(fs, {{-20, 0.18}, {-30, 0.22}}));
        CHECK_NEAR(measure(fs, b).momentary(), -23.0, 0.1);
    }
    // case 13: maximum momentary of a 0.4 s tone after 0.02 x i s silence
    for (int i = 0; i < 20; ++i) {
        Buffer b = steps(fs, {{-300, 0.02 * i}, {-23, 0.4}, {-300, 1.0}});
        CHECK_NEAR(measure(fs, b).maxMomentary(), -23.0, 0.1);
    }
    // silence: integrated is "no value", not 0
    CHECK(measure(fs, sig::silence(fs, 5)).integrated() <= kFloorDb);
    // EBU Tech 3342 loudness range
    CHECK_NEAR(measure(fs, steps(fs, {{-20, 20}, {-30, 20}})).loudnessRange(), 10.0, 1.0);
    CHECK_NEAR(measure(fs, steps(fs, {{-20, 20}, {-15, 20}})).loudnessRange(), 5.0, 1.0);
    // one channel only: 3 dB less (BS.1770 sums channels)
    {
        Buffer b = sig::sine(fs, 1000, -23, 10);
        LoudnessMeter m;
        measureStereo(m, fs, b.data(), nullptr, b.size());
        CHECK_NEAR(m.integrated(), -26.01, 0.1);
    }
}

void kWeighting() {
    std::printf("-- K-weighting at 44.1 kHz against the 48 kHz reference\n");
    const KWeightingCoeffs a = kWeightingCoeffs(48000.0), b = kWeightingCoeffs(44100.0);
    double worst = 0.0;
    for (double f = 20; f <= 16000; f *= 1.05) {
        const double ra = design::magnitudeDb(a.shelf, 48000, f) + design::magnitudeDb(a.highPass, 48000, f);
        const double rb = design::magnitudeDb(b.shelf, 44100, f) + design::magnitudeDb(b.highPass, 44100, f);
        worst = std::max(worst, std::fabs(ra - rb));
    }
    std::printf("   largest difference 20 Hz - 16 kHz: %.4f dB\n", worst);
    CHECK(worst < 0.005);
    // +0.691 dB at 1 kHz (why a 1 kHz sine reads its own level)
    CHECK_NEAR(design::magnitudeDb(a.shelf, 48000, 1000) + design::magnitudeDb(a.highPass, 48000, 1000), 0.691, 0.01);
}

double truePeakDb(const Buffer& b, int factor, int taps) {
    TruePeakDetector d;
    d.configure(factor, taps);
    float p = 0.0f;
    for (float x : b) p = std::max(p, d.push(x));
    for (int i = 0; i < taps; ++i) p = std::max(p, d.push(0.0f)); // flush
    return linToDb(p);
}

void ebuTruePeak(double fs, int factor, int taps) {
    std::printf("-- EBU Tech 3341 true peak at %.0f Hz, %dx / %d taps per phase\n", fs, factor, taps);
    struct Case {
        double div, amp, phase, expected;
    };
    const Case cases[] = {{4, 0.5, 0, -6.02}, {4, 0.5, 45, -6.02}, {6, 0.5, 60, -6.02}, {8, 0.5, 67.5, -6.02},
                          {4, 1.41, 45, 2.98}};
    for (const Case& c : cases) {
        Buffer b = sig::sine(fs, fs / c.div, linToDb(c.amp), 1.0, c.phase);
        sig::fadeEnds(b, fs, 0.01);
        const double tp = truePeakDb(b, factor, taps);
        std::printf("   fs/%g %.2f FFS %g deg: %.2f dBTP (sample peak %.2f dBFS)\n", c.div, c.amp, c.phase, tp,
                    linToDb(sig::peak(b)));
        CHECK_RANGE(tp, c.expected - 0.4, c.expected + 0.2);
    }
}

} // namespace

int main() {
    for (double fs : {44100.0, 48000.0}) ebuLoudness(fs);
    kWeighting();
    for (double fs : {44100.0, 48000.0}) {
        ebuTruePeak(fs, 4, 32); // the output meter's detector
        ebuTruePeak(fs, 8, 32); // the limiter's detector
    }
    return test::finish("loudness_test");
}
