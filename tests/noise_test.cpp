// SPDX-License-Identifier: GPL-3.0-or-later
// The noise reduction (a downward expander in 1 or 4 bands; docs/design.md 3): flat while open, noise under the
// threshold turned down by the depth, music above it untouched, its times, the bands, no clicks on changes.
#include "check.h"
#include "deckrun.h"

#include <cmath>
#include <vector>

using namespace ap;
using sig::Buffer;
using tdeck::allOff;
using tdeck::run;

namespace {

const double fs = 48000.0;

size_t frames(double s) { return static_cast<size_t>(s * fs + 0.5); }

// Only the noise reduction on; target -14 LUFS (the working level: no clipper), threshold -45 LU -> -59.
PresetValues noiseOnly() {
    PresetValues v = allOff();
    v[pTarget] = -14;
    v[pNoiseOn] = 1;
    return v;
}

Buffer sum(const Buffer& a, const Buffer& b) {
    Buffer s(a.size());
    for (size_t i = 0; i < a.size(); ++i) s[i] = a[i] + b[i];
    return s;
}

// Gain (dB) per 1 ms window: output RMS over input RMS.
std::vector<double> gainTrace(const Buffer& in, const Buffer& out) {
    const size_t w = frames(0.001);
    std::vector<double> g;
    for (size_t i = 0; i + w <= in.size(); i += w) g.push_back(tdeck::rmsDb(out, i, w) - tdeck::rmsDb(in, i, w));
    return g;
}

// Energy above `hz` (dB): the signal minus its low-passed self, by a 4th-order high-pass.
double highDb(const Buffer& b, double hz, size_t from) {
    Biquad2 hp;
    hp.set(design::highPass(fs, hz));
    double s = 0.0;
    for (size_t i = 0; i < b.size(); ++i) {
        const double y = hp.process(b[i]);
        if (i >= from) s += y * y;
    }
    return 10.0 * std::log10(s / static_cast<double>(b.size() - from) + 1e-30);
}

void flatWhileOpen() {
    std::printf("-- open: the 4 bands sum flat (tones at -20 dBFS; depth 0, and depth 12 with the bands the tone does not "
                "reach turned down), 1 band bit-exact\n");
    for (int bands : {4, 1})
    for (double depth : {0.0, 12.0}) {
        PresetValues v = noiseOnly();
        v[pNoiseBands] = bands;
        v[pNoiseDepth] = depth;
        double worst = 0.0;
        for (double hz : {40.0, 80.0, 250.0, 1000.0, 3000.0, 5000.0, 12000.0}) {
            const Buffer in = sig::sine(fs, hz, -20, 1.0);
            const tdeck::Stereo o = run(fs, v, in, in);
            const double d = 20.0 * std::log10(sig::amplitudeAt(o.l, fs, hz, frames(0.5)) /
                                               sig::amplitudeAt(in, fs, hz, frames(0.5)));
            worst = std::max(worst, std::fabs(d));
        }
        std::printf("   %d band%s, depth %2.0f: largest change %.4f dB\n", bands, bands == 1 ? " " : "s", depth, worst);
        // with depth: a closed band takes the little of the tone that leaks into it (LR4 slopes)
        CHECK(worst < (depth > 0.0 ? 0.05 : 0.001));
    }
    // 1 band, open: the gain is exactly 1, so the output is the input (delayed by the look-ahead)
    PresetValues v = noiseOnly();
    v[pNoiseBands] = 1;
    const Buffer in = sig::pinkNoise(fs, 2.0, dbToLin(-20), 7);
    const tdeck::Stereo o = run(fs, v, in, in);
    size_t same = 0;
    for (size_t i = 0; i < in.size(); ++i) same += o.l[i] == in[i];
    std::printf("   1 band on pink noise at -20 dBFS: %zu of %zu samples identical\n", same, in.size());
    CHECK(same == in.size());
}

void noiseTurnedDown() {
    std::printf("-- noise under the threshold (pink noise about -80 dBFS RMS; threshold -59, ratio 4)\n");
    for (int bands : {4, 1}) {
        for (double depth : {12.0, 30.0}) {
            PresetValues v = noiseOnly();
            v[pNoiseBands] = bands;
            v[pNoiseDepth] = depth;
            v[pNoiseRatio] = 4;
            const Buffer l = sig::pinkNoise(fs, 3.0, dbToLin(-80), 11), r = sig::pinkNoise(fs, 3.0, dbToLin(-80), 12);
            DeckProcessor d;
            d.prepare(fs);
            const tdeck::Stereo o = run(fs, v, l, r, 1.5, true, 256, &d);
            const double down = tdeck::rmsDb(o.l, frames(1.0), frames(2.0)) - tdeck::rmsDb(l, frames(1.0), frames(2.0));
            // the meter: the largest reduction of a band in the last records
            MeterRecord recs[64];
            float meter = 0.0f;
            for (size_t n; (n = d.meters().pop(recs, 64)) > 0;)
                for (size_t k = 0; k < n; ++k)
                    for (float x : recs[k].noiseDb) meter = std::max(meter, x);
            std::printf("   %d band%s, depth %2.0f dB: noise %+.2f dB, meter %.2f dB\n", bands, bands == 1 ? " " : "s",
                        depth, down, meter);
            CHECK_NEAR(down, -depth, 0.3);
            CHECK_NEAR(meter, depth, 0.1);
        }
    }
    // off: nothing changes
    PresetValues v = noiseOnly();
    v[pNoiseOn] = 0;
    const Buffer l = sig::pinkNoise(fs, 2.0, dbToLin(-75), 11);
    const tdeck::Stereo o = run(fs, v, l, l);
    CHECK_NEAR(tdeck::rmsDb(o.l, frames(1.0), frames(1.0)) - tdeck::rmsDb(l, frames(1.0), frames(1.0)), 0.0, 1e-6);
}

void musicKeepsItsBand() {
    std::printf("-- a 500 Hz tone at -20 dBFS with hiss (white, -70 dBFS peak): 4 bands keep the tone and take the hiss\n");
    // (a tone leaks into the bands next to its own through the LR4 slopes: 2 kHz at -20 dBFS reaches the band above
    // 5 kHz at about -53 and holds it open - music does that too; 500 Hz is 3.3 octaves under that split)
    const Buffer tone = sig::sine(fs, 500, -20, 3.0);
    const Buffer hissL = sig::whiteNoise(fs, 3.0, dbToLin(-70), 21), hissR = sig::whiteNoise(fs, 3.0, dbToLin(-70), 22);
    const Buffer l = sum(tone, hissL), r = sum(tone, hissR);
    for (int bands : {4, 1}) {
        PresetValues v = noiseOnly();
        v[pNoiseBands] = bands;
        v[pNoiseDepth] = 20;
        v[pNoiseRatio] = 4;
        const tdeck::Stereo o = run(fs, v, l, r);
        const double toneDb = 20.0 * std::log10(sig::amplitudeAt(o.l, fs, 500, frames(1.0)) /
                                                sig::amplitudeAt(l, fs, 500, frames(1.0)));
        const double hiss = highDb(o.l, 8000, frames(1.0)) - highDb(l, 8000, frames(1.0));
        std::printf("   %d band%s: tone %+.3f dB, hiss above 8 kHz %+.2f dB\n", bands, bands == 1 ? " " : "s", toneDb, hiss);
        CHECK(std::fabs(toneDb) < 0.05);
        if (bands == 4) CHECK(hiss < -18.0);
        else CHECK(std::fabs(hiss) < 0.5); // the tone holds the wide band open: the hiss stays
    }
}

void times() {
    std::printf("-- times, 1 band: a 3 kHz tone steps -20 -> -80 -> -20 dBFS (hold 50 ms, release 250 ms, attack 2 ms; "
                "ratio 10, depth 20)\n");
    // one band: in 4 bands the tone's leak into the band above (-19 dB through the LR4 slope) has its own timing
    PresetValues v = noiseOnly();
    v[pNoiseBands] = 1;
    v[pNoiseDepth] = 20;
    v[pNoiseRatio] = 10;
    Buffer in = sig::sine(fs, 3000, -20, 1.0);
    sig::append(in, sig::sine(fs, 3000, -80, 2.0));
    sig::append(in, sig::sine(fs, 3000, -20, 1.0));
    const tdeck::Stereo o = run(fs, v, in, in);
    const std::vector<double> g = gainTrace(in, o.l);
    // the wide band's detector (one period of 30 Hz, 33 ms) falls the 35 dB from the tone to the knee in about 270 ms;
    // then the hold (50 ms), then the release
    int under = -1;
    for (int i = 1000; i < 2000 && under < 0; ++i)
        if (g[static_cast<size_t>(i)] < -0.05) under = i - 1000;
    const double held = g[1000 + 290], afterRelease = g[1000 + under + 250], closed = g[2990], open = g[3000 + 10];
    std::printf("   gain moves %d ms after the drop; 290 ms after it %+.2f dB, a release time later %+.2f dB, closed "
                "%+.2f dB, 10 ms after the rise %+.2f dB\n",
                under, held, afterRelease, closed, open);
    CHECK_RANGE(under, 300, 345);            // the detector's fall and the hold
    CHECK(held > -0.1);                      // the hold keeps it open
    CHECK_RANGE(afterRelease, -15.0, -10.0); // about 63 % of 20 dB one release time later
    CHECK_NEAR(closed, -20.0, 0.2);
    CHECK(open > -0.5);                       // open again within a few attack times
}

// Largest second difference in [from, to) over the settled one before and after (as deck_test's click ratio).
double clickRatio(const Buffer& y, size_t change) {
    auto maxD2 = [&](size_t a, size_t b) {
        double m = 0.0;
        for (size_t i = a + 2; i < b; ++i) m = std::max(m, std::fabs(y[i] - 2.0 * y[i - 1] + y[i - 2]));
        return m;
    };
    const double before = maxD2(change - frames(0.4), change), during = maxD2(change, change + frames(0.1));
    const double after = maxD2(change + frames(1.4), change + frames(1.9));
    return during / std::max(before, after);
}

void noClicks() {
    std::printf("-- no clicks: each value changed by 30 %% of its range while a 1 kHz tone at -50 dBFS is turned down "
                "part of the way (second difference during the change / settled)\n");
    PresetValues base = noiseOnly();
    base[pNoiseThreshold] = -30; // -44: the tone (about -50.7 on this scale) 6.7 dB under it
    base[pNoiseDepth] = 20;
    const Buffer l = sig::sine(fs, 1000, -50, 4.0), r = sig::sine(fs, 1000, -50, 4.0, 90);
    double worst = 0.0;
    for (int p = pNoiseOn; p <= pNoiseSplit3; ++p) {
        const ParamInfo& in = paramInfo(p);
        PresetValues v = base;
        double& x = v[static_cast<size_t>(p)];
        const double step = 0.3 * (in.max - in.min);
        x = in.isSwitch ? 1.0 - x : p == pNoiseBands ? 1.0 : x + step <= in.max ? x + step : x - step;
        if (p >= pNoiseSplit1) enforceNoiseSplits(v, p - pNoiseSplit1);
        // the deck in blocks of 256, the new values published at 2 s
        DeckProcessor d;
        d.prepare(fs);
        d.publish(makeDeckConfig(base, DeckControl(), fs));
        Buffer ol = l, orr = r;
        const size_t change = frames(2.0);
        for (size_t pos = 0; pos < ol.size(); pos += 256) {
            if (pos == change - change % 256) d.publish(makeDeckConfig(v, DeckControl(), fs));
            const int n = static_cast<int>(std::min<size_t>(256, ol.size() - pos));
            d.process(ol.data() + pos, orr.data() + pos, n);
            MeterRecord recs[64];
            while (d.meters().pop(recs, 64) > 0) {
            }
        }
        const double ratio = clickRatio(ol, change - change % 256);
        std::printf("   %-10s %8.2f -> %8.2f: %.2f\n", in.name, base[static_cast<size_t>(p)], x, ratio);
        worst = std::max(worst, ratio);
    }
    std::printf("   worst %.2f\n", worst);
    CHECK(worst < 1.5);
}

void splitRule() {
    std::printf("-- splits: rising, at least 1.25x apart, inside 20 Hz .. 16 kHz; bands only 1 or 4\n");
    PresetValues v = defaultValues();
    CHECK(v[pNoiseOn] == 0.0); // off by default
    for (int i = 0; i < kShippedPresetCount; ++i) CHECK(shippedPresetValues(i)[pNoiseOn] == 0.0);
    v[pNoiseSplit2] = 70; // under split 1 (80): split 1 is pushed down
    enforceNoiseSplits(v, 1);
    CHECK_NEAR(v[pNoiseSplit1], 56.0, 1e-9);
    v[pNoiseSplit3] = 20;
    enforceNoiseSplits(v, 2);
    CHECK(v[pNoiseSplit1] >= 20.0 && v[pNoiseSplit2] >= v[pNoiseSplit1] * 1.25 - 1e-9 &&
          v[pNoiseSplit3] >= v[pNoiseSplit2] * 1.25 - 1e-9);
    v[pNoiseBands] = 2;
    normaliseValues(v);
    CHECK(v[pNoiseBands] == 1.0);
    v[pNoiseBands] = 3;
    normaliseValues(v);
    CHECK(v[pNoiseBands] == 4.0);
    CHECK(paramPositions(pNoiseBands) == 1 && paramValueAt(pNoiseBands, 0) == 1.0 && paramValueAt(pNoiseBands, 1) == 4.0);
}

} // namespace

int main() {
    splitRule();
    flatWhileOpen();
    noiseTurnedDown();
    musicKeepsItsBand();
    times();
    noClicks();
    return test::finish("noise_test");
}
