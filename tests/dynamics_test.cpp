// The stages of the chain, each on its own with known answers (handbook 10, 16.1).
#include "check.h"
#include "deckrun.h"

#include <cmath>
#include <functional>

using namespace ap;
using sig::Buffer;
using tdeck::allOff;
using tdeck::run;

namespace {

const double fs = 48000.0;

void compressorCurve() {
    std::printf("-- compressor static curve\n");
    CHECK_NEAR(compressorCurveDb(-30, -14, 2, 6), -30, 1e-12);          // below the knee
    CHECK_NEAR(compressorCurveDb(-4, -14, 2, 6), -9, 1e-12);            // above: t + over / ratio
    CHECK_NEAR(compressorCurveDb(-14, -14, 2, 6), -14.375, 1e-12);      // in the knee: (1/r - 1) k^2 / 2w
    CHECK_NEAR(compressorCurveDb(-17, -14, 2, 6), -17, 1e-12);          // knee edge
    CHECK_NEAR(compressorCurveDb(-11, -14, 2, 6), -12.5, 1e-12);        // knee edge, upper
    CHECK_NEAR(compressorCurveDb(-4, -14, 4, 0), -11.5, 1e-12);         // hard knee
}

// Gain (dB) per 1 ms window: output RMS over input RMS.
std::vector<double> gainTrace(const Buffer& in, const Buffer& out) {
    const size_t w = static_cast<size_t>(fs / 1000);
    std::vector<double> g;
    for (size_t i = 0; i + w <= in.size(); i += w) g.push_back(tdeck::rmsDb(out, i, w) - tdeck::rmsDb(in, i, w));
    return g;
}

void compressorTimes() {
    std::printf("-- compressor step response (attack 10 ms, release 200 ms, ratio 4, threshold 0 LU re -14 LUFS)\n");
    PresetValues v = allOff();
    v[pCompOn] = 1;
    v[pTarget] = -14;
    v[pCompThreshold] = 0;
    v[pCompRatio] = 4;
    v[pCompKnee] = 0;
    v[pCompAttack] = 10;
    v[pCompRelease] = 200;
    v[pCompSideHpf] = 0;
    v[pCompMakeup] = 0;
    Buffer in = sig::sine(fs, 1000, -20, 1.0);
    sig::append(in, sig::sine(fs, 1000, 0, 1.0));
    sig::append(in, sig::sine(fs, 1000, -20, 1.5));
    tdeck::Stereo o = run(fs, v, in, in);
    const std::vector<double> g = gainTrace(in, o.l);
    // expected reduction: level -0.69 LUFS on the detector scale, 13.3 dB over -14, ratio 4 -> 9.98 dB
    const double final = g[1990];
    CHECK_NEAR(final, -9.98, 0.1);
    CHECK_NEAR(g[990], 0.0, 0.01);
    int attack = -1, release = -1;
    for (int i = 1000; i < 2000; ++i)
        if (g[static_cast<size_t>(i)] <= 0.63 * final) {
            attack = i - 1000;
            break;
        }
    for (int i = 2000; i < 3500; ++i)
        if (g[static_cast<size_t>(i)] >= 0.37 * final) {
            release = i - 2000;
            break;
        }
    std::printf("   reduction %.2f dB; 63 %% after %d ms, back to 37 %% after %d ms\n", -final, attack, release);
    CHECK_RANGE(attack, 8, 25);
    CHECK_RANGE(release, 150, 300);
}

void leveller() {
    std::printf("-- leveller (target -14 LUFS, range 9 dB, 1 dB/s, catch-up after a pause at Release 0.5 s)\n");
    PresetValues v = allOff();
    v[pLevOn] = 1;
    v[pLevRelease] = 0.5; // 20 dB/s: the catch-up after a pause as it was before Release (a fixed 20 dB/s)
    v[pTarget] = -14;
    Buffer l = sig::beatMusic(fs, 30, 124, 1), r = sig::beatMusic(fs, 30, 124, 2);
    tdeck::setLoudness(fs, l, r, -21);
    tdeck::Stereo o = run(fs, v, l, r);
    const double settled = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(20 * fs));
    const double early = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(3 * fs), static_cast<size_t>(6 * fs));
    std::printf("   input -21 LUFS: output %.2f LUFS at 3-6 s, %.2f LUFS at 20-30 s\n", early, settled);
    std::printf("   gain per 0.5 s:");
    for (double t = 0; t < 12; t += 0.5) {
        const size_t a0 = static_cast<size_t>(t * fs), w = static_cast<size_t>(0.5 * fs);
        std::printf(" %.1f", tdeck::rmsDb(o.l, a0, w) - tdeck::rmsDb(l, a0, w));
    }
    std::printf("\n");
    CHECK_NEAR(settled, -14.0, 0.5);
    CHECK_NEAR(early, -14.0, 1.5); // catch-up after the start (a stopped deck counts as a pause)

    // never pump a fade: a 10 s fade-out of 40 dB must not be turned up
    Buffer fl = sig::beatMusic(fs, 25, 124, 3), fr = sig::beatMusic(fs, 25, 124, 4);
    tdeck::setLoudness(fs, fl, fr, -14);
    for (size_t i = static_cast<size_t>(15 * fs); i < fl.size(); ++i) {
        const double t = (static_cast<double>(i) / fs - 15.0) / 10.0;
        const float g = static_cast<float>(dbToLin(-40.0 * t));
        fl[i] *= g;
        fr[i] *= g;
    }
    o = run(fs, v, fl, fr);
    double worst = -100;
    for (double t = 16; t < 24.5; t += 0.5) {
        const size_t a = static_cast<size_t>(t * fs), n = static_cast<size_t>(0.5 * fs);
        const double gain = tdeck::rmsDb(o.l, a, n) - tdeck::rmsDb(fl, a, n);
        worst = std::max(worst, gain);
    }
    const size_t a = static_cast<size_t>(14 * fs), n = static_cast<size_t>(fs);
    const double before = tdeck::rmsDb(o.l, a, n) - tdeck::rmsDb(fl, a, n);
    std::printf("   gain before the fade %.2f dB, highest during the fade %.2f dB\n", before, worst);
    CHECK(worst <= before + 1.0);
}

// A track that fades in after a pause: the catch-up starts when the fade passes the hold level (+ the 1.5 s warm
// start) and must not stop half-way up a slow fade (it would leave the slow leveller to correct the rest: 1 dB/s
// here, 0.5 in Gentle).
void levellerFadeIn() {
    std::printf("-- leveller catch-up on a track that fades in (3 s silence, fade-in from -50 dB, target -14 LUFS, "
                "Release 0.5 s)\n");
    PresetValues v = allOff();
    v[pLevOn] = 1;
    v[pLevRelease] = 0.5; // the former catch-up speed
    v[pTarget] = -14;
    for (double fade : {4.0, 12.0}) {
        for (double full : {-21.0, -7.0}) {
            Buffer l = sig::beatMusic(fs, 35, 124, 5), r = sig::beatMusic(fs, 35, 124, 6);
            tdeck::setLoudness(fs, l, r, full);
            const double end = 3.0 + fade;
            for (size_t i = 0; i < l.size(); ++i) {
                const double t = static_cast<double>(i) / fs;
                const double db = t < 3.0 ? -200.0 : t < end ? -50.0 + 50.0 * (t - 3.0) / fade : 0.0;
                const float g = static_cast<float>(dbToLin(db));
                l[i] *= g;
                r[i] *= g;
            }
            const tdeck::Stereo o = run(fs, v, l, r);
            const double soon = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>((end + 1) * fs),
                                                static_cast<size_t>((end + 6) * fs));
            const double late = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(25 * fs));
            std::printf("   fade %2.0f s, full %3.0f LUFS: %.2f LUFS 1-6 s after the fade, %.2f at 25-35 s; gain per s from "
                        "%.0f s:",
                        fade, full, soon, late, end - 4);
            for (double t = end - 4; t < end + 6; t += 1.0) {
                const size_t a0 = static_cast<size_t>(t * fs), w = static_cast<size_t>(fs);
                std::printf(" %.1f", tdeck::rmsDb(o.l, a0, w) - tdeck::rmsDb(l, a0, w));
            }
            std::printf("\n");
            CHECK_NEAR(soon, -14.0, 1.0);
            CHECK_NEAR(late, -14.0, 0.5);
        }
    }
}

// A loud song after a quiet intro (Gentle's leveller: 0.5 dB/s, range 12): the intro turns the gain up, the entry
// is 16 dB louder. Found on real music (session 4): at 0.5 dB/s the first 30 s after the entry played up to 9 LU
// over the target and went into the limiter. Fast down brings the gain down at its own speed while it is more than
// 3 dB above what the 3 s level needs; a chorus a few LU over the verse must not be pulled down by it.
void levellerFastDown() {
    std::printf("-- leveller fast down (target -18 LUFS, range 12 dB, 0.5 dB/s; 6 s intro at -25, then -9 LUFS)\n");
    PresetValues v = allOff();
    v[pLevOn] = 1;
    v[pTarget] = -18;
    v[pLevRange] = 12;
    v[pLevSpeed] = 0.5;
    v[pLevRelease] = 5.0; // Gentle's
    Buffer l = sig::beatMusic(fs, 46, 124, 7), r = sig::beatMusic(fs, 46, 124, 8);
    tdeck::setLoudness(fs, l, r, -9);
    const size_t entry = static_cast<size_t>(6 * fs);
    const float intro = static_cast<float>(dbToLin(-16.0));
    for (size_t i = 0; i < entry; ++i) {
        l[i] *= intro;
        r[i] *= intro;
    }
    auto at = [&](const tdeck::Stereo& o, double from, double to) {
        return tdeck::loudness(fs, o.l, o.r, entry + static_cast<size_t>(from * fs), entry + static_cast<size_t>(to * fs));
    };
    double first[2], soon[2], whole[2], late[2];
    for (int fast = 0; fast < 2; ++fast) {
        v[pLevAttack] = fast ? 1.0 : 20.0; // 10 dB/s (the default), or 0.5 dB/s = this Speed: no fast down
        const tdeck::Stereo o = run(fs, v, l, r);
        first[fast] = at(o, 0, 2);
        soon[fast] = at(o, 4, 10);
        whole[fast] = at(o, 2, 40);
        late[fast] = at(o, 30, 40);
        std::printf("   fast down %s: %.2f LUFS in the first 2 s after the entry, %.2f at 4-10 s, %.2f at 2-40 s, "
                    "%.2f at 30-40 s; gain per 2 s from the entry:",
                    fast ? "10 dB/s" : "off    ", first[fast], soon[fast], whole[fast], late[fast]);
        for (double t = 0; t < 20; t += 2.0) {
            const size_t a0 = entry + static_cast<size_t>(t * fs), w = static_cast<size_t>(2 * fs);
            std::printf(" %.1f", tdeck::rmsDb(o.l, a0, w) - tdeck::rmsDb(l, a0, w));
        }
        std::printf("\n");
    }
    CHECK(soon[0] > -18.0 + 3.0); // the fault this guards against is there without fast down
    CHECK_NEAR(soon[1], -18.0, 1.0);
    CHECK_NEAR(whole[1], -18.0, 0.5);
    CHECK(first[1] < first[0] - 3.0); // the entry itself: the gain is on its way down within the first second
    CHECK_NEAR(late[1], late[0], 0.3); // the settled level does not change

    // a song at the target with an 8 s chorus 4 LU louder: fast down may only take a little more off the chorus
    Buffer cl = sig::beatMusic(fs, 40, 124, 9), cr = sig::beatMusic(fs, 40, 124, 10);
    tdeck::setLoudness(fs, cl, cr, -18);
    const float up = static_cast<float>(dbToLin(4.0));
    for (size_t i = static_cast<size_t>(25 * fs); i < static_cast<size_t>(33 * fs); ++i) {
        cl[i] *= up;
        cr[i] *= up;
    }
    double chorus[2];
    for (int fast = 0; fast < 2; ++fast) {
        v[pLevAttack] = fast ? 1.0 : 20.0;
        const tdeck::Stereo o = run(fs, v, cl, cr);
        chorus[fast] = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(25 * fs), static_cast<size_t>(33 * fs));
    }
    std::printf("   chorus +4 LU for 8 s: %.2f LUFS without fast down, %.2f with\n", chorus[0], chorus[1]);
    CHECK_NEAR(chorus[1], chorus[0], 0.5);
}

// A soft intro (client, 2026-10-27: "it sometimes takes too long before a soft intro is on level, and then it is as
// if someone quickly turns the volume knob"): 3 s of silence, a 15 s intro 12 LU under the target (under the hold
// level), then the song at -12 LUFS. Quiet wait, Release and Attack set how soon the intro is up, how fast it goes
// up and how fast the entry comes down.
void levellerIntro() {
    std::printf("-- leveller and a soft intro (target -14 LUFS, range 12 dB, 1 dB/s; 3 s silence, 15 s intro at -26, "
                "then -12 LUFS)\n");
    const double pause = 3.0, intro = 15.0;
    Buffer l = sig::beatMusic(fs, 40, 124, 11), r = sig::beatMusic(fs, 40, 124, 12);
    tdeck::setLoudness(fs, l, r, -12);
    for (size_t i = 0; i < l.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        const float g = static_cast<float>(t < pause ? 0.0 : t < pause + intro ? dbToLin(-14.0) : 1.0);
        l[i] *= g;
        r[i] *= g;
    }
    struct Case {
        const char* name;
        double wait, release, attack;
    };
    double onLevel[3] = {}, step[3] = {};
    const Case cases[3] = {{"before  (wait 3 s, release 3.3 s, attack 1 s)", 3.0, 3.3, 1.0},
                           {"default (wait 1 s, release 1.5 s, attack 1 s)", 1.0, 1.5, 1.0},
                           {"long    (wait 8 s, release 8 s,   attack 4 s)", 8.0, 8.0, 4.0}};
    for (int k = 0; k < 3; ++k) {
        PresetValues v = allOff();
        v[pLevOn] = 1;
        v[pTarget] = -14;
        v[pLevRange] = 12;
        v[pLevSpeed] = 1;
        v[pLevQuietWait] = cases[k].wait;
        v[pLevRelease] = cases[k].release;
        v[pLevAttack] = cases[k].attack;
        const tdeck::Stereo o = run(fs, v, l, r);
        // the intro on level: the first 0.5 s window whose output is within 2 LU of the target, from the intro's start
        onLevel[k] = intro;
        for (double t = pause; t < pause + intro - 0.5; t += 0.25)
            if (std::fabs(tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(t * fs), static_cast<size_t>((t + 0.5) * fs)) +
                          14.0) < 2.0) {
                onLevel[k] = t - pause;
                break;
            }
        // the entry: the loudest 0.5 s just after it, over the target
        step[k] = -100.0;
        for (double t = pause + intro; t < pause + intro + 4.0; t += 0.25)
            step[k] = std::max(step[k], tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(t * fs),
                                                        static_cast<size_t>((t + 0.5) * fs)) + 14.0);
        std::printf("   %s: intro on level after %.2f s; the entry at most %+.1f LU over the target\n", cases[k].name,
                    onLevel[k], step[k]);
    }
    CHECK(onLevel[0] < 10.0);               // before: the warm start, the wait, then 12 dB at 3 dB/s
    CHECK(onLevel[1] < onLevel[0] - 3.0);   // the default now: clearly sooner
    CHECK(onLevel[2] > onLevel[0] + 3.0);   // long: clearly later
    CHECK(step[2] < step[0]);               // an intro left soft: a smaller jump at the entry
}

// Quiet sections inside a song (client, 2026-10-26: a song whose spoken verses lie 10 LU under its choruses stayed
// 6-8 LU under the target for 10-19 s after each chorus). Target -14, range 12 dB, 1 dB/s, hold 10 LU, fast down
// 10 dB/s. A verse with gaps between its phrases is levelled within seconds; a steady breakdown keeps its drop for
// its first seconds; a slow fade (1 dB/s) is never turned up.
void levellerQuietSections() {
    std::printf("-- leveller and quiet sections (target -14 LUFS, range 12 dB, 1 dB/s, hold 10 LU)\n");
    PresetValues v = allOff();
    v[pLevOn] = 1;
    v[pTarget] = -14;
    v[pLevRange] = 12;
    v[pLevSpeed] = 1;
    v[pLevHold] = 10;
    v[pLevAttack] = 1.0;     // 10 dB/s
    v[pLevQuietWait] = 3.0;  // the rules as they were measured here (the shipped default is 1 s now)
    v[pLevRelease] = 3.3;
    auto gainAt = [](const tdeck::Stereo& o, const Buffer& in, double from, double to) {
        const size_t a = static_cast<size_t>(from * fs), n = static_cast<size_t>((to - from) * fs);
        return tdeck::rmsDb(o.l, a, n) - tdeck::rmsDb(in, a, n);
    };
    auto shape = [](Buffer& l, Buffer& r, const std::function<double(double)>& db) {
        for (size_t i = 0; i < l.size(); ++i) {
            const float g = static_cast<float>(dbToLin(db(static_cast<double>(i) / fs)));
            l[i] *= g;
            r[i] *= g;
        }
    };

    // chorus -12 LUFS (0-20 s), verse 10 LU softer with a 0.8 s gap every 3 s (20-45 s), chorus again
    Buffer l = sig::beatMusic(fs, 60, 124, 11), r = sig::beatMusic(fs, 60, 124, 12);
    tdeck::setLoudness(fs, l, r, -12);
    shape(l, r, [](double t) {
        if (t < 20 || t >= 45) return 0.0;
        return std::fmod(t - 20, 3.0) > 2.2 ? -40.0 : -10.0;
    });
    tdeck::Stereo o = run(fs, v, l, r);
    const double chorus = gainAt(o, l, 17, 20), drop = gainAt(o, l, 20, 22);
    const double verse = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(29 * fs), static_cast<size_t>(45 * fs));
    std::printf("   verse with gaps: gain %.2f dB in the chorus, %.2f in the verse's first 2 s; verse from 9 s on %.2f LUFS; "
                "gain per s from 18 s:", chorus, drop, verse);
    for (double t = 18; t < 32; t += 1.0) std::printf(" %.1f", gainAt(o, l, t, t + 1));
    std::printf("\n");
    CHECK(drop <= chorus + 0.5);     // the drop itself is heard
    CHECK_NEAR(verse, -14.0, 2.0);   // then the verse comes up to the target

    // a steady breakdown 10 LU down for 16 s: its first 3 s keep the drop
    l = sig::beatMusic(fs, 50, 124, 13);
    r = sig::beatMusic(fs, 50, 124, 14);
    tdeck::setLoudness(fs, l, r, -14);
    shape(l, r, [](double t) { return t >= 20 && t < 36 ? -10.0 : 0.0; });
    o = run(fs, v, l, r);
    const double before = gainAt(o, l, 17, 20), first = gainAt(o, l, 20, 23);
    const double back = tdeck::loudness(fs, o.l, o.r, static_cast<size_t>(39 * fs), static_cast<size_t>(50 * fs));
    std::printf("   breakdown: gain %.2f dB before, %.2f in its first 3 s; %.2f LUFS 3-14 s after its end; gain per s from "
                "18 s:", before, first, back);
    for (double t = 18; t < 42; t += 1.0) std::printf(" %.1f", gainAt(o, l, t, t + 1));
    std::printf("\n");
    CHECK(first <= before + 1.0);
    CHECK_NEAR(back, -14.0, 1.0);

    // a slow fade-out: 1 dB/s for 40 s. It is recognised as a fall after about 4 s (two 2 s halves, each down more than
    // 0.8 dB); until then the leveller may follow it at its own speed, so up to 2 dB here (a fast fade: 1 dB, above)
    l = sig::beatMusic(fs, 60, 124, 15);
    r = sig::beatMusic(fs, 60, 124, 16);
    tdeck::setLoudness(fs, l, r, -14);
    shape(l, r, [](double t) { return t < 15 ? 0.0 : -(t - 15); });
    o = run(fs, v, l, r);
    const double start = gainAt(o, l, 13, 15);
    double worst = -100;
    for (double t = 15; t < 50; t += 0.5) worst = std::max(worst, gainAt(o, l, t, t + 0.5));
    std::printf("   slow fade 1 dB/s: gain %.2f dB before, highest during the fade %.2f dB; gain per 2 s from 13 s:", start,
                worst);
    for (double t = 13; t < 51; t += 2.0) std::printf(" %.1f", gainAt(o, l, t, t + 2));
    std::printf("\n");
    CHECK(worst <= start + 2.0); // before this rule: +4.7 dB (+12 with the first version of the quiet-section release)
}

double magnitudeAt(const Buffer& ir, double hz) {
    double re = 0, im = 0;
    const double w = 2 * kPi * hz / fs;
    for (size_t i = 0; i < ir.size(); ++i) {
        re += ir[i] * std::cos(w * static_cast<double>(i));
        im -= ir[i] * std::sin(w * static_cast<double>(i));
    }
    return 10 * std::log10(re * re + im * im);
}

Buffer densityImpulse(PresetValues v) {
    Buffer imp = sig::silence(fs, 1.5);
    imp[100] = 0.25f;
    tdeck::Stereo o = run(fs, v, imp, imp, 0.0);
    Buffer ir(o.l.begin() + 100, o.l.end());
    for (float& x : ir) x *= 4.0f;
    return ir;
}

void densityBank() {
    std::printf("-- density filter bank: flat sum for 1..7 bands, band isolation\n");
    PresetValues v = allOff();
    v[pDensOn] = 1;
    v[pDensAmount] = 0; // ratio 1:1: only the filter bank
    for (int bands = 1; bands <= 7; ++bands) {
        v[pDensBands] = bands;
        loadDefaultSplits(v, bands);
        const Buffer ir = densityImpulse(v);
        if (bands == 1) {
            int shown = 0;
            for (size_t i = 0; i < ir.size() && shown < 12; ++i)
                if (std::fabs(ir[i]) > 1e-4) {
                    std::printf("   ir[%zu] = %.6f\n", i, ir[i]);
                    ++shown;
                }
        }
        double worst = 0;
        for (double f = 30; f <= 15000; f *= 1.1) worst = std::max(worst, std::fabs(magnitudeAt(ir, f)));
        std::printf("   %d band(s): largest deviation 30 Hz - 15 kHz %.4f dB\n", bands, worst);
        CHECK(worst < 0.01);
    }
    // band 1 +6 dB must not move a tone two bands away (5 bands: band 3 = 200 .. 2500 Hz)
    v[pDensBands] = 5;
    loadDefaultSplits(v, 5);
    const double ref = magnitudeAt(densityImpulse(v), 1000);
    v[pDensGain1] = 6;
    const Buffer ir = densityImpulse(v);
    const double moved = magnitudeAt(ir, 1000) - ref;
    // 20 Hz, not 30: at 30 Hz the LR4 split at 60 Hz still hands 6 % to band 2 (5.76 dB expected), at 20 Hz 1 %
    std::printf("   band 1 at +6 dB: 1 kHz moved %.5f dB, 20 Hz %.2f dB\n", moved, magnitudeAt(ir, 20));
    CHECK(std::fabs(moved) < 0.001);
    CHECK_NEAR(magnitudeAt(ir, 20), 5.97, 0.1);
    // the full range (client, 2026-10-26: -12 .. +12 dB, was -6 .. +6): band 3 at its ends, measured in its geometric
    // middle (707 Hz); nearer a split the LR4 neighbours still blend in (at 1 kHz, 1.3 octaves under the 2.5 kHz split:
    // +11.8 / -11.4 dB)
    v[pDensGain1] = 0;
    const double ref707 = magnitudeAt(densityImpulse(v), 707);
    for (double g : {12.0, -12.0}) {
        v[pDensGain3] = g;
        const Buffer band3 = densityImpulse(v);
        const double mid = magnitudeAt(band3, 707) - ref707, at1k = magnitudeAt(band3, 1000) - ref;
        std::printf("   band 3 at %+.0f dB: 707 Hz %+.2f dB, 1 kHz %+.2f dB\n", g, mid, at1k);
        CHECK_NEAR(mid, g, 0.5);
    }
    CHECK(paramInfo(pDensGain3).min == -12.0 && paramInfo(pDensGain3).max == 12.0);
}

void densityAction() {
    std::printf("-- density at work (5 bands, 70 %%)\n");
    PresetValues v = allOff();
    v[pDensOn] = 1;
    v[pDensAmount] = 70;
    v[pTarget] = -14;
    // steady noise near the target, then a loud 50 Hz burst train: only the low band may move
    Buffer l = sig::pinkNoise(fs, 12, 1.0, 5), r = sig::pinkNoise(fs, 12, 1.0, 6);
    tdeck::setLoudness(fs, l, r, -16);
    for (size_t i = static_cast<size_t>(6 * fs); i < l.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        const float k = static_cast<float>(0.5 * std::exp(-std::fmod(t, 0.5) / 0.1) * std::sin(2 * kPi * 50 * t));
        l[i] += k;
        r[i] += k;
    }
    tdeck::Stereo o = run(fs, v, l, r);
    // 5 kHz band level change (in / out) before and during the bursts, measured with a band-pass
    auto band = [&](const Buffer& b, size_t from, size_t n) {
        Biquad2 hp, lp;
        hp.set(design::highPass(fs, 3500));
        lp.set(design::lowPass(fs, 5500));
        double s = 0;
        for (size_t i = 0; i < from + n; ++i) {
            const double y = lp.process(hp.process(b[i]));
            if (i >= from) s += y * y;
        }
        return 10 * std::log10(s / static_cast<double>(n));
    };
    const size_t n = static_cast<size_t>(2 * fs);
    const double before = band(o.l, static_cast<size_t>(3.5 * fs), n) - band(l, static_cast<size_t>(3.5 * fs), n);
    const double during = band(o.l, static_cast<size_t>(9.5 * fs), n) - band(l, static_cast<size_t>(9.5 * fs), n);
    std::printf("   3.5-5.5 kHz gain before the bursts %.2f dB, during %.2f dB\n", before, during);
    CHECK(std::fabs(during - before) < 0.5);

    // start from silence: no deep dip while the references learn (handbook fault 10)
    Buffer s = sig::silence(fs, 2);
    Buffer nl = sig::pinkNoise(fs, 6, 1.0, 7), nr = sig::pinkNoise(fs, 6, 1.0, 8);
    tdeck::setLoudness(fs, nl, nr, -16);
    Buffer sl = s, sr = s;
    sig::append(sl, nl);
    sig::append(sr, nr);
    o = run(fs, v, sl, sr);
    double low = 100;
    const double settledGain = tdeck::rmsDb(o.l, static_cast<size_t>(6 * fs), static_cast<size_t>(fs)) -
                               tdeck::rmsDb(sl, static_cast<size_t>(6 * fs), static_cast<size_t>(fs));
    for (double t = 2.0; t < 5.0; t += 0.1) {
        const size_t a = static_cast<size_t>(t * fs), w = static_cast<size_t>(0.1 * fs);
        low = std::min(low, tdeck::rmsDb(o.l, a, w) - tdeck::rmsDb(sl, a, w) - settledGain);
    }
    std::printf("   after silence: deepest 100 ms in the first 3 s %.2f dB under the settled gain\n", -low);
    CHECK(low > -3.0);
    // nothing lifted in silence: output of silence stays silence
    CHECK(sig::peak(o.l, 0) >= 0 && tdeck::rmsDb(o.l, static_cast<size_t>(0.5 * fs), static_cast<size_t>(fs)) < -200);
}

void width() {
    std::printf("-- stereo width\n");
    PresetValues v = allOff();
    v[pWidthOn] = 1;
    v[pWidthSplit] = 150;
    Buffer tone = sig::sine(fs, 1000, 0 - 6, 1.0), zero = sig::silence(fs, 1.0);
    const size_t from = static_cast<size_t>(0.5 * fs);
    v[pWidth] = 200;
    tdeck::Stereo o = run(fs, v, tone, zero);
    const double a = sig::amplitudeAt(tone, fs, 1000, from);
    std::printf("   left-only tone, width 200 %%: L %.3f R %.3f (expected 1.5 / 0.5)\n",
                sig::amplitudeAt(o.l, fs, 1000, from) / a, sig::amplitudeAt(o.r, fs, 1000, from) / a);
    CHECK_NEAR(sig::amplitudeAt(o.l, fs, 1000, from) / a, 1.5, 0.01);
    CHECK_NEAR(sig::amplitudeAt(o.r, fs, 1000, from) / a, 0.5, 0.01);
    v[pWidth] = 0;
    o = run(fs, v, tone, zero);
    CHECK_NEAR(sig::amplitudeAt(o.l, fs, 1000, from) / a, 0.5, 0.01);
    CHECK_NEAR(sig::amplitudeAt(o.r, fs, 1000, from) / a, 0.5, 0.01);
    // bass width 0 %: a left-only 50 Hz tone becomes mono
    v[pWidth] = 100;
    v[pBassWidth] = 0;
    Buffer bass = sig::sine(fs, 50, -6, 2.0);
    o = run(fs, v, bass, sig::silence(fs, 2.0));
    const double b = sig::amplitudeAt(bass, fs, 50, static_cast<size_t>(fs));
    std::printf("   left-only 50 Hz, bass width 0 %%: L %.3f R %.3f\n", sig::amplitudeAt(o.l, fs, 50, static_cast<size_t>(fs)) / b,
                sig::amplitudeAt(o.r, fs, 50, static_cast<size_t>(fs)) / b);
    CHECK_NEAR(sig::amplitudeAt(o.l, fs, 50, static_cast<size_t>(fs)) / b, 0.5, 0.02);
    CHECK_NEAR(sig::amplitudeAt(o.r, fs, 50, static_cast<size_t>(fs)) / b, 0.5, 0.02);
    // mono stays mono, exactly
    v[pWidth] = 180;
    v[pBassWidth] = 30;
    Buffer m = sig::pinkNoise(fs, 1.0, 0.3, 9);
    o = run(fs, v, m, m);
    bool mono = true;
    for (size_t i = 0; i < o.l.size(); ++i) mono = mono && o.l[i] == o.r[i];
    CHECK(mono);
}

void colour() {
    std::printf("-- bass enhancer, exciter, de-esser, soft clipper\n");
    const size_t from = static_cast<size_t>(0.5 * fs);
    // bass enhancer: 50 Hz gets a 100 Hz overtone; 1 kHz passes unchanged
    PresetValues v = allOff();
    v[pBassOn] = 1;
    v[pBassAmount] = 30;
    Buffer b50 = sig::sine(fs, 50, -12, 2.0);
    tdeck::Stereo o = run(fs, v, b50, b50);
    const double h2 = linToDb(sig::amplitudeAt(o.l, fs, 100, from) / sig::amplitudeAt(o.l, fs, 50, from));
    std::printf("   bass enhancer: 100 Hz %.1f dB under 50 Hz\n", -h2);
    CHECK_RANGE(h2, -14, -6);
    Buffer k1 = sig::sine(fs, 1000, -12, 1.0);
    o = run(fs, v, k1, k1);
    CHECK_NEAR(linToDb(sig::amplitudeAt(o.l, fs, 1000, from) / sig::amplitudeAt(k1, fs, 1000, from)), 0.0, 0.05);
    v[pBassOn] = 0;
    o = run(fs, v, b50, b50);
    CHECK(sig::amplitudeAt(o.l, fs, 100, from) < 1e-6);

    // exciter: 5 kHz gets 15 kHz; 500 Hz unchanged
    v = allOff();
    v[pExcOn] = 1;
    v[pExcAmount] = 20;
    Buffer h5 = sig::sine(fs, 5000, -6, 1.0);
    o = run(fs, v, h5, h5);
    const double h3 = linToDb(sig::amplitudeAt(o.l, fs, 15000, from) / sig::amplitudeAt(o.l, fs, 5000, from));
    std::printf("   exciter: 5 kHz at -6 dBFS gets 15 kHz %.1f dB under it\n", -h3);
    CHECK_RANGE(h3, -45, -15);
    Buffer l5 = sig::sine(fs, 500, -6, 1.0);
    o = run(fs, v, l5, l5);
    CHECK_NEAR(linToDb(sig::amplitudeAt(o.l, fs, 500, from) / sig::amplitudeAt(l5, fs, 500, from)), 0.0, 0.01);

    // de-esser: a loud 7 kHz is turned down, 1 kHz at the same level is not
    v = allOff();
    v[pDessOn] = 1;
    v[pTarget] = -14;
    Buffer s7 = sig::sine(fs, 7000, -12, 1.0);
    o = run(fs, v, s7, s7);
    const double d7 = linToDb(sig::amplitudeAt(o.l, fs, 7000, from) / sig::amplitudeAt(s7, fs, 7000, from));
    o = run(fs, v, k1, k1);
    const double d1 = linToDb(sig::amplitudeAt(o.l, fs, 1000, from) / sig::amplitudeAt(k1, fs, 1000, from));
    std::printf("   de-esser: 7 kHz at -12 dBFS %.2f dB, 1 kHz %.3f dB\n", d7, d1);
    CHECK_RANGE(d7, -6.0, -3.0);
    CHECK_NEAR(d1, 0.0, 0.05);

    // soft clipper: curve, and aliasing of 3 kHz driven 6 dB in
    CHECK_NEAR(softClip(0.3), 0.3, 1e-15);
    CHECK_NEAR(softClip(0.5), 0.5, 1e-15);
    CHECK(softClip(10.0) <= 1.0 && softClip(-10.0) >= -1.0); // never past the ceiling
    CHECK(softClip(0.9) > softClip(0.8));
    v = allOff();
    v[pClipOn] = 1;
    v[pClipDrive] = 6;
    v[pLimCeiling] = -1;
    // at 44.1 kHz the folded harmonics of 3 kHz do not land on multiples of 3 kHz (at 48 kHz they would)
    const double fa = 44100.0;
    Buffer c3 = sig::sine(fa, 3000, -1, 1.0);
    o = run(fa, v, c3, c3);
    // with the limiter off the clipper holds the ceiling itself (back at the base rate a flattened peak would rise a
    // little over it - with the limiter on, the limiter holds it)
    std::printf("   soft clipper, limiter off: peak %.4f dBFS (ceiling -1)\n", linToDb(sig::peak(o.l)));
    CHECK(sig::peak(o.l) <= dbToLin(-1.0) + 1e-6);
    double alias = -300;
    const double fund = sig::amplitudeAt(o.l, fa, 3000, from);
    for (double f = 100; f < 22000; f += 100) {
        if (std::fmod(f, 3000.0) == 0.0) continue; // harmonics below Nyquist are not aliases
        alias = std::max(alias, linToDb(sig::amplitudeAt(o.l, fa, f, from) / fund));
    }
    std::printf("   soft clipper, 3 kHz driven 6 dB in: strongest alias %.1f dB under the tone\n", -alias);
    CHECK(alias < -35);
    // the highs: 13 kHz driven 6 dB in at 48 kHz - its 3rd overtone (39 kHz) folded back to 9 kHz at the base rate
    // (outside review: -16..-44 dBc on 10-15 kHz tones); oversampled (and held at the ceiling) it is gone
    // with the limiter after it, as in every shipped preset: it holds the ceiling the band-limited peaks rise over
    v[pLimOn] = 1;
    Buffer c13 = sig::sine(fs, 13000, -1, 1.0);
    o = run(fs, v, c13, c13);
    const double a9 = linToDb(sig::amplitudeAt(o.l, fs, 9000, from) / sig::amplitudeAt(o.l, fs, 13000, from));
    std::printf("   soft clipper, 13 kHz driven 6 dB in at 48 kHz: the fold-back at 9 kHz %.1f dB under the tone, peak %.3f dBFS\n",
                -a9, linToDb(sig::peak(o.l)));
    CHECK(a9 < -60.0);
    CHECK(sig::peak(o.l) <= dbToLin(-1.0) + 1e-6);

    // the worst case: hard-driven high tones (drive 9 dB, -6 dBFS in), every component that is not the tone (their
    // overtones are all above the Nyquist frequency, so whatever else is there is fold-back). 2x leaves a rest here
    // (the 5th and 7th overtones fold inside the doubled band): about -40 dBc at 15 kHz, from -16 at the base rate; the
    // default 4x: about -62.
    v[pClipDrive] = 9;
    double worst = -300, worstF = 0;
    for (double f : {12000.0, 13000.0, 14000.0, 15000.0, 16000.0}) {
        Buffer t = sig::sine(fs, f, -6, 1.0);
        o = run(fs, v, t, t);
        const double tone = sig::amplitudeAt(o.l, fs, f, from);
        for (double g = 100; g < 24000; g += 100) {
            if (g == f) continue;
            const double a = linToDb(sig::amplitudeAt(o.l, fs, g, from) / tone);
            if (a > worst) {
                worst = a;
                worstF = f;
            }
        }
    }
    std::printf("   soft clipper, 12-16 kHz driven 9 dB in: strongest fold-back %.1f dB under the tone (at %.0f Hz)\n",
                -worst, worstF);
    CHECK(worst < -55.0); // at the default 4x (2x: about -40, the rest the 4x and 8x choices take away)

    // the same with the limiter off: the clipper's hard hold on the ceiling cut the band-limited peaks and brought the
    // fold-back back (-25 dBc, fourth outside review); its curve now bends 1.5 dB under the ceiling, the hold stays
    v[pLimOn] = 0;
    double worstOff = -300, peakOff = 0;
    for (double f : {10000.0, 15000.0}) {
        Buffer t = sig::sine(fs, f, -6, 1.0);
        o = run(fs, v, t, t);
        const double tone = sig::amplitudeAt(o.l, fs, f, from);
        for (double g = 100; g < 24000; g += 100)
            if (g != f) worstOff = std::max(worstOff, linToDb(sig::amplitudeAt(o.l, fs, g, from) / tone));
        peakOff = std::max(peakOff, sig::peak(o.l));
    }
    std::printf("   soft clipper, limiter off, 10 / 15 kHz driven 9 dB in: strongest fold-back %.1f dB under the tone, "
                "peak %.3f dBFS (ceiling -1)\n", -worstOff, linToDb(peakOff));
    CHECK(worstOff < -45.0);
    CHECK(peakOff <= dbToLin(-1.0) + 1e-6);
    // the bend itself: a low tone clipped with the limiter off stays clear of the ceiling (the curve 1.5 dB under it,
    // the band-limited square wave's overshoot about 1 dB back up), not held at it (sixth outside review: taking the
    // 1.5 dB away was not caught - the fold-back above stays low on 10-15 kHz either way); without the bend: -1.00
    Buffer low = sig::sine(fs, 100, -1, 1.0);
    o = run(fs, v, low, low);
    const double lowPeak = linToDb(sig::peak(o.l));
    std::printf("   soft clipper, limiter off, 100 Hz driven 9 dB in: peak %.2f dBFS (ceiling -1, the curve 1.5 dB under it)\n",
                lowPeak);
    CHECK_RANGE(lowPeak, -2.8, -1.3);

    // the exciter's saturation runs at twice the rate too: a 10 kHz tone's 3rd overtone (30 kHz) folded back to
    // 18 kHz at the base rate (-20.6 dBc in the outside review)
    v = allOff();
    v[pExcOn] = 1;
    v[pExcAmount] = 100;
    v[pExcFreq] = 3500;
    Buffer e10 = sig::sine(fs, 10000, -6, 1.0);
    o = run(fs, v, e10, e10);
    const double a18 = linToDb(sig::amplitudeAt(o.l, fs, 18000, from) / sig::amplitudeAt(o.l, fs, 10000, from));
    std::printf("   exciter 100 %%, 10 kHz at -6 dBFS: the fold-back at 18 kHz %.1f dB under the tone\n", -a18);
    CHECK(a18 < -50.0);
}

} // namespace

int main() {
    compressorCurve();
    compressorTimes();
    leveller();
    levellerFadeIn();
    levellerFastDown();
    levellerQuietSections();
    levellerIntro();
    densityBank();
    densityAction();
    width();
    colour();
    return test::finish("dynamics_test");
}
