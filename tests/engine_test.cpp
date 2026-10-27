// The live audio path without a driver: device sample formats and the engine that the ASIO host calls once per
// period (docs/design.md section 6, handbook 11, 12). The test plays the driver: it hands the engine buffers.
#include "check.h"
#include "deckrun.h"
#include "engine.h"
#include "sampleformat.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
#include <xmmintrin.h>

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

const double fs = 48000.0;
// a live stream's first 5 ms fade in (DeckProcessor::startFadeIn, called by AudioEngine::prepare); bit-exact after it
const size_t kStartFade = static_cast<size_t>(0.005 * fs);

const SampleFormat kFormats[] = {SampleFormat::Int16,     SampleFormat::Int24,     SampleFormat::Int32,
                                 SampleFormat::Int32In16, SampleFormat::Int32In18, SampleFormat::Int32In20,
                                 SampleFormat::Int32In24, SampleFormat::Float32,   SampleFormat::Float64};

int bitsOf(SampleFormat f) {
    switch (f) {
    case SampleFormat::Int16:
    case SampleFormat::Int32In16:
        return 16;
    case SampleFormat::Int32In18:
        return 18;
    case SampleFormat::Int32In20:
        return 20;
    case SampleFormat::Int24:
    case SampleFormat::Int32In24:
        return 24;
    case SampleFormat::Int32:
        return 32;
    default:
        return 0;
    }
}

float convertBack(SampleFormat f, float x) {
    unsigned char buf[8] = {};
    fromFloat(&x, f, buf, 0, 1);
    float y = 0;
    toFloat(buf, f, 0, &y, 1);
    return y;
}

void formats() {
    std::printf("-- sample formats: round trip, full scale, clipping, NaN, sign extension\n");
    for (SampleFormat f : kFormats) {
        const int bits = bitsOf(f);
        const double lsb = bits ? std::ldexp(1.0, 1 - bits) : 0.0;
        double worst = 0;
        for (int i = -1000; i <= 1000; ++i) {
            const float x = static_cast<float>(i / 1000.0 * 0.999);
            worst = std::max(worst, std::fabs(static_cast<double>(convertBack(f, x)) - x));
        }
        const float top = convertBack(f, 1.0f), bottom = convertBack(f, -1.0f), over = convertBack(f, 3.0f),
                    under = convertBack(f, -3.0f),
                    nan = convertBack(f, std::numeric_limits<float>::quiet_NaN()),
                    inf = convertBack(f, std::numeric_limits<float>::infinity());
        std::printf("   %-15s %d bytes: round trip error %.3g (half step %.3g), +1 -> %.9f, -1 -> %.9f\n", formatName(f),
                    bytesPerSample(f), worst, lsb / 2, top, bottom);
        if (bits) {
            // 16..20 bits are dithered (+-1 step, triangular): at most 1.5 steps from the value; else rounded
            CHECK(worst <= (bits <= 20 ? 1.5 : 0.5) * lsb + 1e-7);
            CHECK_NEAR(top, 1.0 - lsb, 1e-7);  // the largest code: clamped one step under full scale
            CHECK(bottom == -1.0f);
            CHECK(over == top && under == bottom);
        } else {
            CHECK(worst == 0.0);
            CHECK(top == 1.0f && over == 3.0f); // floats pass everything finite
        }
        CHECK(nan == 0.0f);
        CHECK(inf == 0.0f);
    }

    // every 16-bit code survives int16 -> float -> int16
    std::vector<int16_t> codes(65536), back(65536);
    for (int i = 0; i < 65536; ++i) codes[static_cast<size_t>(i)] = static_cast<int16_t>(i - 32768);
    std::vector<float> f(65536);
    toFloat(codes.data(), SampleFormat::Int16, 0, f.data(), 65536);
    fromFloat(f.data(), SampleFormat::Int16, back.data(), 0, 65536);
    CHECK(codes == back);
    // 24-bit packed: every 97th code of 2^24
    size_t bad = 0;
    for (int32_t c = -8388608; c < 8388608; c += 97) {
        const unsigned char in[3] = {static_cast<unsigned char>(c), static_cast<unsigned char>(c >> 8),
                                     static_cast<unsigned char>(c >> 16)};
        float x;
        toFloat(in, SampleFormat::Int24, 0, &x, 1);
        unsigned char out[3];
        fromFloat(&x, SampleFormat::Int24, out, 0, 1);
        if (std::memcmp(in, out, 3) != 0) ++bad;
    }
    CHECK(bad == 0);
    // 24 bits in a 32-bit container: the high byte is not part of the value (some drivers leave garbage there)
    const int32_t raw[3] = {static_cast<int32_t>(0xAB000001u), 0x00FFFFFF, 0x00800000};
    float v[3];
    toFloat(raw, SampleFormat::Int32In24, 0, v, 3);
    std::printf("   24 in 32: 0xAB000001 -> %+.3g, 0x00FFFFFF -> %+.3g, 0x00800000 -> %+.3g\n", v[0], v[1], v[2]);
    CHECK(v[0] == static_cast<float>(1.0 / 8388608.0));
    CHECK(v[1] == static_cast<float>(-1.0 / 8388608.0));
    CHECK(v[2] == -1.0f);
    // offsets address samples, not bytes; silence() clears exactly the range
    int16_t s16[6] = {1, 2, 3, 4, 5, 6};
    silence(SampleFormat::Int16, s16, 2, 3);
    CHECK(s16[1] == 2 && s16[2] == 0 && s16[4] == 0 && s16[5] == 6);

    // 16-bit dither: a tone of 0.4 steps on a level of 3 steps is gone when rounded (always 3), but lives on under the
    // dither noise; no offset; silence and remains under one step stay silent
    const int n = 480000;
    const double pi = 3.14159265358979;
    std::vector<float> tone(n), zeros(n, 0.0f), remains(n), level(n, static_cast<float>(2.3 / 32768.0));
    for (int i = 0; i < n; ++i) {
        tone[static_cast<size_t>(i)] = static_cast<float>((3.0 + 0.4 * std::sin(2.0 * pi * 1000.0 * i / 48000.0)) / 32768.0);
        remains[static_cast<size_t>(i)] = static_cast<float>(1e-7 * std::sin(2.0 * pi * 50.0 * i / 48000.0));
    }
    std::vector<int16_t> q(n), z(n), r(n), c(n);
    fromFloat(tone.data(), SampleFormat::Int16, q.data(), 0, n);
    fromFloat(zeros.data(), SampleFormat::Int16, z.data(), 0, n);
    fromFloat(remains.data(), SampleFormat::Int16, r.data(), 0, n);
    fromFloat(level.data(), SampleFormat::Int16, c.data(), 0, n);
    double corr = 0.0, mean = 0.0, ref = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s = std::sin(2.0 * pi * 1000.0 * i / 48000.0);
        corr += q[static_cast<size_t>(i)] * s;
        ref += s * s;
        mean += q[static_cast<size_t>(i)];
    }
    const double amp = corr / ref; // in steps; rounding alone gives 0 here
    auto silent = [](const std::vector<int16_t>& v) { return std::all_of(v.begin(), v.end(), [](int16_t x) { return x == 0; }); };
    // TPDF (+-1 step triangular) plus the rounding: an error variance of 1/4 step^2 for any value between steps
    // (rectangular dither would give 1/6, rounding alone 0 for a steady value)
    double em = 0.0, ev = 0.0;
    for (int16_t x : c) em += x - 2.3;
    em /= n;
    for (int16_t x : c) ev += (x - 2.3 - em) * (x - 2.3 - em);
    ev /= n;
    std::printf("   16-bit dither: a 0.4-step tone on 3 steps comes out at %.3f steps, offset %.4f steps; error variance "
                "%.3f step^2 (TPDF 0.25); silence %s, remains under a step %s\n",
                amp, mean / n - 3.0, ev, silent(z) ? "silent" : "NOT silent", silent(r) ? "silent" : "NOT silent");
    CHECK(std::fabs(amp - 0.4) < 0.02);
    CHECK(std::fabs(mean / n - 3.0) < 0.01);
    CHECK(std::fabs(ev - 0.25) < 0.01 && std::fabs(em) < 0.01);
    CHECK(silent(z) && silent(r));

    // a steady tone of 0.4 steps around zero (no level under it) is dithered too, not cut to silence: the blocks that
    // get no dither are only those under a quarter step (outside review: at one step this tone vanished)
    std::vector<float> soft(4800);
    for (size_t i = 0; i < soft.size(); ++i)
        soft[i] = static_cast<float>(0.4 * std::sin(2.0 * pi * 1000.0 * static_cast<double>(i) / 48000.0) / 32768.0);
    double softAmp = 0.0, softRef = 0.0;
    std::vector<int16_t> sq(soft.size());
    for (int rep = 0; rep < 100; ++rep) {
        for (size_t b = 0; b < soft.size(); b += 480) // device periods of 480 frames
            fromFloat(soft.data() + b, SampleFormat::Int16, sq.data() + b, 0, 480);
        for (size_t i = 0; i < soft.size(); ++i) {
            const double s = std::sin(2.0 * pi * 1000.0 * static_cast<double>(i) / 48000.0);
            softAmp += sq[i] * s;
            softRef += s * s;
        }
    }
    softAmp /= softRef;
    std::printf("   16-bit dither: a 0.4-step tone alone comes out at %.3f steps\n", softAmp);
    CHECK(std::fabs(softAmp - 0.4) < 0.03);
}

// A fake driver: four input and four output channel buffers of one format, `period` frames per buffer switch.
struct FakeDevice {
    SampleFormat inF, outF;
    int period;
    std::vector<unsigned char> in[4], out[4];
    FakeDevice(SampleFormat i, SampleFormat o, int p) : inF(i), outF(o), period(p) {
        for (int c = 0; c < 4; ++c) {
            in[c].assign(static_cast<size_t>(p) * bytesPerSample(i), 0);
            out[c].assign(static_cast<size_t>(p) * bytesPerSample(o), 0);
        }
    }
    // Runs whole periods over `src` (4 float channels), returns the 4 output channels as float.
    std::vector<std::vector<float>> run(AudioEngine& e, const std::vector<std::vector<float>>& src, bool countAllocs = false) {
        const size_t n = src[0].size();
        std::vector<std::vector<float>> res(4, std::vector<float>(n, 0.0f));
        const void* inp[4] = {in[0].data(), in[1].data(), in[2].data(), in[3].data()};
        void* outp[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
        const SampleFormat inFs[4] = {inF, inF, inF, inF}, outFs[4] = {outF, outF, outF, outF};
        std::vector<float> tmp(static_cast<size_t>(period));
        for (size_t pos = 0; pos + static_cast<size_t>(period) <= n; pos += static_cast<size_t>(period)) {
            for (int c = 0; c < 4; ++c) fromFloat(src[static_cast<size_t>(c)].data() + pos, inF, in[c].data(), 0, period);
            if (countAllocs) gCounting = true;
            e.process(inp, inFs, outp, outFs, period);
            gCounting = false;
            for (int c = 0; c < 4; ++c) {
                toFloat(out[c].data(), outF, 0, tmp.data(), period);
                std::copy(tmp.begin(), tmp.end(), res[static_cast<size_t>(c)].begin() + static_cast<long long>(pos));
            }
        }
        return res;
    }
};

std::vector<std::vector<float>> signals(size_t n) {
    std::vector<std::vector<float>> s;
    for (uint64_t seed = 1; seed <= 4; ++seed) {
        sig::Buffer b = sig::beatMusic(fs, static_cast<double>(n) / fs, 124, seed);
        b.resize(n);
        for (float& x : b) x = static_cast<float>(x * 0.5);
        s.push_back(b);
    }
    return s;
}

void publishBoth(AudioEngine& e, const PresetValues& a, const PresetValues& b, const DeckControl& ca,
                 const DeckControl& cb) {
    e.deck(0).publish(makeDeckConfig(a, ca, fs));
    e.deck(1).publish(makeDeckConfig(b, cb, fs));
    float z = 0;
    e.deck(0).process(&z, &z, 0); // taken before the first period, like the host's beforeStart
    e.deck(1).process(&z, &z, 0);
}

void bypassThroughDevice() {
    std::printf("-- engine as the driver sees it: bypass bit-exact, delayed by the look-ahead, any period size\n");
    const size_t n = 48000;
    for (int period : {1, 37, 64, 256, 300, 2048}) {
        for (double look : {0.0, 1.5}) {
            AudioEngine e;
            e.prepare(fs);
            DeckControl off;
            off.processing = false;
            off.lookMs = look;
            publishBoth(e, shippedPresetValues(4), shippedPresetValues(1), off, off);
            FakeDevice dev(SampleFormat::Float32, SampleFormat::Float32, period);
            const auto src = signals(n);
            const auto out = dev.run(e, src);
            const size_t lat = static_cast<size_t>(e.deck(0).latency());
            const size_t done = n / static_cast<size_t>(period) * static_cast<size_t>(period);
            size_t bad = 0, loud = 0;
            for (int c = 0; c < 4; ++c)
                for (size_t i = 0; i < done; ++i) {
                    const float want = i >= lat ? src[static_cast<size_t>(c)][i - lat] : 0.0f;
                    const float got = out[static_cast<size_t>(c)][i];
                    if (i < kStartFade) { // the start's fade-in: from 0, never louder than the input
                        if (std::fabs(got) > std::fabs(want) || (i == 0 && got != 0.0f)) ++loud;
                    } else if (std::memcmp(&want, &got, 4) != 0) {
                        ++bad;
                    }
                }
            if (period == 37 || period == 2048 || bad || loud)
                std::printf("   period %4d, look-ahead %.1f ms (%zu frames): %zu samples differ after the 5 ms start "
                            "fade, %zu louder than the input in it\n", period, look, lat, bad, loud);
            CHECK(bad == 0);
            CHECK(loud == 0);
        }
    }
}

void formatsThroughDevice() {
    std::printf("-- engine with integer device formats (24-bit in, 32-bit out) and mixed formats\n");
    const size_t n = 24000;
    AudioEngine e;
    e.prepare(fs);
    DeckControl off;
    off.processing = false;
    off.lookMs = 0;
    publishBoth(e, defaultValues(), defaultValues(), off, off);
    // 24-bit input codes come out exactly as 32-bit output (24-bit value, low byte zero)
    FakeDevice dev(SampleFormat::Int24, SampleFormat::Int32, 64);
    auto src = signals(n);
    for (auto& ch : src)
        for (float& x : ch) x = static_cast<float>(std::llround(x * 8388608.0) / 8388608.0); // on the 24-bit grid
    const auto out = dev.run(e, src);
    size_t bad = 0;
    for (int c = 0; c < 4; ++c)
        for (size_t i = kStartFade; i < n; ++i)
            if (out[static_cast<size_t>(c)][i] != src[static_cast<size_t>(c)][i]) ++bad;
    std::printf("   24-bit in -> 32-bit out, bypass: %zu samples differ (after the start fade)\n", bad);
    CHECK(bad == 0);

    // processing on: the same output whatever the device format (within the format's step)
    AudioEngine a, b;
    a.prepare(fs);
    b.prepare(fs);
    const DeckControl on;
    publishBoth(a, shippedPresetValues(3), shippedPresetValues(0), on, on);
    publishBoth(b, shippedPresetValues(3), shippedPresetValues(0), on, on);
    FakeDevice fa(SampleFormat::Float32, SampleFormat::Float32, 128), fb(SampleFormat::Float64, SampleFormat::Int32In24, 128);
    const auto s2 = signals(n);
    const auto oa = fa.run(a, s2), ob = fb.run(b, s2);
    double worst = 0;
    for (int c = 0; c < 4; ++c)
        for (size_t i = 0; i < n; ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(oa[static_cast<size_t>(c)][i]) - ob[static_cast<size_t>(c)][i]));
    std::printf("   processing on, float32 device vs float64 in / 24-in-32 out: largest difference %.3g (24-bit step %.3g)\n",
                worst, 1.0 / 8388608.0);
    CHECK(worst <= 1.0 / 8388608.0);
}

void decksApart() {
    std::printf("-- deck A silent in, deck B playing: deck A stays silent\n");
    AudioEngine e;
    e.prepare(fs);
    const DeckControl on;
    publishBoth(e, shippedPresetValues(4), shippedPresetValues(4), on, on);
    auto src = signals(48000);
    std::fill(src[0].begin(), src[0].end(), 0.0f);
    std::fill(src[1].begin(), src[1].end(), 0.0f);
    FakeDevice dev(SampleFormat::Float32, SampleFormat::Float32, 128);
    const auto out = dev.run(e, src);
    double a = 0, b = 0;
    for (size_t i = 0; i < out[0].size(); ++i) {
        a = std::max(a, static_cast<double>(std::fabs(out[0][i])) + std::fabs(out[1][i]));
        b = std::max(b, static_cast<double>(std::fabs(out[2][i])));
    }
    std::printf("   deck A peak %.3g, deck B peak %.3f\n", a, b);
    CHECK(a == 0.0);
    CHECK(b > 0.1);
}

void oneChannel() {
    std::printf("-- one channel: deck B has no buffers - not processed, deck A bit-exact in bypass\n");
    AudioEngine e;
    e.prepare(fs);
    DeckControl off;
    off.processing = false;
    off.lookMs = 0;
    publishBoth(e, defaultValues(), defaultValues(), off, off);
    const int period = 256, periods = 40;
    const auto src = signals(static_cast<size_t>(period * periods));
    std::vector<float> outA(2 * static_cast<size_t>(period));
    size_t bad = 0;
    for (int k = 0; k < periods; ++k) {
        const void* in[4] = {src[0].data() + k * period, src[1].data() + k * period, nullptr, nullptr};
        void* out[4] = {outA.data(), outA.data() + period, nullptr, nullptr};
        const SampleFormat f[4] = {SampleFormat::Float32, SampleFormat::Float32, SampleFormat::Float32,
                                   SampleFormat::Float32};
        e.process(in, f, out, f, period);
        for (int i = 0; i < period; ++i)
            if (static_cast<size_t>(k * period + i) >= kStartFade &&
                (outA[static_cast<size_t>(i)] != src[0][static_cast<size_t>(k * period + i)] ||
                 outA[static_cast<size_t>(period + i)] != src[1][static_cast<size_t>(k * period + i)]))
                ++bad;
    }
    MeterRecord recs[64];
    const int recB = e.deck(1).meters().pop(recs, 64), recA = e.deck(0).meters().pop(recs, 64);
    std::printf("   deck A: %zu samples differ, %d meter records; deck B: %d meter records\n", bad, recA, recB);
    CHECK(bad == 0);
    CHECK(recA > 0 && recB == 0);
}

void healthAndSafety() {
    std::printf("-- health counters, unprepared engine, allocations, MXCSR\n");
    AudioEngine idle; // not prepared: answers with silence and counts it
    FakeDevice d0(SampleFormat::Int16, SampleFormat::Int16, 64);
    for (auto& ch : d0.out) std::fill(ch.begin(), ch.end(), static_cast<unsigned char>(0x5A));
    d0.run(idle, signals(640));
    bool silent = true;
    for (auto& ch : d0.out)
        for (unsigned char x : ch) silent = silent && x == 0;
    CHECK(silent);
    CHECK(idle.health().refused == 10 && idle.health().blocks == 0);

    AudioEngine e;
    e.prepare(fs);
    const DeckControl on;
    publishBoth(e, shippedPresetValues(4), shippedPresetValues(4), on, on);
    FakeDevice dev(SampleFormat::Int32In24, SampleFormat::Int32In24, 64);
    dev.run(e, signals(64));
    std::printf("   first period: %.1f %% of its time\n", e.health().lastPercent);
    e.resetHealth();
    gAllocs = 0;
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(0x1F80);
    dev.run(e, signals(48000), true);
    const unsigned csr = _mm_getcsr() & ~0x3Fu;
    _mm_setcsr(saved);
    EngineHealth h = e.health();
    std::printf("   %lld periods of 64 frames: heaviest %.1f %%, last %.1f %%, over half %lld, late %lld; %ld allocations; "
                "MXCSR 0x%04X\n",
                h.blocks, h.heaviestPercent, h.lastPercent, h.overHalf, h.late, gAllocs.load(), csr);
    CHECK(h.blocks == 750);
    CHECK(h.heaviestPercent > 0.0 && h.heaviestPercent >= h.lastPercent);
    CHECK(gAllocs.load() == 0);
    CHECK(csr == 0x1F80);
    e.resetHealth();
    dev.run(e, signals(64));
    h = e.health();
    CHECK(h.blocks == 1 && h.overHalf <= 1);
}

} // namespace

int main() {
    formats();
    bypassThroughDevice();
    formatsThroughDevice();
    decksApart();
    oneChannel();
    healthAndSafety();
    return test::finish("engine_test");
}
