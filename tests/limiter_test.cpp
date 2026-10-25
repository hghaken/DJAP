// Look-ahead true-peak limiter on hostile signals (handbook 10.10, fault 11 and fault 25).
#include "check.h"
#include "limiter.h"
#include "signals.h"

#include <algorithm>
#include <string>

using namespace ap;
using sig::Buffer;

namespace {

struct Out {
    Buffer l, r;
};

int gFactor = 8, gTaps = 96;
double gBeta = 5.0;

Out run(double fs, double lookMs, double ceilingDb, double releaseMs, const Buffer& inL, const Buffer& inR,
        int* lookOut = nullptr, bool* tpOut = nullptr) {
    Limiter lim;
    const int maxLook = static_cast<int>(std::ceil(5.0 * fs / 1000.0));
    lim.prepare(maxLook, gFactor, gTaps, gBeta);
    lim.setLook(static_cast<int>(std::lround(lookMs * fs / 1000.0)));
    lim.setParams(dbToLin(ceilingDb), onePoleCoeff(releaseMs / 1000.0, fs));
    if (lookOut) *lookOut = lim.look();
    if (tpOut) *tpOut = lim.usesTruePeak();
    Out o;
    const size_t n = inL.size() + static_cast<size_t>(lim.look());
    o.l.resize(n);
    o.r.resize(n);
    for (size_t i = 0; i < n; ++i) {
        double l = i < inL.size() ? inL[i] : 0.0, r = i < inR.size() ? inR[i] : 0.0;
        lim.process(l, r);
        o.l[i] = static_cast<float>(l);
        o.r[i] = static_cast<float>(r);
    }
    return o;
}

// Reference true peak: 16x, 128 taps per phase (far finer than the limiter's own detector).
double refTruePeakDb(const Buffer& b, size_t from = 0) {
    TruePeakDetector d;
    d.configure(16, 128, 10.0);
    float p = 0.0f;
    for (size_t i = 0; i < b.size(); ++i) {
        const float v = d.push(b[i]);
        if (i >= from + 64) p = std::max(p, v);
    }
    for (int i = 0; i < 128; ++i) p = std::max(p, d.push(0.0f));
    return linToDb(p);
}

void check(const char* what, double fs, double lookMs, const Buffer& l, const Buffer& r, bool expectTp,
           size_t settle = 0) {
    const double ceilDb = -1.0;
    int look = 0;
    bool tp = false;
    Out o = run(fs, lookMs, ceilDb, 80.0, l, r, &look, &tp);
    const double sp = linToDb(std::max(sig::peak(o.l, settle), sig::peak(o.r, settle)));
    const double tpl = std::max(refTruePeakDb(o.l, settle), refTruePeakDb(o.r, settle));
    std::printf("   %-34s %5.0f Hz look %4.2f ms (%2d fr, %s): sample peak %+6.2f dBFS, true peak %+6.2f dBTP\n", what,
                fs, lookMs, look, tp ? "TP" : "SP", sp, tpl);
    CHECK(sp <= ceilDb + 1e-4);
    CHECK(tp == expectTp);
    if (tp) CHECK(tpl <= ceilDb + 0.1);
}

void hostile(double fs) {
    std::printf("-- ceiling -1 dB at %.0f Hz\n", fs);
    for (double lookMs : {1.5, 0.2, 0.1, 0.0}) {
        const int look = static_cast<int>(std::lround(lookMs * fs / 1000.0));
        const bool tp = look >= 48;
        // single +12 dBFS samples on silence and on a -20 dBFS sine
        Buffer l = sig::silence(fs, 2.0), r = sig::silence(fs, 2.0);
        Buffer bl = sig::sine(fs, 997, -20, 2.0), br = sig::sine(fs, 997, -20, 2.0, 90);
        for (size_t i = 1000; i < l.size(); i += static_cast<size_t>(fs / 7)) {
            const float a = static_cast<float>(dbToLin(12.0)) * ((i / 1000) % 2 ? 1.0f : -1.0f);
            l[i] = a;
            bl[i] += a;
            br[i + 1 < br.size() ? i + 1 : i] -= a;
        }
        check("single +12 dBFS samples", fs, lookMs, l, r, tp);
        check("single samples on a -20 dBFS sine", fs, lookMs, bl, br, tp);
        // +6 dBFS sine
        Buffer s6 = sig::sine(fs, 1000, 6, 2.0);
        check("+6 dBFS 1 kHz sine", fs, lookMs, s6, s6, tp, static_cast<size_t>(0.05 * fs));
        // fs/4 square at +6 dBFS: inter-sample peaks 3 dB over the samples
        Buffer sq(static_cast<size_t>(2.0 * fs));
        for (size_t i = 0; i < sq.size(); ++i) sq[i] = (i / 2) % 2 ? -2.0f : 2.0f;
        check("fs/4 square +6 dBFS", fs, lookMs, sq, sq, tp, static_cast<size_t>(0.05 * fs));
        // full-band white noise bursts, peak +9.5 dBFS, independent channels
        Buffer nl = sig::whiteNoise(fs, 4.0, dbToLin(9.5), 11), nr = sig::whiteNoise(fs, 4.0, dbToLin(9.5), 23);
        for (size_t i = 0; i < nl.size(); ++i)
            if ((i / static_cast<size_t>(0.2 * fs)) % 2) nl[i] = nr[i] = 0.0f;
        check("full-band noise bursts +9.5 dBFS", fs, lookMs, nl, nr, tp);
    }
}

void delayAndTransparency(double fs) {
    std::printf("-- delay and transparency at %.0f Hz\n", fs);
    Buffer imp = sig::silence(fs, 0.1);
    imp[100] = 0.5f;
    int look = 0;
    Out o = run(fs, 1.5, -1.0, 80, imp, imp, &look);
    CHECK(o.l[100 + static_cast<size_t>(look)] == 0.5f);
    CHECK(look == static_cast<int>(std::lround(1.5 * fs / 1000.0)));
    // below the ceiling nothing changes, bit for bit
    Buffer s = sig::sine(fs, 1000, -6, 1.0);
    o = run(fs, 1.5, -1.0, 80, s, s, &look);
    bool same = true;
    for (size_t i = 0; i < s.size(); ++i) same = same && o.l[i + static_cast<size_t>(look)] == s[i];
    CHECK(same);
    // switched off (huge ceiling): delayed input, bit for bit, also above full scale
    Buffer loud = sig::sine(fs, 1000, 6, 0.5);
    o = run(fs, 1.5, 180.0, 80, loud, loud, &look);
    same = true;
    for (size_t i = 0; i < loud.size(); ++i) same = same && o.l[i + static_cast<size_t>(look)] == loud[i];
    CHECK(same);
}

void release(double fs) {
    std::printf("-- release at %.0f Hz\n", fs);
    Limiter lim;
    lim.prepare(static_cast<int>(5e-3 * fs) + 1, 8, 96, 5.0);
    lim.setLook(static_cast<int>(std::lround(1.5e-3 * fs)));
    lim.setParams(dbToLin(-6.0), onePoleCoeff(0.080, fs));
    // hold the gain at -6 dB with a 0 dBFS DC-like level, then go quiet
    double l, r;
    for (int i = 0; i < static_cast<int>(1.5 * fs); ++i) {
        l = r = 1.0;
        lim.process(l, r);
    }
    const double g0 = lim.gain();
    CHECK_NEAR(linToDb(g0), -6.1, 0.01); // -6 dB ceiling, 0.1 dB true-peak margin
    const int window = lim.look() + 1; // window plus the detector delay before the gain may rise
    const int n = window + static_cast<int>(0.080 * fs);
    for (int i = 0; i < n; ++i) {
        l = r = 0.0;
        lim.process(l, r);
    }
    const double rest = (1.0 - lim.gain()) / (1.0 - g0); // share of the reduction left after one release time
    std::printf("   reduction left after the window + 80 ms: %.3f (one-pole: 0.368)\n", rest);
    CHECK_RANGE(rest, 0.30, 0.42);
}

} // namespace

// Which detector holds the true-peak ceiling on full-band noise (informational sweep).
void detectorSweep() {
    std::printf("-- detector sweep: full-band noise bursts +9.5 dBFS, look 2 ms, overshoot over -1 dBTP\n");
    for (double fs : {44100.0, 48000.0}) {
        Buffer nl = sig::whiteNoise(fs, 4.0, dbToLin(9.5), 11), nr = sig::whiteNoise(fs, 4.0, dbToLin(9.5), 23);
        for (size_t i = 0; i < nl.size(); ++i)
            if ((i / static_cast<size_t>(0.2 * fs)) % 2) nl[i] = nr[i] = 0.0f;
        for (int factor : {4, 8})
            for (int taps : {64, 96, 128})
                for (double beta : {4.0, 5.0, 6.0}) {
                    gFactor = factor;
                    gTaps = taps;
                    gBeta = beta;
                    Out o = run(fs, 3.0, -1.0, 80, nl, nr);
                    const double tp = std::max(refTruePeakDb(o.l), refTruePeakDb(o.r));
                    std::printf("   %5.0f Hz %2dx %2d taps beta %.0f: %+5.2f dB\n", fs, factor, taps, beta, tp + 1.0);
                }
    }
    gFactor = 8;
    gTaps = 96;
    gBeta = 5.0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--sweep") {
        detectorSweep();
        return 0;
    }
    for (double fs : {44100.0, 48000.0}) {
        hostile(fs);
        delayAndTransparency(fs);
        release(fs);
    }
    return test::finish("limiter_test");
}
