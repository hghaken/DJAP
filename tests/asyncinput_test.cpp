// A deck input on another clock (docs/design.md section 6.1): the capture side and the output side are simulated
// with their own clocks - drift in ppm, late callbacks, pauses - and the reader must hold its fill, measure the drift,
// never run dry in normal running, recover from faults, and pass the signal unchanged (bit-exact at read speed 1,
// flat and clean when it interpolates).
#include "check.h"
#include "engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <vector>

namespace {
std::atomic<long> gAllocs{0};
std::atomic<bool> gCounting{false};
void* countedAlloc(std::size_t n) {
    if (gCounting.load(std::memory_order_relaxed)) gAllocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
} // namespace
void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace ap;

namespace {

const double pi = 3.14159265358979323846;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uniform() { // [0, 1)
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
};

// The capture side and the output side, each on its own clock. Times in seconds of the shared steady clock.
struct Sim {
    double fs = 48000.0;
    double driftPpm = 0.0;    // the capture clock's rate against the output clock
    int block = 480;          // capture frames per callback
    int split = 1;            // ... delivered as this many callbacks straight after each other (a 10 ms packet in
                              // short callbacks, or a long device period in callbacks of at most 512 frames)
    std::vector<int> pieces;  // or these callback sizes (adding up to `block`), straight after each other
    int period = 256;         // output frames per period
    int playSplit = 1;        // ... asked for as this many callbacks straight after each other
    int primeCalls = 0;       // the output's first callbacks all at once (a stream filling its buffer at the start)
    double outStart = 0.0;    // s: the output starts this late (its priming after the first capture blocks)
    int firstPeriod = 0;      // the output's first callback this long instead (a DirectSound stream: its whole buffer)
    int devicePeriod = 0;     // AsyncInput::setDevicePeriod (0: not set)
    bool catchUp = false;     // the capture blocks of a capPause come all at once after it (else they are lost)
    double capJitter = 0.0;   // s: capture callbacks come up to this late
    double outJitter = 0.0;   // s: output callbacks come up to this late
    double capPause[2] = {-1, -1}, outPause[2] = {-1, -1}; // no callbacks in [from, to) (s since start)
    std::function<void(int64_t n, float& l, float& r)> signal; // capture frame n
    uint64_t seed = 1;

    std::vector<float> outL, outR;    // what the output side read
    std::vector<double> correction;   // ppm, each period after `settleAfter`
    double settleAfter = 1e9;
    double probeAt = -1.0, probed = 0.0; // the buffer (targetMs) at this time (s since start)

    double start = 1000.0; // s on the shared clock where this run begins

    void run(AsyncInput& ai, double seconds) {
        const double t0 = start;
        Rng rng(seed);
        const double capRate = fs * (1.0 + driftPpm * 1e-6);
        int64_t capIndex = 0, k = 0, m = 0;
        std::vector<float> buf(2 * static_cast<size_t>(block));
        std::vector<float> held; // capture blocks held back by a pause with catchUp
        float l[256], r[256];
        auto capTime = [&] { return t0 + static_cast<double>(k + 1) * block / capRate + capJitter * rng.uniform(); };
        auto outTime = [&] {
            if (m < primeCalls) return t0 + outStart + 2e-5 * static_cast<double>(m);
            return t0 + outStart + static_cast<double>(m - primeCalls + 1) * period / fs + outJitter * rng.uniform();
        };
        if (devicePeriod > 0) ai.setDevicePeriod(devicePeriod);
        double tc = capTime(), to = outTime();
        auto paused = [](const double p[2], double t) { return t >= p[0] && t < p[1]; };
        while (std::min(tc, to) < t0 + seconds) {
            if (tc <= to) { // a capture block is complete
                for (int i = 0; i < block; ++i) signal(capIndex++, buf[2 * static_cast<size_t>(i)], buf[2 * static_cast<size_t>(i) + 1]);
                if (paused(capPause, tc - t0)) {
                    if (catchUp) held.insert(held.end(), buf.begin(), buf.end());
                } else {
                    double t = tc;
                    for (size_t at = 0; at < held.size(); at += 2 * static_cast<size_t>(block)) { // the catch-up
                        ai.write(held.data() + at, block, t);
                        t += 2e-5;
                    }
                    held.clear();
                    if (!pieces.empty()) {
                        size_t at = 0;
                        for (int n : pieces) {
                            ai.write(buf.data() + at, n, t);
                            at += 2 * static_cast<size_t>(n);
                            t += 1e-3;
                        }
                    } else {
                        for (int part = 0; part < split; ++part) { // 1 ms apart
                            ai.write(buf.data() + 2 * static_cast<size_t>(part * block / split), block / split, t);
                            t += 1e-3;
                        }
                    }
                }
                ++k;
                tc = capTime();
            } else {
                if (!paused(outPause, to - t0)) {
                    const int part = m == 0 && firstPeriod > 0 ? firstPeriod : period / playSplit;
                    for (int p = 0; p < playSplit; ++p) {
                        ai.beginPeriod(part, to + 1e-3 * p);
                        for (int pos = 0; pos < part; pos += 256) {
                            const int n = std::min(256, part - pos);
                            ai.read(l, r, n);
                            outL.insert(outL.end(), l, l + n);
                            outR.insert(outR.end(), r, r + n);
                        }
                    }
                    if (to - t0 >= settleAfter) correction.push_back(ai.state().correctionPpm);
                    if (probeAt >= 0.0 && to - t0 >= probeAt) {
                        probed = ai.state().targetMs;
                        probeAt = -1.0;
                    }                }
                ++m;
                to = outTime();
            }
        }
    }
};

std::function<void(int64_t, float&, float&)> sine(double f, double amp, double fs) {
    return [=](int64_t n, float& l, float& r) {
        l = static_cast<float>(amp * std::sin(2.0 * pi * f * static_cast<double>(n) / fs));
        r = static_cast<float>(amp * std::cos(2.0 * pi * f * static_cast<double>(n) / fs));
    };
}

std::function<void(int64_t, float&, float&)> noise() {
    return [](int64_t n, float& l, float& r) {
        Rng a(static_cast<uint64_t>(n) * 2 + 1), b(static_cast<uint64_t>(n) * 2 + 2);
        l = static_cast<float>(a.uniform() - 0.5);
        r = static_cast<float>(b.uniform() - 0.5);
    };
}

// Least-squares fit of a sin + b cos at angular frequency w (rad / frame) over x[from, from + n): amplitude and the
// residual's rms.
void fit(const std::vector<float>& x, size_t from, size_t n, double w, double& amp, double& resRms) {
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s = std::sin(w * static_cast<double>(from + i)), c = std::cos(w * static_cast<double>(from + i));
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += x[from + i] * s;
        xc += x[from + i] * c;
    }
    const double det = ss * cc - sc * sc;
    amp = resRms = 0.0;
    if (n == 0 || !(std::fabs(det) > 0.0)) return;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double e = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s = std::sin(w * static_cast<double>(from + i)), c = std::cos(w * static_cast<double>(from + i));
        const double d = x[from + i] - (a * s + b * c);
        e += d * d;
    }
    amp = std::sqrt(a * a + b * b);
    resRms = std::sqrt(e / static_cast<double>(n));
}

// Index of the first frame that is not silent.
size_t firstSound(const std::vector<float>& x) {
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] != 0.0f) return i;
    return x.size();
}

// Frames of the fade-in when reading starts (its first frame is 0, so firstSound finds the second).
size_t fadeFrames(double fs) { return static_cast<size_t>(std::ceil(AsyncInput::kFadeInMs * fs / 1000.0)); }

void bitExact() {
    std::printf("-- read speed 1, equal clocks: the output is the input, delayed, bit-exact\n");
    struct Case {
        int block, period;
        double capJitter;
    };
    for (const Case& c : {Case{480, 256, 0.0}, Case{441, 64, 0.002}, Case{1024, 1024, 0.001}, Case{160, 2048, 0.0}}) {
        AsyncInput ai;
        ai.reset(48000);
        ai.setFixedRatio(1.0);
        Sim s;
        s.block = c.block;
        s.period = c.period;
        s.capJitter = c.capJitter;
        s.signal = noise();
        s.run(ai, 5.0);
        const size_t start = firstSound(s.outL), m0 = start + fadeFrames(48000); // after the fade-in
        // which input frame came out at m0
        int64_t n0 = -1;
        for (int64_t n = 0; n < 48000 && n0 < 0; ++n) {
            float l, r;
            s.signal(n, l, r);
            if (std::memcmp(&l, &s.outL[m0], 4) == 0) n0 = n;
        }
        size_t bad = 0, loud = 0;
        for (size_t i = start; i < s.outL.size(); ++i) {
            float l, r;
            s.signal(n0 + static_cast<int64_t>(i) - static_cast<int64_t>(m0), l, r);
            if (i < m0) { // the fade-in: never louder than the input
                if (std::fabs(s.outL[i]) > std::fabs(l) || std::fabs(s.outR[i]) > std::fabs(r)) ++loud;
            } else if (std::memcmp(&l, &s.outL[i], 4) != 0 || std::memcmp(&r, &s.outR[i], 4) != 0) {
                ++bad;
            }
        }
        const AsyncInputState st = ai.state();
        std::printf("   capture %4d frames, output period %4d: starts after %.1f ms, buffer %.1f ms, %zu of %zu frames "
                    "differ after the 10 ms fade-in (%zu louder than the input in it), underruns %lld\n",
                    c.block, c.period, start / 48.0, st.targetMs, bad, s.outL.size() - m0, loud, st.underruns);
        CHECK(n0 >= 0);
        CHECK(bad == 0);
        CHECK(loud == 0);
        CHECK(std::fabs(s.outL[start]) < 1e-3f); // the fade starts from (almost) nothing
        CHECK(st.underruns == 0 && st.overruns == 0 && st.resyncs == 0);
        CHECK(m0 < 48000 / 10); // running within 100 ms
    }
}

void driftTracking() {
    std::printf("-- clocks apart by -500 .. +500 ppm, late callbacks: fill held, drift measured, never dry\n");
    for (double d : {-500.0, -100.0, 0.0, 30.0, 500.0}) {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.driftPpm = d;
        s.capJitter = 0.002;
        s.outJitter = 0.0005;
        s.signal = sine(997.0, 0.5, 48000);
        s.settleAfter = 90.0;
        s.run(ai, 120.0);
        const AsyncInputState st = ai.state();
        double mean = 0, var = 0;
        for (double c : s.correction) mean += c;
        mean /= static_cast<double>(s.correction.size());
        for (double c : s.correction) var += (c - mean) * (c - mean);
        const double sd = std::sqrt(var / static_cast<double>(s.correction.size()));
        std::printf("   %+5.0f ppm: measured %+7.1f ppm, read speed %+7.1f ppm (sd %.1f ppm) over the last 30 s; buffer "
                    "%.2f ms, fill %.2f ms; underruns %lld, overruns %lld, resyncs %lld\n",
                    d, st.driftPpm, mean, sd, st.targetMs, st.fillMs, st.underruns, st.overruns, st.resyncs);
        CHECK(st.underruns == 0 && st.overruns == 0 && st.resyncs == 0);
        CHECK_NEAR(st.driftPpm, d, 3.0);
        CHECK_NEAR(mean, d, 3.0);
        CHECK(sd < 20.0); // pitch wobble below 0.035 cent
        CHECK_NEAR(st.fillMs, st.targetMs, 0.5);
        CHECK(st.targetMs < 20.0); // 10 ms capture blocks + 5.3 ms periods + margin
    }
}

void quality() {
    std::printf("-- interpolating (read speed 1 + 100 ppm): flat to 16 kHz, clean\n");
    for (double fs : {44100.0, 48000.0}) {
        for (double f : {100.0, 1000.0, 5000.0, 10000.0, 16000.0, 18000.0, 20000.0}) {
            AsyncInput ai;
            ai.reset(fs);
            const double ratio = 1.0001;
            ai.setFixedRatio(ratio);
            Sim s;
            s.fs = fs;
            s.driftPpm = 100.0; // the capture side keeps up
            s.signal = sine(f, 0.5, fs);
            s.run(ai, 1.5);
            const size_t m0 = firstSound(s.outL) + fadeFrames(fs) + 64; // after the fade-in
            const double w = 2.0 * pi * f / fs * ratio;
            // short windows: every fractional position is passed every 10000 frames
            double lo = 1e9, hi = -1e9;
            const size_t win = 256;
            for (size_t i = m0; i + win <= s.outL.size(); i += win) {
                double a, e;
                fit(s.outL, i, win, w, a, e);
                lo = std::min(lo, 20 * std::log10(a / 0.5));
                hi = std::max(hi, 20 * std::log10(a / 0.5));
            }
            double a, e;
            fit(s.outL, m0, 20000, w, a, e);
            const double resDb = 20 * std::log10(e / (a / std::sqrt(2.0)));
            std::printf("   %.1f kHz, %6.0f Hz: gain %+.3f .. %+.3f dB, residual %.1f dB\n", fs / 1000, f, lo, hi, resDb);
            if (f <= 16000.0) {
                CHECK(lo > -0.1 && hi < 0.1);
            }
            if (f <= 10000.0) {
                CHECK(resDb < -85.0);
            }
        }
    }
}

void faults() {
    std::printf("-- faults: the capture stops for 200 ms; the output stops for 1 s\n");
    {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.driftPpm = 50.0;
        s.capJitter = 0.002;
        s.capPause[0] = 20.0;
        s.capPause[1] = 20.2;
        s.signal = sine(440.0, 0.5, 48000);
        s.settleAfter = 30.0;
        s.run(ai, 60.0);
        const AsyncInputState st = ai.state();
        // the last 10 s play without gaps
        size_t gaps = 0;
        for (size_t i = s.outL.size() - 480000; i < s.outL.size(); ++i)
            if (s.outL[i] == 0.0f && s.outR[i] == 0.0f) ++gaps;
        std::printf("   capture pause: underruns %lld, buffer now %.2f ms, fill %.2f ms, silent frames in the last 10 s "
                    "%zu\n",
                    st.underruns, st.targetMs, st.fillMs, gaps);
        CHECK(st.underruns >= 1 && st.underruns <= 3);
        CHECK(st.active);
        CHECK(gaps == 0);
        CHECK(st.overruns == 0 && st.resyncs == 0);
        CHECK_NEAR(st.fillMs, st.targetMs, 0.5);
    }
    {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.driftPpm = -50.0;
        s.capJitter = 0.002;
        s.outPause[0] = 20.0;
        s.outPause[1] = 21.0;
        s.signal = sine(440.0, 0.5, 48000);
        s.run(ai, 60.0);
        const AsyncInputState st = ai.state();
        std::printf("   output pause: overruns %lld, resyncs %lld, underruns %lld, fill %.2f of %.2f ms\n", st.overruns,
                    st.resyncs, st.underruns, st.fillMs, st.targetMs);
        CHECK(st.overruns >= 1);
        CHECK(st.resyncs == 1);
        CHECK(st.underruns == 0);
        CHECK_NEAR(st.fillMs, st.targetMs, 0.5);
    }
    {
        // the margin a stall added comes back down (0.5 ms per calm minute), without a gap
        double base = 0.0;
        {
            AsyncInput ai;
            ai.reset(48000);
            Sim s;
            s.driftPpm = 50.0;
            s.capJitter = 0.002;
            s.signal = sine(440.0, 0.5, 48000);
            s.run(ai, 10.0);
            base = ai.state().targetMs;
        }
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.driftPpm = 50.0;
        s.capJitter = 0.002;
        s.capPause[0] = 10.0;
        s.capPause[1] = 10.2;
        s.signal = sine(440.0, 0.5, 48000);
        s.run(ai, 400.0);
        const AsyncInputState st = ai.state();
        size_t gaps = 0;
        for (size_t i = 20 * 48000; i < s.outL.size(); ++i) // from 10 s after the stall on
            if (s.outL[i] == 0.0f && s.outR[i] == 0.0f) ++gaps;
        std::printf("   after a stall: buffer %.2f ms after 400 s (%.2f without the stall), underruns %lld, silent frames "
                    "after it %zu\n",
                    st.targetMs, base, st.underruns, gaps);
        CHECK(st.underruns >= 1 && st.underruns <= 3);
        CHECK_NEAR(st.targetMs, base, 0.3);
        CHECK(gaps == 0 && st.resyncs == 0);
    }
    {
        // lasting jitter larger than the base margin: the decay must not walk back into the margin that failed
        // (an outside review measured an underrun every ~4 minutes for ever with a plain decay)
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.driftPpm = 50.0;
        s.capJitter = 0.008;
        s.signal = sine(440.0, 0.5, 48000);
        s.run(ai, 2400.0);
        const AsyncInputState st = ai.state();
        std::printf("   lasting 8 ms capture jitter, 40 minutes: underruns %lld, buffer %.2f ms\n", st.underruns,
                    st.targetMs);
        CHECK(st.underruns <= 5); // the climb to a margin that holds, one step down that failed - then none
    }
}

void bursts() {
    std::printf("-- bursts: blocks that come straight after each other count as one block\n");
    struct Case {
        int block, split, period, playSplit;
        std::vector<int> pieces;
        int device = 0;
    };
    // the input's bursts; the output's (fifth outside review: 1024 frames as two of 512 on the output side gave
    // underruns); unequal pieces (512 + 512 + 76: the quarter rule follows the last piece's length)
    for (const Case& c : {Case{480, 4, 128, 1, {}}, Case{1024, 2, 256, 1, {}}, Case{480, 1, 128, 1, {}},
                          Case{480, 1, 1024, 2, {}}, Case{1024, 2, 1024, 2, {}}, Case{1100, 1, 256, 1, {512, 512, 76}},
                          // a packet over the 50 ms cap, in callbacks of 512: the device's period sets the cap then
                          // (sixth outside review: 7 underruns with the cap alone)
                          Case{3072, 6, 512, 1, {}, 3072}}) {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.block = c.block;
        s.split = c.split;
        s.pieces = c.pieces;
        s.period = c.period;
        s.playSplit = c.playSplit;
        s.devicePeriod = c.device;
        s.driftPpm = 50.0;
        s.capJitter = 0.001;
        s.signal = sine(440.0, 0.5, 48000);
        s.run(ai, 60.0);
        const AsyncInputState st = ai.state();
        const double least = (c.block + c.period) / 48.0; // ms: the whole burst and the output period
        std::printf("   %d frames as %d callbacks, output %d frames as %d: buffer %.2f ms (at least %.2f), underruns %lld\n",
                    c.block, c.pieces.empty() ? c.split : static_cast<int>(c.pieces.size()), c.period, c.playSplit,
                    st.targetMs, least, st.underruns);
        // an outside review measured 5-6 underruns after the start when only the callback's size counted
        CHECK(st.underruns == 0);
        CHECK(st.targetMs > least);
        CHECK(st.active && st.overruns == 0 && st.resyncs == 0);
    }
}

void stallCatchUp() {
    std::printf("-- a stall whose blocks come all at once afterwards: the buffer goes back to where it was\n");
    double base = 0.0;
    for (const bool stall : {false, true}) {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.driftPpm = 50.0;
        s.capJitter = 0.001;
        s.catchUp = true;
        if (stall) {
            s.capPause[0] = 20.0;
            s.capPause[1] = 20.1; // 100 ms: ten blocks of 480 frames, then all of them at once
        }
        s.signal = sine(440.0, 0.5, 48000);
        s.probeAt = 30.0;
        s.settleAfter = 45.0; // the read speed from 45 s on: the burst is forgotten in there
        // 330 s: the burst is forgotten after 20-40 s, the margin the stall's underrun added after 4 minutes
        s.run(ai, stall ? 330.0 : 31.0);
        const double at30 = s.probed;
        if (!stall) {
            base = at30;
            continue;
        }
        const AsyncInputState st = ai.state();
        double fastest = 0.0;
        for (double c : s.correction) fastest = std::max(fastest, std::fabs(c));
        std::printf("   100 ms stall at 20 s: buffer %.2f ms at 30 s, %.2f ms 5 minutes later (%.2f without the stall); "
                    "underruns %lld, resyncs %lld; read speed at most %.0f ppm off while it comes down\n", at30,
                    st.targetMs, base, st.underruns, st.resyncs, fastest);
        // sixth outside review: the target fell 40 ms in one step and the read speed jumped to +2000 ppm (3.5 cents)
        CHECK(fastest < 700.0);
        // fifth outside review: the catch-up counted as one block for good - 0.1.148 stayed at about 125 ms
        CHECK(at30 < base + 60.0); // the burst is capped (50 ms)
        CHECK_NEAR(st.targetMs, base, 0.3);
        CHECK(st.active && st.underruns <= 2);
    }
}

// An underrun ends with a fade: what is still there, then the last frame held - no step from music to silence, at
// whatever moment it comes (sixth outside review: with nearly nothing left in the ring the step came back - 0.7 to
// 11 % of 300 moments, by the output period).
void fadeOut() {
    std::printf("-- an underrun fades out, at any moment\n");
    // a 1 kHz sine of 0.5 moves at most 0.5 x 2 pi x 1000 / 48000 = 0.065 per frame; a cut to silence is up to 0.5
    const double sineStep = 0.5 * 2.0 * pi * 1000.0 / 48000.0;
    for (const int period : {64, 128, 480}) {
        double worst = 0.0;
        long long underruns = 0;
        for (int k = 0; k < 30; ++k) {
            AsyncInput ai;
            ai.reset(48000);
            Sim s;
            s.period = period;
            s.capPause[0] = 3.0 + 0.000731 * k; // 30 moments within a capture block
            s.capPause[1] = s.capPause[0] + 0.2;
            s.signal = sine(1000.0, 0.5, 48000);
            s.run(ai, 3.5);
            for (size_t i = 48000; i < s.outL.size(); ++i)
                worst = std::max(worst, static_cast<double>(std::fabs(s.outL[i] - s.outL[i - 1])));
            underruns += ai.state().underruns;
        }
        std::printf("   output period %4d, 30 moments: largest step %.2f x the sine's own, %lld underruns\n", period,
                    worst / sineStep, underruns);
        CHECK(underruns >= 30);
        CHECK(worst < 1.25 * sineStep);
    }
    // the fade's length and both channels (seventh outside review: the right channel's hold and the fade's length were
    // not checked): a steady level, unequal in L and R, fades as one raised cosine over kFadeOutMs
    for (const int period : {64, 480}) {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.period = period;
        s.capPause[0] = 3.0;
        s.capPause[1] = 3.2;
        s.signal = [](int64_t, float& l, float& r) {
            l = 0.5f;
            r = -0.25f;
        };
        s.run(ai, 3.5);
        size_t from = 0, to = 0; // the fade: the first frame under the level after 1 s, the last one not silent
        for (size_t i = 48000; i < s.outL.size() && !from; ++i)
            if (s.outL[i] < 0.4999f) from = i;
        for (size_t i = from; from && i < s.outL.size() && s.outL[i] != 0.0f; ++i) to = i;
        double worstLR = 0.0;
        for (size_t i = from; from && i <= to; ++i)
            worstLR = std::max(worstLR, static_cast<double>(std::fabs(s.outR[i] + 0.5f * s.outL[i])));
        const double ms = from ? static_cast<double>(to - from + 2) / 48.0 : 0.0;
        std::printf("   output period %4d: fade-out over %.2f ms (%.1f ms set), R follows L within %.1e\n", period, ms,
                    AsyncInput::kFadeOutMs, worstLR);
        CHECK(from > 0 && ai.state().underruns >= 1);
        CHECK_NEAR(ms, 3.0, 0.1); // the design's 3 ms (not the constant itself: a changed constant must show)
        CHECK(worstLR < 1e-5);
        // its shape: a raised cosine stands at 0.85 a quarter of the way down (a straight line: 0.75); and the
        // fade-in when reading starts again lasts 10 ms (eighth outside review: neither was pinned)
        const double quarter = from + 35 < s.outL.size() ? s.outL[from + 35] / 0.5 : 0.0;
        size_t back = 0, full = 0;
        for (size_t i = to + 1; i < s.outL.size() && !back; ++i)
            if (s.outL[i] != 0.0f) back = i;
        for (size_t i = back; back && i < s.outL.size() && !full; ++i)
            if (s.outL[i] >= 0.4999f) full = i;
        const double fadeIn = full ? static_cast<double>(full - back + 1) / 48.0 : 0.0;
        std::printf("   a quarter into the fade-out at %.3f of the level; fade-in over %.2f ms\n", quarter, fadeIn);
        // a raised cosine there: 0.848 (a straight line 0.75, a square-root curve 0.863 - ninth review)
        CHECK(quarter > 0.840 && quarter < 0.856);
        CHECK_NEAR(fadeIn, 10.0, 0.3);
    }
}

// A stream fills its buffer at the start with callbacks straight after each other: not a burst (sixth outside
// review: the buffer stood 20 ms higher for 20-40 s after every start), whether the output starts before the first
// capture block or after it (seventh review: "no input yet" held only in the first order).
void priming() {
    std::printf("-- the output's first callbacks all at once (filling its buffer) do not raise the buffer\n");
    for (const double start : {0.0, 0.0125, 0.025}) { // before the first capture block (at 10 ms), 2.5 ms after, 15 ms after
        double t[2] = {0.0, 0.0};
        for (int i = 0; i < 2; ++i) {
            AsyncInput ai;
            ai.reset(48000);
            Sim s;
            s.primeCalls = i == 0 ? 0 : 3;
            s.outStart = start;
            s.signal = sine(440.0, 0.5, 48000);
            s.probeAt = 10.0;
            s.run(ai, 11.0);
            t[i] = s.probed;
            CHECK(ai.state().underruns == 0);
        }
        std::printf("   output starting at %4.1f ms: buffer at 10 s %.2f ms, with 3 periods at once at the start %.2f ms\n",
                    start * 1000.0, t[0], t[1]);
        CHECK_NEAR(t[1], t[0], 0.1);
    }
    // eighth outside review: a DirectSound stream's first callback is its whole buffer (here 3 periods) - as a burst it
    // held the target 20 ms higher for 40-65 s; and the same AsyncInput started again (the program reuses it at every
    // start: reset() must forget the gaps, else the second start's priming counts again)
    double base = 0.0;
    for (int c = 0; c < 3; ++c) {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.signal = sine(440.0, 0.5, 48000);
        s.probeAt = 10.0;
        if (c == 1) s.firstPeriod = 3 * s.period;
        if (c == 2) { // a first run, then reset and a late priming - later on the clock, as a restart is (ninth
                      // review: on the same clock a reset that kept the first run's start time went unnoticed)
            s.run(ai, 3.0);
            ai.reset(48000);
            s = Sim();
            s.start = 1600.0;
            s.signal = sine(440.0, 0.5, 48000);
            s.probeAt = 10.0;
            s.outStart = 0.025;
            s.primeCalls = 3;
        }
        s.run(ai, 11.0);
        if (c == 0) base = s.probed;
        std::printf("   %s: buffer at 10 s %.2f ms (%.2f without), underruns %lld\n",
                    c == 0 ? "plain start" : c == 1 ? "first callback 3 periods long" : "second start after reset, late priming",
                    s.probed, base, ai.state().underruns);
        CHECK_NEAR(s.probed, base, 0.1);
        CHECK(ai.state().underruns == 0);
    }
    // an output whose callbacks never pause (faster than real time for good): reading starts after kPrimeTimeoutS
    // anyway instead of never (eighth outside review)
    {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.signal = sine(440.0, 0.5, 48000);
        s.period = 16;          // (short: the run makes some 60000 callbacks)
        s.primeCalls = 1000000; // every callback 20 us after the last
        s.run(ai, 1.0 + AsyncInput::kPrimeTimeoutS);
        const size_t first = firstSound(s.outL);
        // callbacks of 16 frames every 20 us: 800 000 frames a second of the clock
        const double after = static_cast<double>(first) / (s.period / 2e-5);
        std::printf("   an output that never pauses: sound after %.3f s (%zu of %zu frames)\n", after, first, s.outL.size());
        CHECK(first < s.outL.size());
        CHECK_NEAR(after, 1.0, 0.02); // kPrimeTimeoutS itself (ninth review: 0.1 s and 10 s went unnoticed)
    }
}

// The input's block size as Health shows it, and blocks read twice (ninth outside review: the largest burst went over
// Health's 10 ms line after one short stall; a repeated block was not noticed at all).
void blockSizes() {
    std::printf("-- the steady block size and repeated blocks\n");
    struct Case {
        int block, split;
        double pause;
        double steady;
    };
    for (const Case& c : {Case{128, 1, 0.008, 128 / 48.0}, Case{480, 4, 0.0, 10.0}, Case{177, 1, 0.015, 177 / 48.0}}) {
        AsyncInput ai;
        ai.reset(48000);
        Sim s;
        s.block = c.block;
        s.split = c.split;
        s.catchUp = true;
        if (c.pause > 0.0) {
            s.capPause[0] = 5.0;
            s.capPause[1] = 5.0 + c.pause;
        }
        s.signal = sine(440.0, 0.5, 48000);
        s.run(ai, 8.0);
        const AsyncInputState st = ai.state();
        std::printf("   %d frames%s%s: steady %.2f ms (expected %.2f), largest %.2f ms, repeats %lld\n", c.block,
                    c.split > 1 ? " in 4 callbacks" : "", c.pause > 0.0 ? ", one stall" : "", st.steadyMs, c.steady,
                    st.blockMs, st.repeats);
        CHECK_NEAR(st.steadyMs, c.steady, 0.05);
        CHECK(st.repeats == 0);
        if (c.pause > 0.0) CHECK(st.blockMs > c.steady + 4.0); // the stall's catch-up: the largest, not the steady
    }
}

// A writer test without its own output: an output period of the same length after each block, so the ring never
// fills (a full ring drops the blocks before they are looked at).
void drained(AsyncInput& in, const float* x, int n, double t) {
    static std::vector<float> l, r;
    l.resize(static_cast<size_t>(n));
    r.resize(static_cast<size_t>(n));
    in.write(x, n, t);
    in.beginPeriod(n, t);
    in.read(l.data(), r.data(), n);
}

// Blocks read twice (tenth outside review: DC, a test tone in whole periods and clipped plateaus counted too, and the
// count never went away). `blocks`: a letter per block - a..e different sound, z silence, k DC, digits a 16-frame
// block (0) or a 15-frame one (5), t a 1 kHz tone of 48 frames (one period: every block the same).
long long repeatsOf(const char* blocks, int frames = 64) {
    AsyncInput r;
    r.reset(48000);
    double t = 1.0;
    for (const char* p = blocks; *p; ++p) {
        const int n = *p == '0' ? 16 : *p == '5' ? 15 : *p == 't' ? 48 : frames;
        std::vector<float> x(2 * static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const size_t k = 2 * static_cast<size_t>(i);
            if (*p >= 'a' && *p <= 'e') { // sound, L and R unequal
                x[k] = static_cast<float>(0.5 * std::sin(0.1 * i + (*p - 'a')));
                x[k + 1] = static_cast<float>(0.4 * std::sin(0.13 * i + (*p - 'a')));
            } else if (*p == 'k') {
                x[k] = x[k + 1] = -1.0f / 32768.0f; // an idle input's offset of -1 LSB
            } else if (*p == 't' || *p == '0' || *p == '5') {
                x[k] = x[k + 1] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * i / 48.0));
            }
        }
        r.write(x.data(), n, t += 0.01);
    }
    return r.state().repeats;
}

void repeatedBlocks() {
    std::printf("-- repeated blocks\n");
    struct Case {
        const char* blocks;
        long long expected;
        const char* what;
    };
    const Case cases[] = {
        {"abbc", 1, "a block read twice"},
        {"abbbc", 1, "three times"},
        {"abbbbc", 1, "four times"},
        {"abbbbbc", 0, "five times: a period, not a read"},
        {"abb", 0, "twice, nothing after it yet"},
        {"abbckkc", 1, "and DC twice (flat)"},
        {"azzzc", 0, "silence"},
        {"akkkkkkkc", 0, "DC of -1 LSB"},
        {"atttttttttttttttttttc", 0, "a tone of whole periods per block"},
        {"abbcbbdeeb", 3, "three reads twice"},
        {"a00c", 1, "16 frames"},
        {"a55c", 0, "15 frames: too short to tell"},
    };
    for (const Case& c : cases) {
        const long long got = repeatsOf(c.blocks);
        std::printf("   %-22s %-36s repeats %lld (expected %lld)\n", c.blocks, c.what, got, c.expected);
        CHECK(got == c.expected);
    }
    // only one channel differs: not the same block
    {
        std::vector<float> a(128), b(128);
        for (int i = 0; i < 64; ++i) {
            a[2 * static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(0.2 * i));
            a[2 * static_cast<size_t>(i) + 1] = static_cast<float>(0.3 * std::cos(0.2 * i));
            b[2 * static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(0.3 * i));
            b[2 * static_cast<size_t>(i) + 1] = static_cast<float>(0.2 * std::cos(0.3 * i));
        }
        std::vector<float> l = a, rr = a;
        l[40] += 0.001f;  // only the left channel differs
        rr[41] += 0.001f; // only the right
        for (const std::vector<float>* odd : {&l, &rr}) {
            AsyncInput q;
            q.reset(48000);
            double t = 1.0;
            const std::vector<float>* order[] = {&b, &a, odd, &b};
            for (const std::vector<float>* v : order) q.write(v->data(), 64, t += 0.01);
            std::printf("   %s channel differs: repeats %lld (expected 0)\n", odd == &l ? "the left" : "the right",
                        q.state().repeats);
            CHECK(q.state().repeats == 0);
        }
    }
    // the recent count: the window rolls, a reset clears all
    {
        AsyncInput r;
        r.reset(48000);
        std::vector<float> a(128), b(128);
        for (int i = 0; i < 64; ++i)
            for (int c = 0; c < 2; ++c) {
                a[2 * static_cast<size_t>(i) + c] = static_cast<float>(0.4 * std::sin(0.1 * i + c));
                b[2 * static_cast<size_t>(i) + c] = static_cast<float>(0.4 * std::cos(0.1 * i + c));
            }
        double t = 1.0;
        for (int k = 0; k < 4; ++k) { // four reads twice at the start
            r.write(a.data(), 64, t += 0.01);
            r.write(a.data(), 64, t += 0.01);
            r.write(b.data(), 64, t += 0.01);
        }
        const long long early = r.state().recentRepeats;
        for (int k = 0; k < 4500; ++k) drained(r, (k % 2 ? b : a).data(), 64, t += 0.01); // 45 s, nothing repeated
        const AsyncInputState s = r.state();
        std::printf("   recent: %lld at first, %lld after 45 s without (all %lld)\n", early, s.recentRepeats, s.repeats);
        CHECK(early == 4);
        CHECK(s.recentRepeats == 0);
        CHECK(s.repeats == 4);
        drained(r, a.data(), 64, t += 0.01);
        drained(r, a.data(), 64, t += 0.01); // a run open at the reset ...
        r.reset(48000);
        CHECK(r.state().repeats == 0 && r.state().recentRepeats == 0);
        r.write(a.data(), 64, t += 0.01); // ... is forgotten: nothing left over inside, the next blocks start from 0
        r.write(b.data(), 64, t += 0.01);
        CHECK(r.state().recentRepeats == 0 && r.state().repeats == 0);
    }
}

// The steady size is the median burst of the last 20-40 s (tenth outside review: the smallest let one short burst
// show for up to 40 s; the windows were never rolled in a test).
void steadyMedian() {
    std::printf("-- the steady block size over the windows\n");
    for (double fs : {48000.0, 44100.0}) {
        AsyncInput r;
        r.reset(fs);
        const int n4 = static_cast<int>(std::lround(0.004 * fs)), n10 = static_cast<int>(std::lround(0.010 * fs));
        std::vector<float> x(2 * static_cast<size_t>(n10) + 64);
        for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.1 * std::sin(0.01 * static_cast<double>(i)));
        double t = 1.0;
        for (int k = 0; k < 2500; ++k) drained(r, x.data(), n10, t += 0.010); // 25 s of 10 ms blocks
        const double tenMs = r.state().steadyMs;
        drained(r, x.data(), 16, t += 0.010); // one short burst, a gap on each side
        t += 0.002;
        // then 3 s of 4 ms blocks: more than the 10 ms ones of the current window, fewer than of both windows
        for (int k = 0; k < 750; ++k) drained(r, x.data(), n4, t += 0.004);
        const double mixed = r.state().steadyMs;
        for (int k = 0; k < 12000; ++k) drained(r, x.data(), n4, t += 0.004); // 48 s: both windows hold 4 ms only
        const double fourMs = r.state().steadyMs;
        std::printf("   %.1f kHz: 10 ms blocks %.2f ms; one 16-frame burst and 3 s of 4 ms: %.2f ms; 48 s later %.2f ms\n",
                    fs / 1000.0, tenMs, mixed, fourMs);
        CHECK_NEAR(tenMs, 1000.0 * n10 / fs, 0.01);
        CHECK_NEAR(mixed, 1000.0 * n10 / fs, 0.01); // the median: one short burst and fewer 4 ms ones do not move it
        CHECK_NEAR(fourMs, 1000.0 * n4 / fs, 0.01);
    }
}

// Health's notes on an input under the ASIO output, as a table (tenth outside review).
void healthNotes() {
    std::printf("-- Health's notes on the input\n");
    struct Row {
        double steady, block;
        long long repeats;
        bool lowLatency;
        double requested, onStep, deflt;
        bool rateDiffers;
        const char* expect; // a part the notes must hold ("" = no notes)
    };
    const Row rows[] = {
        {4.00, 4.00, 0, true, 4, 4.00, 10.0, false, ""},                                   // as asked
        {10.00, 10.00, 0, false, 0, 0, 0, false, ""},                                      // Windows' default chosen
        {10.00, 10.00, 0, true, 4, 4.00, 10.0, false, "still comes in 10.0 ms blocks"},    // the mode not taken
        {9.80, 9.80, 0, true, 4, 4.00, 10.0, false, "still comes in 9.8 ms blocks"},       // within 0.3 of the default
        {9.60, 9.60, 0, true, 4, 4.00, 10.0, false, "input period 9.60 ms (4.00 asked)"},  // neither
        {9.00, 9.00, 0, true, 8, 9.00, 10.0, false, "input period 9.00 ms (8 asked: the device's own step)"},
        {5.33, 5.33, 0, true, 5, 5.33, 10.0, false, "(5 asked: the device's own step)"},
        {5.03, 5.03, 0, true, 5, 5.03, 10.0, false, ""},                                   // within 0.05 of the ask
        {4.29, 4.29, 0, true, 4, 4.00, 10.0, false, ""},                                   // within 0.3 of the step
        {4.31, 4.31, 0, true, 4, 4.00, 10.0, false, "input period 4.31 ms (4.00 asked)"},
        {10.16, 10.16, 0, true, 8, 0.0, 10.16, false, "the device's step allows no period from 8 ms"},
        {10.00, 10.00, 0, true, 4, 0.0, 10.0, true, "set to 48 kHz in Windows' sound settings"},
        {10.00, 10.00, 0, true, 4, 0.0, 0.0, false, "offers Windows no shorter one"},
        {10.00, 10.00, 0, false, 4, 4.0, 10.0, false, ""},                                 // exclusive: no advice
        {4.00, 6.00, 0, true, 4, 4.00, 10.0, false, ""},                                   // bursts 1.5 x: not yet
        {4.00, 6.10, 0, true, 4, 4.00, 10.0, false, "bursts of up to 6.1 ms"},
        {4.00, 4.00, 2, true, 4, 4.00, 10.0, false, ""},                                   // two repeats: not shown
        {4.00, 4.00, 3, true, 4, 4.00, 10.0, false, "3 input blocks came twice lately"},
        {0.00, 0.00, 0, true, 4, 4.00, 10.0, false, ""},                                   // nothing measured yet
    };
    for (const Row& r : rows) {
        InputNoteFacts f;
        f.steadyMs = r.steady;
        f.blockMs = r.block;
        f.recentRepeats = r.repeats;
        f.lowLatency = r.lowLatency;
        f.requestedMs = r.requested;
        f.onStepMs = r.onStep;
        f.defaultMs = r.deflt;
        f.rateDiffers = r.rateDiffers;
        f.sampleRate = 48000;
        const std::string notes = inputNotes(f);
        const bool ok = *r.expect ? notes.find(r.expect) != std::string::npos : notes.empty();
        std::printf("   steady %5.2f ask %g on step %5.2f default %5.2f: %s%s\n", r.steady, r.requested, r.onStep,
                    r.deflt, notes.empty() ? "(nothing)" : notes.c_str() + 2, ok ? "" : "   <- WRONG");
        CHECK(ok);
    }
}

void throughEngine() {
    std::printf("-- the engine: deck A from an async input, deck B from the device; bypass exact, no allocation\n");
    const double fs = 48000;
    AudioEngine e;
    e.prepare(fs);
    DeckControl off;
    off.processing = false;
    off.lookMs = 1.5;
    e.deck(0).publish(makeDeckConfig(defaultValues(), off, fs));
    e.deck(1).publish(makeDeckConfig(defaultValues(), off, fs));
    float z = 0;
    e.deck(0).process(&z, &z, 0);
    e.deck(1).process(&z, &z, 0);
    AsyncInput ai;
    ai.reset(fs);
    ai.setFixedRatio(1.0);
    e.setAsyncInput(0, &ai);
    CHECK(e.asyncInput(0) == &ai && e.asyncInput(1) == nullptr);

    const int period = 128, block = 480;
    float inB[2][period], out[4][period];
    const void* in[4] = {nullptr, nullptr, inB[0], inB[1]};
    void* outp[4] = {out[0], out[1], out[2], out[3]};
    const SampleFormat f[4] = {SampleFormat::Float32, SampleFormat::Float32, SampleFormat::Float32,
                               SampleFormat::Float32};
    auto sig = noise();
    std::vector<float> cap(2 * block), aL, bL, bIn;
    int64_t capIndex = 0, m = 0, k = 0;
    const double t0 = 500.0;
    size_t lat = static_cast<size_t>(e.deck(0).latency());
    while (m * period < 5 * 48000) {
        const double tc = t0 + static_cast<double>(k + 1) * block / fs, to = t0 + static_cast<double>(m) * period / fs;
        if (tc <= to) {
            for (int i = 0; i < block; ++i) sig(capIndex++, cap[2 * static_cast<size_t>(i)], cap[2 * static_cast<size_t>(i) + 1]);
            ai.write(cap.data(), block, tc);
            ++k;
        } else {
            for (int i = 0; i < period; ++i) {
                inB[0][i] = static_cast<float>(std::sin(0.01 * static_cast<double>(m * period + i)) * 0.3);
                inB[1][i] = -inB[0][i];
                bIn.push_back(inB[0][i]);
            }
            gCounting = true;
            e.process(in, f, outp, f, period, to);
            gCounting = false;
            aL.insert(aL.end(), out[0], out[0] + period);
            bL.insert(bL.end(), out[2], out[2] + period);
            ++m;
        }
    }
    // deck B: the device input, delayed by the look-ahead, exactly
    size_t badB = 0;
    for (size_t i = std::max(lat, static_cast<size_t>(0.005 * fs)); i < bL.size(); ++i) // after the deck's 5 ms start fade
        if (bL[i] != bIn[i - lat]) ++badB;
    // deck A: the capture, delayed (buffer + look-ahead), exactly
    const size_t m0 = firstSound(aL) + fadeFrames(fs); // after the fade-in
    int64_t n0 = -1;
    for (int64_t n = 0; n < 48000 && n0 < 0; ++n) {
        float l, r;
        sig(n, l, r);
        if (l == aL[m0]) n0 = n;
    }
    size_t badA = 0;
    for (size_t i = m0; i < aL.size(); ++i) {
        float l, r;
        sig(n0 + static_cast<int64_t>(i - m0), l, r);
        if (l != aL[i]) ++badA;
    }
    std::printf("   deck A: %zu of %zu frames differ (input buffer %.1f ms); deck B: %zu differ; %ld allocations\n", badA,
                aL.size() - m0, ai.state().targetMs, badB, gAllocs.load());
    CHECK(n0 >= 0 && badA == 0);
    CHECK(badB == 0);
    CHECK(gAllocs.load() == 0);
    e.prepare(fs); // a new start forgets the async input
    CHECK(e.asyncInput(0) == nullptr);
}

} // namespace

int main() {
    bitExact();
    driftTracking();
    quality();
    faults();
    bursts();
    stallCatchUp();
    fadeOut();
    priming();
    blockSizes();
    repeatedBlocks();
    steadyMedian();
    healthNotes();
    throughEngine();
    return test::finish("asyncinput_test");
}
