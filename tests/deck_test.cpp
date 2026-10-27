// The whole deck: bit-exact bypass and neutral chain, bad input, MXCSR, no allocation on the audio path, no clicks
// on changes, deck independence and latency through renderOffline, CPU, and the shipped presets (handbook 11, 16).
#include "check.h"
#include "deckrun.h"
#include "render.h"

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <xmmintrin.h>

// ---- allocation counter: every global operator new while counting is on ----
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
using sig::Buffer;
using tdeck::allOff;
using tdeck::Stereo;

namespace {

const double fs = 48000.0;
bool gVerbose = false;

size_t frames(double seconds) { return static_cast<size_t>(seconds * fs + 0.5); }

// A configuration published at a frame (at a block boundary, as from the GUI between two audio blocks).
struct Change {
    size_t at;
    PresetValues v;
    DeckControl ctl;
};

// Runs one deck over a stereo signal in blocks of `chunk`, publishing `changes` on the way. The output keeps the
// look-ahead delay (like a live device); `lat` receives the latency at the start.
Stereo runLive(const PresetValues& v0, const DeckControl& c0, const Buffer& l, const Buffer& r,
               const std::vector<Change>& changes = {}, int chunk = 256, int* lat = nullptr) {
    DeckProcessor d;
    d.prepare(fs);
    d.publish(makeDeckConfig(v0, c0, fs));
    d.process(nullptr, nullptr, 0);
    if (lat) *lat = d.latency();
    Stereo o{l, r};
    size_t pos = 0, next = 0;
    while (pos < o.l.size()) {
        while (next < changes.size() && changes[next].at <= pos) {
            d.publish(makeDeckConfig(changes[next].v, changes[next].ctl, fs));
            ++next;
        }
        size_t end = std::min(o.l.size(), pos + static_cast<size_t>(chunk));
        if (next < changes.size() && changes[next].at < end) end = changes[next].at;
        d.process(o.l.data() + pos, o.r.data() + pos, static_cast<int>(end - pos));
        d.meters().skipAll();
        pos = end;
    }
    return o;
}

// Frames from `from` on where out[i] != in[i - delay] (bit for bit; before the delay the output must be 0).
size_t mismatches(const Buffer& in, const Buffer& out, int delay, size_t from = 0) {
    size_t bad = 0;
    for (size_t i = from; i < out.size(); ++i) {
        const float want = i >= static_cast<size_t>(delay) ? in[i - static_cast<size_t>(delay)] : 0.0f;
        if (std::memcmp(&want, &out[i], sizeof(float)) != 0) ++bad;
    }
    return bad;
}

Buffer music(double seconds, uint64_t seed, double scale) {
    Buffer b = sig::beatMusic(fs, seconds, 124, seed);
    for (float& x : b) x = static_cast<float>(x * scale);
    return b;
}

// ---- fast math of the gain computers ----

void fastMath() {
    std::printf("-- fast exp2 / log2 / tanh against the library\n");
    double e2 = 0, l2 = 0, th = 0, db = 0;
    for (double x = -60.0; x <= 60.0; x += 0.001237) e2 = std::max(e2, std::fabs(fastExp2(x) / std::exp2(x) - 1.0));
    for (double x = 1e-12; x < 1e6; x *= 1.0137) l2 = std::max(l2, std::fabs(fastLog2(x) - std::log2(x)));
    for (double x = -25.0; x <= 25.0; x += 0.000913) th = std::max(th, std::fabs(fastTanh(x) - std::tanh(x)));
    for (double x = -120.0; x <= 40.0; x += 0.0071) db = std::max(db, std::fabs(linToDb(fastDbToLin(x)) - x));
    std::printf("   exp2 relative %.2g, log2 absolute %.2g, tanh absolute %.2g, dB round trip %.2g dB\n", e2, l2, th, db);
    CHECK(e2 < 2e-7);
    CHECK(l2 < 1e-9);
    CHECK(th < 2e-7);
    CHECK(db < 1e-5);
    CHECK(fastExp2(-2000.0) == 0.0 && fastLog2(0.0) == -1000.0 && fastTanh(100.0) == 1.0 && fastTanh(-100.0) == -1.0);
}

// The amplitude of frequency f (Hz) in x[from, from + n) at fs (an exact bin: whole periods in n).
double toneAt(const std::vector<double>& x, size_t from, size_t n, double f) {
    std::complex<double> acc = 0.0;
    for (size_t i = 0; i < n; ++i)
        acc += x[from + i] * std::polar(1.0, -2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / fs);
    return 2.0 * std::abs(acc) / static_cast<double>(n);
}

// The 2x oversampler of the clipper and the exciter: flat, a few frames of delay, and the overtones a nonlinearity
// makes above the Nyquist frequency no longer fold back into the audio band.
void oversampler() {
    std::printf("-- 2x oversampler (polyphase IIR halfbands)\n");
    const double* k = halfbandCoefs();
    for (int i = 1; i < kHalfbandCoefs; ++i) CHECK(k[i] > k[i - 1] && k[i] > 0.0 && k[i] < 1.0);
    // up and down without a stage between: gain and delay
    const size_t n = 48000;
    for (double f : {100.0, 1000.0, 10000.0, 18000.0, 20000.0, 22000.0}) {
        StereoOversampler os;
        os.setFactor(2);
        std::vector<double> in(2 * n), out(2 * n);
        for (size_t i = 0; i < 2 * n; ++i) {
            in[i] = 0.5 * std::sin(2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / fs);
            Pair ab[2];
            os.up(makePair(in[i], in[i]), ab);
            out[i] = leftOf(os.down(ab));
        }
        const double gainDb = 20.0 * std::log10(toneAt(out, n, n, f) / 0.5);
        // delay from the phase (in frames)
        std::complex<double> ci = 0.0, co = 0.0;
        for (size_t i = n; i < 2 * n; ++i) {
            const std::complex<double> e = std::polar(1.0, -2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / fs);
            ci += in[i] * e;
            co += out[i] * e;
        }
        double ph = std::arg(ci) - std::arg(co);
        while (ph < 0.0) ph += 2.0 * 3.14159265358979323846;
        const double delay = ph / (2.0 * 3.14159265358979323846 * f) * fs;
        std::printf("   %5.0f Hz: %+.5f dB, delay %.2f frames\n", f, gainDb, delay);
        if (f <= 20000.0) CHECK(std::fabs(gainDb) < 0.001);
        if (f <= 1000.0) CHECK(delay < 4.0);
    }
    // a hard-driven 13 kHz tone through tanh: its 3rd overtone (39 kHz) folds back to 9 kHz at 48 kHz without
    // oversampling; with it, it is removed
    auto aliasDb = [&](int factor, double f, double alias) {
        StereoOversampler os;
        os.setFactor(factor);
        std::vector<double> out(2 * n);
        for (size_t i = 0; i < 2 * n; ++i) {
            const double x = 0.9 * std::sin(2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / fs);
            if (factor == 1) {
                out[i] = std::tanh(3.0 * x) / 3.0;
            } else {
                Pair p[StereoOversampler::kMaxFactor];
                os.up(makePair(x, x), p);
                for (int k = 0; k < os.factor(); ++k)
                    p[k] = makePair(std::tanh(3.0 * leftOf(p[k])) / 3.0, std::tanh(3.0 * rightOf(p[k])) / 3.0);
                out[i] = leftOf(os.down(p));
            }
        }
        return 20.0 * std::log10(toneAt(out, n, n, alias) / toneAt(out, n, n, f));
    };
    const double plain = aliasDb(1, 13000.0, 9000.0), over = aliasDb(2, 13000.0, 9000.0);
    std::printf("   tanh, 13 kHz tone: alias at 9 kHz %.1f dBc without, %.1f dBc with 2x oversampling\n", plain, over);
    CHECK(plain > -40.0); // the fold-back the review found
    CHECK(over < plain - 40.0);
    // 15 kHz: its 5th overtone (75 kHz) folds back inside the doubled band at 2x (to 21 kHz), its 7th (105 kHz) to
    // 9 kHz; 4x and 8x take them away
    for (int factor : {2, 4, 8}) {
        const double a21 = aliasDb(factor, 15000.0, 21000.0), a9 = aliasDb(factor, 15000.0, 9000.0);
        std::printf("   tanh, 15 kHz tone, %dx: fold-back at 21 kHz %.1f dBc, at 9 kHz %.1f dBc\n", factor, a21, a9);
        if (factor >= 4) CHECK(std::max(a21, a9) < -70.0);
    }
    // 4x and 8x are as flat and add little delay
    for (int factor : {4, 8}) {
        StereoOversampler os;
        os.setFactor(factor);
        std::vector<double> in(2 * n), out(2 * n);
        for (size_t i = 0; i < 2 * n; ++i) {
            in[i] = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i) / fs);
            Pair p[StereoOversampler::kMaxFactor];
            os.up(makePair(in[i], in[i]), p);
            out[i] = leftOf(os.down(p));
        }
        const double g1 = 20.0 * std::log10(toneAt(out, n, n, 1000.0) / 0.5);
        std::printf("   %dx: 1 kHz %+.5f dB\n", factor, g1);
        CHECK(std::fabs(g1) < 0.001);
    }
}

// The delay the chain adds beyond the look-ahead, per frequency (informational; the docs quote it): the IIR filters
// of the density bands, the width split and the oversampled clipper shift low frequencies later. Group delay from
// the phase of two close tones (a quiet level: the dynamics hold still), each preset at 48 kHz.
void groupDelay() {
    std::printf("-- group delay beyond the look-ahead (ms; from two tones 2 %% apart, each in its own run, -30 dBFS)\n");
    const double pi = 3.14159265358979323846;
    const size_t n = frames(6.0), win = frames(1.0);
    auto phaseAt = [&](const Buffer& x, size_t from, double f) {
        std::complex<double> acc = 0.0;
        for (size_t i = 0; i < win; ++i) acc += static_cast<double>(x[from + i]) * std::polar(1.0, -2.0 * pi * f * static_cast<double>(i) / fs);
        return std::arg(acc);
    };
    auto wrap = [&](double p) {
        while (p > pi) p -= 2.0 * pi;
        while (p < -pi) p += 2.0 * pi;
        return p;
    };
    for (int preset = 0; preset < kShippedPresetCount; ++preset) {
        std::printf("   %-10s", shippedPresetName(preset));
        for (double f : {50.0, 100.0, 1000.0, 5000.0}) {
            // one steady tone per run (two at once beat, and the dynamics follow the beat)
            auto delta = [&](double ff) {
                Buffer l(n);
                for (size_t i = 0; i < n; ++i) l[i] = static_cast<float>(0.0316 * std::sin(2.0 * pi * ff * static_cast<double>(i) / fs));
                int lat = 0;
                const Stereo o = runLive(shippedPresetValues(preset), DeckControl(), l, l, {}, 256, &lat);
                const size_t at = n - win; // the last second, from the output's frame `at` (input frame at - lat)
                return wrap(phaseAt(l, at - static_cast<size_t>(lat), ff) - phaseAt(o.l, at, ff));
            };
            const double f2 = f * 1.02;
            const double gd = wrap(delta(f2) - delta(f)) / (2.0 * pi * (f2 - f)) * 1000.0;
            std::printf("  %5.0f Hz %+6.2f", f, gd);
            CHECK(gd > -0.5 && gd < 20.0);
        }
        std::printf("\n");
    }
}

// ---- bit-exact paths ----

void bypassExact() {
    std::printf("-- bypass bit-exact (from the start and after switching)\n");
    // peaks up to ~1.5: the bypass must not clip either
    const Buffer l = music(4, 11, 1.5 / sig::peak(sig::beatMusic(fs, 4, 124, 11))),
                 r = music(4, 12, 1.5 / sig::peak(sig::beatMusic(fs, 4, 124, 12)));
    for (double look : {1.5, 0.0, 5.0}) {
        DeckControl off;
        off.lookMs = look;
        off.processing = false;
        int lat = 0;
        Stereo o = runLive(shippedPresetValues(4), off, l, r, {}, 256, &lat);
        const size_t bad = mismatches(l, o.l, lat) + mismatches(r, o.r, lat);
        std::printf("   look-ahead %.1f ms (%d frames): %zu frames differ\n", look, lat, bad);
        CHECK(bad == 0);
        CHECK(lat == static_cast<int>(std::lround(look * fs / 1000.0)));
    }
    // a look-ahead above 0 never under the true-peak minimum (deck.h): at every rate it is true-peak limiting
    for (double rate : {44100.0, 48000.0, 88200.0, 96000.0})
        for (double look : {0.1, 0.5, 1.0, 1.2}) {
            const int f = lookFrames(look, rate);
            CHECK(f == std::max(kMinLookFrames, static_cast<int>(std::lround(look * rate / 1000.0))));
            Limiter lim;
            lim.prepare(f, kTpFactorLimiter, kTpTapsLimiter, kTpBetaLimiter);
            lim.setLook(f);
            CHECK(lim.usesTruePeak());
        }
    CHECK(lookFrames(0.0, fs) == 0 && lookFrames(-1.0, fs) == 0 && lookFrames(std::nan(""), fs) == 0);
    CHECK(lookFrames(9.0, fs) == static_cast<int>(std::lround(kMaxLookMs * fs / 1000.0)));
    std::printf("   0.1 .. 1.2 ms: at least %d frames (true peak) at 44.1 .. 96 kHz\n", kMinLookFrames);
    // processing on -> off at 1 s -> on at 2 s -> off at 2.5 s: exact again 20 ms (ramp) after each "off"
    DeckControl on, off;
    off.processing = false;
    const PresetValues v = shippedPresetValues(4);
    int lat = 0;
    Stereo o = runLive(v, on, l, r, {{frames(1.0), v, off}, {frames(2.0), v, on}, {frames(2.5), v, off}}, 256, &lat);
    const size_t ramp = frames(0.020) + 1;
    size_t bad1 = 0;
    for (size_t i = frames(1.0) + ramp; i < frames(2.0); ++i) {
        const size_t k = i - static_cast<size_t>(lat);
        if (std::memcmp(&o.l[i], &l[k], sizeof(float)) || std::memcmp(&o.r[i], &r[k], sizeof(float))) ++bad1;
    }
    const size_t bad2 = mismatches(l, o.l, lat, frames(2.5) + ramp) + mismatches(r, o.r, lat, frames(2.5) + ramp);
    std::printf("   after switching off: %zu / %zu frames differ\n", bad1, bad2);
    CHECK(bad1 == 0);
    CHECK(bad2 == 0);
}

void neutralExact() {
    std::printf("-- neutral chain (every stage off, processing on) bit-exact\n");
    const Buffer l = music(3, 13, 0.95 / sig::peak(sig::beatMusic(fs, 3, 124, 13))),
                 r = music(3, 14, 0.95 / sig::peak(sig::beatMusic(fs, 3, 124, 14)));
    for (double look : {1.5, 0.0}) {
        DeckControl ctl;
        ctl.lookMs = look;
        int lat = 0;
        Stereo o = runLive(allOff(), ctl, l, r, {}, 256, &lat);
        const size_t bad = mismatches(l, o.l, lat) + mismatches(r, o.r, lat);
        std::printf("   look-ahead %.1f ms: %zu frames differ\n", look, bad);
        CHECK(bad == 0);
    }
}

// ---- volume after the chain (Windows' volume) ----

void volume() {
    std::printf("-- volume after the chain and input factor (setVolume)\n");
    const Buffer l = music(3, 15, 0.9 / sig::peak(sig::beatMusic(fs, 3, 124, 15))),
                 r = music(3, 16, 0.9 / sig::peak(sig::beatMusic(fs, 3, 124, 16)));
    const PresetValues v = shippedPresetValues(3);
    const Stereo ref = tdeck::run(fs, v, l, r);
    auto mismatchScaled = [](const Stereo& a, const Stereo& b, float scale) {
        size_t bad = 0;
        for (size_t i = 0; i < a.l.size(); ++i)
            if (a.l[i] != b.l[i] * scale || a.r[i] != b.r[i] * scale) ++bad;
        return bad;
    };
    // set before prepare: from the first sample; half the volume = every sample halved (exact in float)
    DeckProcessor half;
    half.setVolume(0.5);
    half.prepare(fs);
    const size_t badHalf = mismatchScaled(tdeck::run(fs, v, l, r, 1.5, true, 256, &half), ref, 0.5f);
    // input at a quarter with a factor of 4 = the original input (exact)
    Buffer ql = l, qr = r;
    for (auto& x : ql) x *= 0.25f;
    for (auto& x : qr) x *= 0.25f;
    DeckProcessor comp;
    comp.setVolume(1.0, 4.0);
    comp.prepare(fs);
    const size_t badComp = mismatchScaled(tdeck::run(fs, v, ql, qr, 1.5, true, 256, &comp), ref, 1.0f);
    std::printf("   volume 0.5: %zu frames differ from half the output; input / 4 with factor 4: %zu differ\n",
                badHalf, badComp);
    CHECK(badHalf == 0);
    CHECK(badComp == 0);

    // mute while playing: glides down (no step), silent after 0.3 s, back to the same output after unmute
    DeckProcessor d;
    d.prepare(fs);
    DeckControl ctl;
    d.publish(makeDeckConfig(v, ctl, fs));
    Buffer ol = l, orr = r;
    const size_t n = l.size(), at = 256 * 188, back = 256 * 375; // about 1.0 s and 2.0 s, on block starts
    double worstJump = 0.0, afterMute = 0.0;
    for (size_t pos = 0; pos < n; pos += 256) {
        if (pos == at) d.setVolume(0.0);
        if (pos == back) d.setVolume(1.0);
        d.process(ol.data() + pos, orr.data() + pos, static_cast<int>(std::min<size_t>(256, n - pos)));
    }
    const size_t lat = static_cast<size_t>(d.latency());
    double lastGain = 1.0;
    for (size_t i = at; i < at + frames(0.1); ++i) { // the gain (output / reference) per sample: no step
        const double x = ref.l[i - lat];
        if (std::fabs(x) < 0.05) continue;
        const double g = ol[i] / x;
        worstJump = std::max(worstJump, std::fabs(g - lastGain));
        lastGain = g;
    }
    for (size_t i = at + frames(0.3); i < back; ++i) afterMute = std::max(afterMute, std::fabs(static_cast<double>(ol[i])));
    size_t badBack = 0;
    for (size_t i = back + frames(0.6); i < n; ++i)
        if (ol[i] != ref.l[i - lat]) ++badBack;
    std::printf("   mute: largest gain step %.5f (between loud samples), peak 0.3 s after %.1e, unmuted: %zu differ\n",
                worstJump, afterMute, badBack);
    CHECK(worstJump < 0.01);
    CHECK(afterMute < 1e-3);
    CHECK(badBack == 0);
}

// ---- fade-out before a stream stops (startFadeOut) ----

void fadeOut() {
    std::printf("-- fade-out before a stop: 10 ms to silence, then silent; a new prepare() plays again\n");
    const Buffer l = music(2, 21, 0.5), r = music(2, 22, 0.5);
    const PresetValues v = shippedPresetValues(2);
    const Stereo ref = tdeck::run(fs, v, l, r, 1.5, false);
    DeckProcessor d;
    d.prepare(fs);
    DeckControl ctl;
    d.publish(makeDeckConfig(v, ctl, fs));
    Stereo o{l, r};
    const size_t at = 256 * 188; // about 1.0 s, on a block start
    bool doneEarly = false;
    for (size_t pos = 0; pos < o.l.size(); pos += 256) {
        if (pos == at) {
            doneEarly = d.fadedOut();
            d.startFadeOut();
        }
        d.process(o.l.data() + pos, o.r.data() + pos, static_cast<int>(std::min<size_t>(256, o.l.size() - pos)));
    }
    const size_t fade = static_cast<size_t>(kFadeOutMs * fs / 1000.0);
    size_t before = 0, louder = 0, after = 0;
    for (size_t i = 0; i < at; ++i)
        if (o.l[i] != ref.l[i] || o.r[i] != ref.r[i]) ++before;
    for (size_t i = at; i < at + fade + 1; ++i)
        if (std::fabs(o.l[i]) > std::fabs(ref.l[i]) || std::fabs(o.r[i]) > std::fabs(ref.r[i])) ++louder;
    for (size_t i = at + fade + 1; i < o.l.size(); ++i)
        if (o.l[i] != 0.0f || o.r[i] != 0.0f) ++after;
    // the gain's change per sample in the fade (measured between loud samples): at most a raised cosine's over 10 ms,
    // pi / 2 / 480 = 0.0033
    double worstStep = 0.0, lastG = 1.0;
    size_t lastI = at - 1;
    for (size_t i = at; i < at + fade; ++i) {
        if (std::fabs(ref.l[i]) < 0.05) continue;
        const double g = o.l[i] / ref.l[i];
        worstStep = std::max(worstStep, std::fabs(g - lastG) / static_cast<double>(i - lastI));
        lastG = g;
        lastI = i;
    }
    std::printf("   before: %zu differ; in the fade: %zu louder than without, largest gain step %.4f; after: %zu not "
                "silent; reported silent: %s (not before the request: %s)\n",
                before, louder, worstStep, after, d.fadedOut() ? "yes" : "no", doneEarly ? "no" : "yes");
    CHECK(before == 0);
    CHECK(louder == 0);
    CHECK(worstStep < 0.004);
    CHECK(after == 0);
    CHECK(d.fadedOut() && !doneEarly);
    CHECK(d.processedFrames() == o.l.size());
    // the next start (prepare) plays normally
    d.prepare(fs);
    d.publish(makeDeckConfig(v, ctl, fs));
    Stereo again{l, r};
    for (size_t pos = 0; pos < again.l.size(); pos += 256)
        d.process(again.l.data() + pos, again.r.data() + pos,
                  static_cast<int>(std::min<size_t>(256, again.l.size() - pos)));
    size_t badAgain = 0;
    for (size_t i = 0; i < again.l.size(); ++i)
        if (again.l[i] != ref.l[i]) ++badAgain;
    std::printf("   after a new prepare(): %zu frames differ from a fresh deck, silent reported: %s\n", badAgain,
                d.fadedOut() ? "yes" : "no");
    CHECK(badAgain == 0);
    CHECK(!d.fadedOut());
}

// ---- bad input, MXCSR ----

void badInput() {
    std::printf("-- NaN / Inf / huge input\n");
    Buffer l = music(5, 15, 1.0), r = music(5, 16, 1.0);
    tdeck::setLoudness(fs, l, r, -12);
    Buffer bl = l, br = r;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    bl[frames(1.0)] = nan;
    br[frames(1.5)] = inf;
    bl[frames(1.7)] = -inf;
    for (size_t i = frames(2.0); i < frames(2.01); ++i) bl[i] = br[i] = nan;
    bl[frames(2.5)] = 1e30f;
    br[frames(2.6)] = 1e5f; // below the 1e6 cut: goes through the chain
    DeckProcessor d;
    d.prepare(fs);
    d.publish(makeDeckConfig(shippedPresetValues(4), DeckControl(), fs));
    Stereo o{bl, br};
    for (size_t pos = 0; pos < o.l.size(); pos += 256) {
        const int n = static_cast<int>(std::min<size_t>(256, o.l.size() - pos));
        d.process(o.l.data() + pos, o.r.data() + pos, n);
    }
    size_t bad = 0;
    for (size_t i = 0; i < o.l.size(); ++i)
        if (!std::isfinite(o.l[i]) || !std::isfinite(o.r[i]) || std::fabs(o.l[i]) > 1.0f || std::fabs(o.r[i]) > 1.0f)
            ++bad;
    const Stereo clean = runLive(shippedPresetValues(4), DeckControl(), l, r);
    const double a = tdeck::loudness(fs, o.l, o.r, frames(4.0)), b = tdeck::loudness(fs, clean.l, clean.r, frames(4.0));
    std::printf("   non-finite or > 1 output samples: %zu; resets %d; 4-5 s %.2f LUFS (clean input %.2f)\n", bad,
                d.resets(), a, b);
    CHECK(bad == 0);
    CHECK_NEAR(a, b, 0.5);
}

void mxcsrRestored() {
    std::printf("-- MXCSR restored after process()\n");
    DeckProcessor d;
    d.prepare(fs);
    d.publish(makeDeckConfig(shippedPresetValues(2), DeckControl(), fs));
    Buffer l = music(0.2, 17, 0.3), r = music(0.2, 18, 0.3);
    const unsigned saved = _mm_getcsr();
    for (unsigned csr : {0x1F80u, 0x9FC0u, 0x3F80u}) { // default; FTZ + DAZ already set; round down
        _mm_setcsr(csr);
        d.process(l.data(), r.data(), 256);
        const unsigned after = _mm_getcsr() & ~0x3Fu; // without the sticky exception flags
        std::printf("   0x%04X -> 0x%04X\n", csr, after);
        CHECK(after == (csr & ~0x3Fu));
    }
    _mm_setcsr(saved);
}

// ---- no allocation on the audio path ----

void noAllocations() {
    std::printf("-- no allocation in process(), with changes published\n");
    DeckProcessor d;
    d.prepare(fs);
    Buffer l = music(6, 19, 0.5), r = music(6, 20, 0.5);
    // configurations made beforehand (makeDeckConfig belongs to the GUI thread)
    std::vector<DeckConfig> cfgs;
    for (int p = 0; p < kShippedPresetCount; ++p) cfgs.push_back(makeDeckConfig(shippedPresetValues(p), DeckControl(), fs));
    for (int bands : {3, 7, 1, 4}) {
        PresetValues v = shippedPresetValues(3);
        v[pDensBands] = bands;
        loadDefaultSplits(v, bands);
        cfgs.push_back(makeDeckConfig(v, DeckControl(), fs));
    }
    DeckControl c;
    c.lookMs = 0.5;
    cfgs.push_back(makeDeckConfig(shippedPresetValues(4), c, fs));
    c.lookMs = 5.0;
    c.freeze = true;
    cfgs.push_back(makeDeckConfig(shippedPresetValues(4), c, fs));
    c.processing = false;
    cfgs.push_back(makeDeckConfig(shippedPresetValues(4), c, fs));
    cfgs.push_back(makeDeckConfig(allOff(), DeckControl(), fs));
    d.publish(cfgs[0]);
    MeterRecord recs[64];
    gAllocs = 0;
    size_t next = 1;
    for (size_t pos = 0; pos < l.size(); pos += 128) {
        if (pos % frames(0.25) < 128 && next < cfgs.size() * 2) d.publish(cfgs[next++ % cfgs.size()]);
        const int n = static_cast<int>(std::min<size_t>(128, l.size() - pos));
        gCounting = true;
        d.process(l.data() + pos, r.data() + pos, n);
        gCounting = false;
        while (d.meters().pop(recs, 64) > 0) {
        }
    }
    std::printf("   %zu configurations published, %ld allocations\n", next, gAllocs.load());
    CHECK(gAllocs.load() == 0);
}

// ---- clicks ----

// Largest |second difference| over [a, b) of both channels: a step or a corner in the waveform shows up here.
double maxD2(const Stereo& o, size_t a, size_t b) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(a, 2); i < b && i < o.l.size(); ++i) {
        m = std::max(m, std::fabs(static_cast<double>(o.l[i]) - 2.0 * o.l[i - 1] + o.l[i - 2]));
        m = std::max(m, std::fabs(static_cast<double>(o.r[i]) - 2.0 * o.r[i - 1] + o.r[i - 2]));
    }
    return m;
}

// Second difference during a change against the larger of the settled values before and after it.
double clickRatio(const PresetValues& v0, const DeckControl& c0, const std::vector<Change>& changes, double tChange,
                  double tEnd, const Buffer& l, const Buffer& r) {
    const Stereo o = runLive(v0, c0, l, r, changes);
    const double before = maxD2(o, frames(tChange - 0.4), frames(tChange));
    const double during = maxD2(o, frames(tChange), frames(tEnd));
    const double after = maxD2(o, frames(tEnd), frames(tEnd + 0.4));
    if (gVerbose) {
        size_t at = 0;
        double m = 0;
        for (size_t i = frames(tChange); i < frames(tEnd); ++i) {
            const double d = std::max(std::fabs(static_cast<double>(o.l[i]) - 2.0 * o.l[i - 1] + o.l[i - 2]),
                                      std::fabs(static_cast<double>(o.r[i]) - 2.0 * o.r[i - 1] + o.r[i - 2]));
            if (d > m) {
                m = d;
                at = i;
            }
        }
        std::printf("      before %.3g during %.3g after %.3g, largest %.1f ms after the change\n", before, during, after,
                    (static_cast<double>(at) - static_cast<double>(frames(tChange))) / fs * 1000.0);
    }
    // floor: the second difference of a 1 kHz sine 26 dB under the test tone. On a low tone the start of a 5 ms
    // gain ramp is a corner that the plain ratio would count as a click.
    const double floor = dbToLin(-40.0) * std::pow(2.0 * kPi * 1000.0 / fs, 2.0);
    return during / std::max(floor, std::max(before, after));
}

// A value well away from the default: +30 % of the range, or -30 % when that does not fit; switches toggle.
double changedValue(int p, double now) {
    const ParamInfo& in = paramInfo(p);
    if (in.isSwitch) return now >= 0.5 ? 0.0 : 1.0;
    const double step = 0.3 * (in.max - in.min);
    return now + step <= in.max ? now + step : now - step;
}

// Test tone per parameter: one the stage works on (a filter change in the bass enhancer does not show on 1 kHz).
double toneFor(int p) {
    switch (p) {
    case pBassOn:
    case pBassFreq:
    case pBassAmount:
    case pToneBass:
    case pToneBassFreq:
    case pSubAmount:
    case pSubFreq:
    case pLoudness:
        return 60;
    case pToneLowMid:
    case pToneLowMidFreq:
        return 350;
    case pTonePresence:
    case pTonePresenceFreq:
        return 3000;
    case pToneTreble:
    case pToneTrebleFreq:
        return 9000;
    case pCompSideHpf:
    case pBassWidth:
    case pWidthSplit:
        return 120;
    case pExcOn:
    case pExcFreq:
    case pExcAmount:
        return 5000;
    case pDessOn:
    case pDessFreq:
    case pDessThreshold:
    case pDessMax:
        return 7000;
    default:
        return 1000;
    }
}

void clicks() {
    std::printf("-- no clicks on changes (sine -14 dBFS, 1 kHz or in the stage's own band; second difference during "
                "the change / settled)\n");
    // L and R 90 degrees apart, so the side signal is not empty for the width stage
    std::vector<double> hz;
    std::vector<std::pair<Buffer, Buffer>> tones;
    tones.reserve(8);
    auto tone = [&](double f) -> const std::pair<Buffer, Buffer>& {
        for (size_t i = 0; i < hz.size(); ++i)
            if (hz[i] == f) return tones[i];
        hz.push_back(f);
        tones.emplace_back(sig::sine(fs, f, -14, 4.6), sig::sine(fs, f, -14, 4.6, 90));
        return tones.back();
    };
    const Buffer& l = tone(1000).first;
    const Buffer& r = tone(1000).second;
    PresetValues base = defaultValues(); // Medium, plus the two stages it leaves off
    base[pBassOn] = 1;
    base[pWidthOn] = 1;
    base[pWidth] = 150;
    // the colour values on a base with colour on, so a frequency change is heard (and the bass change crosses 0 dB)
    PresetValues colourBase = base;
    colourBase[pToneBass] = 3;
    colourBase[pToneLowMid] = -2;
    colourBase[pTonePresence] = 2;
    colourBase[pToneTreble] = 2;
    colourBase[pSubAmount] = 30;
    colourBase[pLoudness] = 30;
    // the auto tone only for its own values: on a lone test tone it pulls the bands to their limits, and a 6 dB bass
    // change then turns the leveller round at the change - a corner on a 60 Hz tone, not a click
    PresetValues autoBase = colourBase;
    autoBase[pAutoAmount] = 50;
    const DeckControl ctl;
    double worst = 0.0;
    int worstParam = -1;
    for (int p = 0; p < kParamCount; ++p) {
        const PresetValues& b = p == pAutoAmount || p == pAutoMax || p == pAutoTime ? autoBase
                                : isColourParam(p)                                     ? colourBase
                                                                                        : base;
        PresetValues v = b;
        v[static_cast<size_t>(p)] = changedValue(p, b[static_cast<size_t>(p)]);
        if (p == pDensBands) loadDefaultSplits(v, static_cast<int>(std::lround(v[pDensBands])));
        if (p >= pDensSplit1 && p <= pDensSplit6) enforceSplits(v, p - pDensSplit1);
        const auto& t = tone(toneFor(p));
        const double ratio = clickRatio(b, ctl, {{frames(3.0), v, ctl}}, 3.0, 4.0, t.first, t.second);
        std::printf("   %-14s %-24s %8.2f -> %8.2f  (%5.0f Hz): %.2f\n", paramInfo(p).stage, paramInfo(p).name,
                    b[static_cast<size_t>(p)], v[static_cast<size_t>(p)], toneFor(p), ratio);
        if (ratio > worst) {
            worst = ratio;
            worstParam = p;
        }
    }
    std::printf("   worst parameter: %s %s, %.2f\n", worstParam >= 0 ? paramInfo(worstParam).stage : "-",
                worstParam >= 0 ? paramInfo(worstParam).name : "-", worst);
    CHECK(worst < 1.5);

    // band count 3 -> 7 -> 1, the second change in the middle of the first crossfade
    auto withBands = [&](int bands) {
        PresetValues v = base;
        v[pDensBands] = bands;
        loadDefaultSplits(v, bands);
        return v;
    };
    double ratio = clickRatio(withBands(3), ctl, {{frames(3.0), withBands(7), ctl}, {frames(3.01), withBands(1), ctl}},
                              3.0, 4.0, l, r);
    std::printf("   bands 3 -> 7 -> 1 (10 ms apart): %.2f\n", ratio);
    CHECK(ratio < 1.5);

    // look-ahead 1.5 -> 0 -> 5 ms: fade out 5 ms, refill, fade in 5 ms (a short gap, no click)
    DeckControl l0 = ctl, l5 = ctl;
    l0.lookMs = 0.0;
    l5.lookMs = 5.0;
    ratio = clickRatio(base, ctl, {{frames(3.0), base, l0}, {frames(3.3), base, l5}}, 3.0, 4.0, l, r);
    std::printf("   look-ahead 1.5 -> 0 -> 5 ms: %.2f\n", ratio);
    CHECK(ratio < 1.5);

    // oversampling 4x -> 8x -> 2x: the clipper and the exciter fade out, take the new factor, fade back in
    DeckControl o8 = ctl, o2 = ctl;
    o8.oversampling = 8;
    o2.oversampling = 2;
    ratio = clickRatio(base, ctl, {{frames(3.0), base, o8}, {frames(3.3), base, o2}}, 3.0, 4.0, l, r);
    std::printf("   oversampling 4x -> 8x -> 2x: %.2f\n", ratio);
    CHECK(ratio < 1.5);

    // oversampling 4x -> 8x -> back to 4x within the fade: the change is called off and the stages fade straight back
    // in, no 40 ms gap of fade out - fade in for a factor that runs anyway (outside review; fourth: untested)
    {
        PresetValues v = base;
        v[pClipOn] = 1;
        v[pExcOn] = 1;
        const Stereo ref = runLive(v, ctl, l, r, {});
        const Stereo aba = runLive(v, ctl, l, r, {{frames(3.0), v, o8}, {frames(3.005), v, ctl}});
        size_t first = 0, last = 0;
        for (size_t i = frames(3.0); i < ref.l.size(); ++i)
            if (std::fabs(aba.l[i] - ref.l[i]) > 1e-3 || std::fabs(aba.r[i] - ref.r[i]) > 1e-3) {
                if (!first) first = i;
                last = i;
            }
        const double ms = first ? (static_cast<double>(last - first) + 1.0) / fs * 1000.0 : 0.0;
        ratio = clickRatio(v, ctl, {{frames(3.0), v, o8}, {frames(3.005), v, ctl}}, 3.0, 4.0, l, r);
        std::printf("   oversampling 4x -> 8x -> 4x (5 ms apart): output differs for %.1f ms, click ratio %.2f\n", ms, ratio);
        CHECK(ms < 25.0);
        CHECK(ratio < 1.5);
    }

    // the clipper's curve moves 1.5 dB under the ceiling when the limiter goes off, and back: it glides (fifth outside
    // review: without the glide a loud 200 Hz tone got a step of twice its own slope)
    {
        PresetValues on = allOff();
        on[pClipOn] = 1;
        on[pClipDrive] = 6;
        on[pLimOn] = 1;
        on[pLimCeiling] = -1;
        PresetValues off = on;
        off[pLimOn] = 0;
        const Buffer t200 = sig::sine(fs, 200, -1, 4.6), t200r = sig::sine(fs, 200, -1, 4.6, 90);
        const double down = clickRatio(on, ctl, {{frames(3.0), off, ctl}}, 3.0, 4.0, t200, t200r);
        const double up = clickRatio(off, ctl, {{frames(3.0), on, ctl}}, 3.0, 4.0, t200, t200r);
        std::printf("   limiter off / on under a clipping 200 Hz tone (the clipper's curve glides): %.2f / %.2f\n", down, up);
        CHECK(down < 1.5 && up < 1.5);
    }

    // bypass on and off, freeze on and off
    DeckControl by = ctl, fr = ctl;
    by.processing = false;
    fr.freeze = true;
    ratio = clickRatio(base, ctl, {{frames(3.0), base, by}, {frames(3.3), base, ctl}}, 3.0, 4.0, l, r);
    std::printf("   bypass on / off: %.2f\n", ratio);
    CHECK(ratio < 1.5);
    ratio = clickRatio(base, ctl, {{frames(3.0), base, fr}, {frames(3.3), base, ctl}}, 3.0, 4.0, l, r);
    std::printf("   freeze on / off: %.2f\n", ratio);
    CHECK(ratio < 1.5);
}

// ---- two decks through renderOffline ----

WavData interleave(const std::vector<const Buffer*>& ch) {
    WavData w;
    w.channels = static_cast<int>(ch.size());
    w.sampleRate = static_cast<int>(fs);
    const size_t n = ch[0]->size();
    w.samples.resize(n * ch.size());
    for (size_t i = 0; i < n; ++i)
        for (size_t c = 0; c < ch.size(); ++c) w.samples[i * ch.size() + c] = (*ch[c])[i];
    return w;
}

Buffer channel(const WavData& w, int c) {
    Buffer b(w.frames());
    for (size_t i = 0; i < b.size(); ++i) b[i] = w.samples[i * static_cast<size_t>(w.channels) + static_cast<size_t>(c)];
    return b;
}

bool render(const WavData& in, const RenderDeck decks[2], bool align, WavData& out, RenderResult& res) {
    std::string err;
    const bool ok = renderOffline(in, decks, align, 256, out, res, err);
    if (!ok) std::printf("   render failed: %s\n", err.c_str());
    return ok;
}

bool sameBits(const Buffer& a, const Buffer& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

void twoDecks() {
    std::printf("-- deck independence and equal latency (renderOffline, 4 channels)\n");
    Buffer al = music(8, 21, 1.0), ar = music(8, 22, 1.0), bl = music(8, 23, 1.0), br = music(8, 24, 1.0);
    tdeck::setLoudness(fs, al, ar, -20);
    tdeck::setLoudness(fs, bl, br, -8);
    RenderDeck decks[2];
    decks[0].used = decks[1].used = true;
    decks[0].values = shippedPresetValues(3); // Loud
    decks[1].values = shippedPresetValues(0); // Gentle
    WavData both, onlyA, onlyB;
    RenderResult r4, ra, rb;
    CHECK(render(interleave({&al, &ar, &bl, &br}), decks, true, both, r4));
    CHECK(render(interleave({&al, &ar}), &decks[0], true, onlyA, ra));
    RenderDeck bAsA[2] = {decks[1], RenderDeck()};
    CHECK(render(interleave({&bl, &br}), bAsA, true, onlyB, rb));
    const bool aSame = sameBits(channel(both, 0), channel(onlyA, 0)) && sameBits(channel(both, 1), channel(onlyA, 1));
    const bool bSame = sameBits(channel(both, 2), channel(onlyB, 0)) && sameBits(channel(both, 3), channel(onlyB, 1));
    std::printf("   deck A alone = deck A next to B: %s; deck B alone = deck B next to A: %s\n", aSame ? "yes" : "NO",
                bSame ? "yes" : "NO");
    CHECK(aSame);
    CHECK(bSame);
    const double ia = r4.deck[0].out.integrated(), ib = r4.deck[1].out.integrated();
    std::printf("   deck A (Loud, input -20) %.2f LUFS, deck B (Gentle, input -8) %.2f LUFS\n", ia, ib);

    // deck B not used: passes unprocessed, delayed like deck A
    decks[1].used = false;
    WavData passB;
    RenderResult rp;
    CHECK(render(interleave({&al, &ar, &bl, &br}), decks, true, passB, rp));
    const bool bPass = sameBits(channel(passB, 2), bl) && sameBits(channel(passB, 3), br);
    std::printf("   deck B unused: output = input %s\n", bPass ? "yes" : "NO");
    CHECK(bPass);

    // latency: an impulse on both decks (A neutral chain, B bypassed) comes out on the same frame
    for (double look : {1.5, 0.0, 5.0}) {
        Buffer imp = sig::silence(fs, 0.5);
        imp[1000] = 0.5f;
        RenderDeck d2[2];
        d2[0].used = true;
        d2[0].values = allOff();
        d2[0].control.lookMs = look;
        d2[1].control.lookMs = look;
        WavData out;
        RenderResult rr;
        CHECK(render(interleave({&imp, &imp, &imp, &imp}), d2, false, out, rr));
        size_t at[4] = {};
        for (int c = 0; c < 4; ++c) {
            const Buffer b = channel(out, c);
            for (size_t i = 0; i < b.size(); ++i)
                if (b[i] != 0.0f) {
                    at[c] = i;
                    break;
                }
        }
        const size_t want = 1000 + static_cast<size_t>(rr.latency);
        std::printf("   look-ahead %.1f ms: impulse at %zu / %zu / %zu / %zu, expected %zu\n", look, at[0], at[1], at[2],
                    at[3], want);
        CHECK(at[0] == want && at[1] == want && at[2] == want && at[3] == want);
        CHECK(out.frames() == imp.size() + static_cast<size_t>(rr.latency)); // the delay kept: nothing cut off
    }
    // an impulse on the very last frame still comes out, with the delay kept and removed
    for (const bool align : {false, true}) {
        Buffer imp = sig::silence(fs, 0.1);
        imp.back() = 0.5f;
        RenderDeck d2[2];
        d2[0].used = true;
        d2[0].values = allOff();
        WavData out;
        RenderResult rr;
        CHECK(render(interleave({&imp, &imp}), d2, align, out, rr));
        const Buffer b = channel(out, 0);
        CHECK(!b.empty() && b.back() != 0.0f);
    }
}

// ---- CPU ----

void cpu() {
    std::printf("-- CPU: both decks Very Loud at 48 kHz\n");
    Buffer al = music(20, 25, 1.0), ar = music(20, 26, 1.0), bl = music(20, 27, 1.0), br = music(20, 28, 1.0);
    tdeck::setLoudness(fs, al, ar, -12);
    tdeck::setLoudness(fs, bl, br, -12);
    RenderDeck decks[2];
    decks[0].used = decks[1].used = true;
    decks[0].values = decks[1].values = shippedPresetValues(4);
    WavData out;
    RenderResult res;
    const WavData in = interleave({&al, &ar, &bl, &br});
    double best = 1e9;
    for (int run = 0; run < 3; ++run) { // best of three: the machine may be busy with something else
        CHECK(render(in, decks, true, out, res));
        best = std::min(best, res.processSeconds / res.audioSeconds * 100.0);
    }
    // budget set by the client 2026-10-24 (docs/design.md section 10): goal < 3 %; fail > 7 % of one core (client,
    // 2026-10-27: "CPU is not a problem" - 5 % failed on a busier machine with the oversampled clipper and exciter)
    std::printf("   %.2f %% of one core for both decks (goal < 3 %%, fail > 7 %%)%s\n", best,
                best < 3.0 ? "" : "  - over the goal");
    CHECK(best < 7.0);
    // the same with every colour section working: tone, auto tone, sub bass, loudness (the most a colour costs)
    for (RenderDeck& d : decks) {
        d.values[pToneBass] = 3;
        d.values[pToneLowMid] = -2;
        d.values[pTonePresence] = 1.5;
        d.values[pToneTreble] = 2;
        d.values[pAutoAmount] = 70;
        d.values[pSubAmount] = 30;
        d.values[pLoudness] = 40;
    }
    double colour = 1e9;
    for (int run = 0; run < 3; ++run) {
        CHECK(render(in, decks, true, out, res));
        colour = std::min(colour, res.processSeconds / res.audioSeconds * 100.0);
    }
    std::printf("   with every colour stage on: %.2f %% (+%.2f)\n", colour, colour - best);
    CHECK(colour < 7.0);
}

// --cpu: cost of each stage (Very Loud with one stage off), informational
void cpuStages() {
    std::printf("-- CPU per stage: one deck, Very Loud with one stage off (best of 3, %% of one core)\n");
    Buffer l = music(10, 25, 1.0), r = music(10, 26, 1.0);
    tdeck::setLoudness(fs, l, r, -12);
    const WavData in = interleave({&l, &r});
    auto measure = [&](const PresetValues& v, double lookMs) {
        RenderDeck decks[2];
        decks[0].used = true;
        decks[0].values = v;
        decks[0].control.lookMs = lookMs;
        WavData out;
        RenderResult res;
        double best = 1e9;
        for (int run = 0; run < 3; ++run) {
            render(in, decks, true, out, res);
            best = std::min(best, res.processSeconds / res.audioSeconds * 100.0);
        }
        return best;
    };
    const PresetValues vl = shippedPresetValues(4);
    const double all = measure(vl, 1.5);
    std::printf("   all stages %.3f %%\n", all);
    const Param sw[] = {pLevOn, pCompOn, pDensOn, pBassOn, pExcOn, pDessOn, pWidthOn, pClipOn, pLimOn};
    for (Param p : sw) {
        PresetValues v = vl;
        v[p] = 1 - v[p];
        std::printf("   %-14s %s: %.3f %% (%+.3f)\n", paramInfo(p).stage, v[p] > 0.5 ? "on " : "off", measure(v, 1.5),
                    measure(v, 1.5) - all);
    }
    PresetValues one = vl;
    one[pDensBands] = 1;
    std::printf("   density 1 band: %.3f %%\n", measure(one, 1.5));
    std::printf("   look-ahead 0 ms (sample-peak limiter): %.3f %%\n", measure(vl, 0.0));
    PresetValues none = allOff();
    std::printf("   every stage off: %.3f %%\n", measure(none, 1.5));
    RenderDeck bypass[2];
    bypass[0].used = false;
    WavData out;
    RenderResult res;
    render(in, bypass, true, out, res);
    std::printf("   bypass: %.3f %%\n", res.processSeconds / res.audioSeconds * 100.0);
}

// ---- shipped presets ----

double refTruePeakDb(const Buffer& l, const Buffer& r) {
    TruePeakDetector d;
    d.configure(16, 128, 10.0);
    float m = 0.0f;
    for (const Buffer* b : {&l, &r}) {
        d.reset();
        for (float x : *b) m = std::max(m, d.push(x));
        for (int i = 0; i < d.delay() + 1; ++i) m = std::max(m, d.push(0.0f));
    }
    return linToDb(m);
}

void presets() {
    std::printf("-- shipped presets on synthetic music (40 s, measured 15-40 s)\n");
    std::printf("   %-10s %6s | %-24s | %-24s\n", "preset", "target", "input -18 LUFS: I / dBTP", "input -9 LUFS: I / dBTP");
    double prev[2] = {-100, -100};
    for (int p = 0; p < kShippedPresetCount; ++p) {
        const PresetValues v = shippedPresetValues(p);
        double res[2][2];
        int k = 0;
        for (double inLufs : {-18.0, -9.0}) {
            Buffer l = music(40, 31, 1.0), r = music(40, 32, 1.0);
            tdeck::setLoudness(fs, l, r, inLufs);
            const Stereo o = tdeck::run(fs, v, l, r);
            res[k][0] = tdeck::loudness(fs, o.l, o.r, frames(15));
            res[k][1] = refTruePeakDb(o.l, o.r);
            // every preset's range covers both levels: output within 1 LU of the target (docs/design.md section 10);
            // the output gain (FM Station: -4 dB) comes after the limiter
            CHECK_NEAR(res[k][0], v[pTarget] + v[pOutputGain], 1.0);
            CHECK(res[k][1] <= v[pLimCeiling] + v[pOutputGain] + 0.1);
            if (p < kStrengthPresetCount) { // the ladder of strength: each preset louder than the one before
                CHECK(res[k][0] > prev[k] + 1.0);
                prev[k] = res[k][0];
            }
            ++k;
        }
        std::printf("   %-10s %6.1f | %10.2f / %8.2f    | %10.2f / %8.2f\n", shippedPresetName(p),
                    v[pTarget] + v[pOutputGain], res[0][0],
                    res[0][1], res[1][0], res[1][1]);
        if (p == 0) { // Gentle's 12 dB range: a loud club track of -6 LUFS still comes down to -18
            Buffer l = music(40, 31, 1.0), r = music(40, 32, 1.0);
            tdeck::setLoudness(fs, l, r, -6.0);
            const Stereo o = tdeck::run(fs, v, l, r);
            const double i = tdeck::loudness(fs, o.l, o.r, frames(15));
            std::printf("   %-10s %6s | input -6 LUFS: %.2f LUFS\n", "", "", i);
            CHECK_NEAR(i, v[pTarget], 1.0);
        }
    }
}

// ---- the analysis window's signal-chain bars (MeterRecord::chain / active) ----

struct ChainRun {
    Stereo out;
    double e[kChainTaps] = {}; // sums over the records from `from` on
    double outEnergy = 0;      // sum of the output samples' squares over the same frames
    uint16_t active = 0;       // of the last record
    float levellerDb = 0;      // of the last record
    double lastRatioDb = 0;    // 10 log10 of the last record's leveller / input points
};

// Runs a deck in 256-frame blocks (no delay removed), reading the meter records on the way.
ChainRun chainRun(const PresetValues& v, double lookMs, const Buffer& l, const Buffer& r, size_t from) {
    DeckProcessor d;
    d.prepare(fs);
    DeckControl ctl;
    ctl.lookMs = lookMs;
    d.publish(makeDeckConfig(v, ctl, fs));
    ChainRun run;
    run.out = Stereo{l, r};
    size_t done = 0; // frames covered by the records read
    MeterRecord recs[64];
    for (size_t pos = 0; pos < l.size(); pos += 256) {
        const int n = static_cast<int>(std::min<size_t>(256, l.size() - pos));
        d.process(run.out.l.data() + pos, run.out.r.data() + pos, n);
        int got;
        while ((got = d.meters().pop(recs, 64)) > 0)
            for (int i = 0; i < got; ++i) {
                const MeterRecord& m = recs[i];
                if (done >= from) {
                    for (int k = 0; k < kChainTaps; ++k) run.e[k] += m.chain[k];
                    for (size_t f = done; f < done + m.frames; ++f)
                        run.outEnergy += static_cast<double>(run.out.l[f]) * run.out.l[f] +
                                         static_cast<double>(run.out.r[f]) * run.out.r[f];
                }
                done += m.frames;
                run.active = m.active;
                run.levellerDb = m.levellerDb;
                run.lastRatioDb = 10.0 * std::log10(m.chain[ctLeveller] / m.chain[ctInput]);
            }
    }
    return run;
}

void chainMeters() {
    std::printf("-- signal-chain meters (the analysis window's bars)\n");
    Buffer l = music(12, 41, 1.0), r = music(12, 42, 1.0);
    tdeck::setLoudness(fs, l, r, -20);
    // every stage off, no look-ahead: every point of the chain sees the input
    {
        const ChainRun run = chainRun(allOff(), 0.0, l, r, frames(1));
        double worst = 0;
        for (int k = 1; k < kChainTaps; ++k) worst = std::max(worst, std::fabs(run.e[k] / run.e[ctInput] - 1.0));
        std::printf("   every stage off: largest difference between two points %.2g, stages running %u\n", worst,
                    run.active);
        CHECK(worst < 1e-6);
        CHECK(run.active == 0);
    }
    // Very Loud: the bits name the preset's stages; the leveller point is the leveller gain; the output point is
    // the output samples
    {
        const PresetValues v = shippedPresetValues(4);
        const ChainRun run = chainRun(v, 1.5, l, r, frames(6));
        const Param onOf[kStageCount] = {pLevOn, pCompOn, pDensOn, pBassOn, pExcOn, pDessOn, pWidthOn, pClipOn, pLimOn};
        uint16_t want = 0;
        for (int s = 0; s < kStageCount; ++s)
            if (v[onOf[s]] >= 0.5) want = static_cast<uint16_t>(want | 1u << s);
        if (makeDeckConfig(v, DeckControl(), fs).colour.any()) want = static_cast<uint16_t>(want | 1u << kChainColourBit);
        std::printf("   Very Loud, 6-12 s, change at each step (dB):");
        const char* names[kChainTaps] = {"in",      "leveller", "colour",  "comp",     "density", "bass",
                                         "exciter", "de-ess",   "width",   "clipper",  "limiter", "out"};
        for (int k = 1; k < kChainTaps; ++k)
            std::printf(" %s %+.2f", names[k], 10.0 * std::log10(run.e[k] / run.e[k - 1]));
        std::printf("\n   stages running %#x (preset %#x); last record: leveller %.3f dB, points %.3f dB; output "
                    "point / output samples %.6f\n",
                    run.active, want, run.levellerDb, run.lastRatioDb, run.e[ctOutput] / run.outEnergy);
        CHECK(run.active == want);
        CHECK_NEAR(run.lastRatioDb, run.levellerDb, 0.05);
        CHECK_NEAR(run.e[ctOutput] / run.outEnergy, 1.0, 1e-4);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--cpu") {
        cpuStages();
        return 0;
    }
    const bool clicksOnly = argc > 1 && std::string(argv[1]) == "--clicks";
    gVerbose = argc > 2 && std::string(argv[2]) == "--verbose";
    if (!clicksOnly) {
        fastMath();
        oversampler();
        groupDelay();
        bypassExact();
        neutralExact();
        volume();
        fadeOut();
        badInput();
        mxcsrRestored();
        noAllocations();
        chainMeters();
    }
    clicks();
    if (!clicksOnly) {
        twoDecks();
        cpu();
        presets();
    }
    return test::finish("deck_test");
}
