// SPDX-License-Identifier: GPL-3.0-or-later
// A deck's colour (docs/design.md 3.3): every tone section against its analog prototype, neutral = bit-exact again
// after a glide back, the loudness contour, the auto tone's correction (and its hold), the sub bass an octave down.
#include "check.h"
#include "colour.h"
#include "deckrun.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace ap;
using sig::Buffer;
using tdeck::allOff;
using tdeck::Stereo;

namespace {

const double fs = 48000.0;
const double kPiD = 3.14159265358979323846;

size_t frames(double s) { return static_cast<size_t>(s * fs + 0.5); }

// Gain in dB of a deck with values v at frequency f (a steady sine, after 0.5 s).
double gainAt(const PresetValues& v, double f) {
    const Buffer l = sig::sine(fs, f, -20, 1.0), r = sig::sine(fs, f, -20, 1.0, 90);
    const Stereo o = tdeck::run(fs, v, l, r);
    return tdeck::rmsDb(o.l, frames(0.5), frames(0.4)) - tdeck::rmsDb(l, frames(0.5), frames(0.4));
}

// Analog prototypes (RBJ), at the prewarped frequency ratio w.
double protoDb(SvfCoeffs::Mode mode, double w, double db, double q) {
    const double A = std::pow(10.0, db / 40.0);
    const std::complex<double> s(0.0, w);
    std::complex<double> h;
    if (mode == SvfCoeffs::LowShelf)
        h = A * (s * s + std::sqrt(A) / q * s + A) / (A * s * s + std::sqrt(A) / q * s + 1.0);
    else if (mode == SvfCoeffs::HighShelf)
        h = A * (A * s * s + std::sqrt(A) / q * s + 1.0) / (s * s + std::sqrt(A) / q * s + A);
    else
        h = (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0);
    return 20.0 * std::log10(std::abs(h));
}

void sections() {
    std::printf("-- tone sections against their analog prototypes\n");
    struct Case {
        const char* name;
        Param gain, freq;
        SvfCoeffs::Mode mode;
        double q, hz, db;
    };
    const Case cases[] = {
        {"bass +6 @ 100", pToneBass, pToneBassFreq, SvfCoeffs::LowShelf, 0.70710678, 100, 6},
        {"bass -9 @ 60", pToneBass, pToneBassFreq, SvfCoeffs::LowShelf, 0.70710678, 60, -9},
        {"low-mid -6 @ 350", pToneLowMid, pToneLowMidFreq, SvfCoeffs::Bell, 0.9, 350, -6},
        {"presence +4 @ 3000", pTonePresence, pTonePresenceFreq, SvfCoeffs::Bell, 0.9, 3000, 4},
        {"treble +6 @ 8000", pToneTreble, pToneTrebleFreq, SvfCoeffs::HighShelf, 0.70710678, 8000, 6},
        {"treble -6 @ 5000", pToneTreble, pToneTrebleFreq, SvfCoeffs::HighShelf, 0.70710678, 5000, -6},
    };
    for (const Case& c : cases) {
        PresetValues v = allOff();
        v[c.gain] = c.db;
        v[c.freq] = c.hz;
        double worst = 0.0;
        for (double f : {30.0, c.hz / 2, c.hz, 2 * c.hz, 1000.0, 15000.0}) {
            if (f >= 20000.0) continue;
            const double w = std::tan(kPiD * f / fs) / std::tan(kPiD * c.hz / fs);
            worst = std::max(worst, std::fabs(gainAt(v, f) - protoDb(c.mode, w, c.db, c.q)));
        }
        std::printf("   %-20s largest difference %.4f dB\n", c.name, worst);
        CHECK(worst < 0.02);
    }
    // the loudness contour at 100 %: +9 dB shelf at 100 Hz, +4 dB shelf at 10 kHz
    PresetValues v = allOff();
    v[pLoudness] = 100;
    const double g30 = gainAt(v, 30), g1k = gainAt(v, 1000), g16k = gainAt(v, 16000);
    std::printf("   loudness 100 %%: %+.2f dB at 30 Hz, %+.2f at 1 kHz, %+.2f at 16 kHz\n", g30, g1k, g16k);
    CHECK_RANGE(g30, 7.5, 9.0);
    CHECK_NEAR(g1k, 0.0, 0.3);
    CHECK_RANGE(g16k, 3.0, 4.1);
}

void neutralAgain() {
    std::printf("-- every colour on, then neutral: after the glide the output is the input, bit for bit\n");
    Buffer l = sig::beatMusic(fs, 2.0, 124, 21), r = sig::beatMusic(fs, 2.0, 124, 22);
    for (float& x : l) x *= 0.3f;
    for (float& x : r) x *= 0.3f;
    PresetValues on = allOff(), off = allOff();
    on[pToneBass] = 6;
    on[pToneLowMid] = -4;
    on[pTonePresence] = 3;
    on[pToneTreble] = 3;
    on[pSubAmount] = 50;
    on[pLoudness] = 50;
    on[pAutoAmount] = 100;
    DeckProcessor d;
    d.prepare(fs);
    DeckControl ctl;
    d.publish(makeDeckConfig(on, ctl, fs));
    d.process(nullptr, nullptr, 0);
    const int lat = d.latency();
    Buffer ol = l, orr = r;
    bool switched = false;
    for (size_t pos = 0; pos < l.size(); pos += 256) {
        if (!switched && pos >= frames(1.0)) {
            d.publish(makeDeckConfig(off, ctl, fs));
            switched = true;
        }
        const int n = static_cast<int>(std::min<size_t>(256, l.size() - pos));
        d.process(ol.data() + pos, orr.data() + pos, n);
    }
    size_t bad = 0;
    double worst = 0;
    for (size_t i = frames(1.5); i < l.size(); ++i) {
        if (std::memcmp(&ol[i], &l[i - static_cast<size_t>(lat)], 4) != 0 ||
            std::memcmp(&orr[i], &r[i - static_cast<size_t>(lat)], 4) != 0)
            ++bad;
        worst = std::max(worst, std::fabs(static_cast<double>(ol[i]) - l[i - static_cast<size_t>(lat)]));
    }
    std::printf("   %zu frames differ from 0.5 s after the change (largest difference %.3g)\n", bad, worst);
    CHECK(bad == 0);
}

// Pink-ish noise: `bassDb` lifts or cuts everything under 150 Hz.
Buffer music(double seconds, double bassDb, uint64_t seed) {
    Buffer x = sig::pinkNoise(fs, seconds, 0.1, seed);
    Biquad lo;
    lo.c = design::lowPass(fs, 150);
    const double g = std::pow(10.0, bassDb / 20.0) - 1.0;
    for (float& s : x) s = static_cast<float>(s + g * lo.process(s));
    return x;
}

void autoTone() {
    std::printf("-- auto tone: a song with 12 dB less bass gets the correction, limited and held\n");
    const double ctrlRate = fs / kControlBlock;
    PresetValues v = allOff();
    v[pAutoAmount] = 100;
    v[pAutoMax] = 6;
    v[pAutoTime] = 10;
    const ColourConfig c = makeColourConfig(v, fs, ctrlRate);
    for (double bassDb : {-12.0, -4.0, 0.0, 6.0}) {
        const Buffer x = music(40.0, bassDb, 3);
        double bal[kToneBands];
        toneBalance(x.data(), x.size(), fs, bal);
        ColourProcessor p;
        p.prepare(fs);
        p.reset(c);
        double l = 0, r = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            if (i % kControlBlock == 0) p.control(c, false, i < frames(6.0));
            p.measure(x[i]);
            l = r = x[i];
            p.process(c, l, r);
        }
        const double want = clampd(kToneReference[0] - bal[0], -6.0, 6.0);
        std::printf("   bass %+5.1f dB: balance %+.1f dB against the reference %+.1f -> correction %+.2f dB (expected %+.2f)\n",
                    bassDb, bal[0], kToneReference[0], p.correctionDb(0), want);
        CHECK_NEAR(p.correctionDb(0), want, 0.5);
        // hold: 5 s of quiet music with no bass at all, marked as held, leave the correction alone
        const double before = p.correctionDb(0);
        const Buffer thin = music(5.0, -40.0, 4);
        for (size_t i = 0; i < thin.size(); ++i) {
            if (i % kControlBlock == 0) p.control(c, true, false);
            p.measure(thin[i]);
            l = r = thin[i];
            p.process(c, l, r);
        }
        CHECK_NEAR(p.correctionDb(0), before, 1e-9);
    }
}

double goertzelDb(const Buffer& x, size_t from, size_t n, double f) {
    const double w = 2.0 * kPiD * f / fs, k = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + n; ++i) {
        const double s = x[i] + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
    const double amp = 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
    return 20.0 * std::log10(amp + 1e-12);
}

void subBass() {
    std::printf("-- sub bass: an octave under a 60 Hz bass, nothing on silence\n");
    for (double amount : {0.0, 50.0, 100.0}) {
        PresetValues v = allOff();
        v[pSubAmount] = amount;
        v[pSubFreq] = 80;
        const Buffer l = sig::sine(fs, 60, -12, 2.0), r = l;
        const Stereo o = tdeck::run(fs, v, l, r);
        const double f60 = goertzelDb(o.l, frames(1.0), frames(0.5), 60), f30 = goertzelDb(o.l, frames(1.0), frames(0.5), 30);
        std::printf("   amount %3.0f %%: 60 Hz %.1f dB, 30 Hz %.1f dB\n", amount, f60, f30);
        if (amount == 0.0) CHECK(f30 < -100.0);
        if (amount == 100.0) CHECK_RANGE(f30 - f60, -9.0, 0.0);
        if (amount == 50.0) CHECK_RANGE(f30 - f60, -15.0, -4.0);
    }
    PresetValues v = allOff();
    v[pSubAmount] = 100;
    const Buffer z(frames(1.0), 0.0f);
    const Stereo o = tdeck::run(fs, v, z, z);
    double peak = 0;
    for (float s : o.l) peak = std::max(peak, static_cast<double>(std::fabs(s)));
    std::printf("   on silence: peak %.3g\n", peak);
    CHECK(peak == 0.0);
}

// The spectrum window's EQ curve (colourCurveDb) against the processing itself: a colour with every tone section
// and the loudness contour, measured with steady sines.
void curve() {
    std::printf("-- colour curve (spectrum window) against the processing\n");
    PresetValues v = allOff();
    v[pToneBass] = 6;
    v[pToneBassFreq] = 120;
    v[pToneLowMid] = -4;
    v[pToneLowMidFreq] = 400;
    v[pTonePresence] = 3;
    v[pTonePresenceFreq] = 2500;
    v[pToneTreble] = -5;
    v[pToneTrebleFreq] = 9000;
    v[pLoudness] = 50;
    double worst = 0;
    for (double f : {40.0, 100.0, 250.0, 400.0, 1000.0, 2500.0, 6000.0, 12000.0, 18000.0}) {
        const double want = colourCurveDb(v, nullptr, fs, f), got = gainAt(v, f);
        std::printf("   %6.0f Hz: curve %+6.2f dB, processing %+6.2f dB\n", f, want, got);
        worst = std::max(worst, std::fabs(want - got));
    }
    CHECK(worst < 0.05);
    PresetValues flat = allOff();
    CHECK_NEAR(colourCurveDb(flat, nullptr, fs, 1000), 0.0, 1e-12);
    const double autoDb[4] = {3, 0, 0, 0}; // the auto tone's bass correction adds to the bass section
    flat[pToneBassFreq] = 100;
    CHECK_NEAR(colourCurveDb(flat, autoDb, fs, 20), 3.0, 0.1);
}

} // namespace
int main() {
    sections();
    neutralAgain();
    autoTone();
    subBass();
    curve();
    return test::finish("colour_test");
}
