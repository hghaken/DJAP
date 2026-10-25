// Test signals with known answers (handbook 16.2).
#pragma once

#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sig {

using Buffer = std::vector<float>;

// Sine of peak level `dbfs`, `seconds` long, start phase in degrees.
inline Buffer sine(double fs, double hz, double dbfs, double seconds, double phaseDeg = 0.0) {
    const size_t n = static_cast<size_t>(seconds * fs + 0.5);
    Buffer b(n);
    const double a = ap::dbToLin(dbfs), w = 2.0 * ap::kPi * hz / fs, ph = phaseDeg * ap::kPi / 180.0;
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<float>(a * std::sin(w * static_cast<double>(i) + ph));
    return b;
}

inline Buffer silence(double fs, double seconds) { return Buffer(static_cast<size_t>(seconds * fs + 0.5), 0.0f); }

inline void append(Buffer& to, const Buffer& from) { to.insert(to.end(), from.begin(), from.end()); }

// Linear fades of `seconds` at both ends.
inline void fadeEnds(Buffer& b, double fs, double seconds) {
    const size_t n = static_cast<size_t>(seconds * fs);
    for (size_t i = 0; i < n && i < b.size(); ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(n);
        b[i] *= g;
        b[b.size() - 1 - i] *= g;
    }
}

// Deterministic generator (xorshift64*).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    // uniform in [-1, 1)
    double uniform() { return static_cast<double>(next() >> 11) / 4503599627370496.0 - 1.0; }
};

// White noise, uniform with the given peak amplitude.
inline Buffer whiteNoise(double fs, double seconds, double peak, uint64_t seed) {
    Rng r(seed);
    Buffer b(static_cast<size_t>(seconds * fs + 0.5));
    for (float& x : b) x = static_cast<float>(peak * r.uniform());
    return b;
}

// Pink-ish noise (P. Kellet's economy filter), scaled to roughly unit RMS times `rms`.
inline Buffer pinkNoise(double fs, double seconds, double rms, uint64_t seed) {
    Rng r(seed);
    Buffer b(static_cast<size_t>(seconds * fs + 0.5));
    double b0 = 0, b1 = 0, b2 = 0;
    for (float& x : b) {
        const double w = r.uniform();
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        x = static_cast<float>((b0 + b1 + b2 + w * 0.1848) * 0.25 * rms / 0.3);
    }
    return b;
}

// A crude "dance track": pink noise with a 4/4 pulse plus a 55 Hz kick every beat, at roughly `lufs`.
// Loudness is set afterwards by the caller with a measurement when exact values matter.
inline Buffer beatMusic(double fs, double seconds, double bpm, uint64_t seed) {
    Buffer b = pinkNoise(fs, seconds, 0.12, seed);
    const double beat = 60.0 / bpm;
    for (size_t i = 0; i < b.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        const double inBeat = std::fmod(t, beat);
        const double env = 0.55 + 0.45 * std::exp(-inBeat / 0.12);
        const double kick = 0.5 * std::exp(-inBeat / 0.15) * std::sin(2.0 * ap::kPi * 55.0 * inBeat);
        b[i] = static_cast<float>(b[i] * env + kick);
    }
    return b;
}

inline double peak(const Buffer& b, size_t from = 0) {
    double p = 0.0;
    for (size_t i = from; i < b.size(); ++i) p = std::max(p, static_cast<double>(std::fabs(b[i])));
    return p;
}

// Amplitude of the component at `hz` (single-bin DFT over the whole buffer from `from`; use whole periods).
inline double amplitudeAt(const Buffer& b, double fs, double hz, size_t from = 0) {
    double re = 0.0, im = 0.0;
    const double w = 2.0 * ap::kPi * hz / fs;
    for (size_t i = from; i < b.size(); ++i) {
        re += b[i] * std::cos(w * static_cast<double>(i));
        im -= b[i] * std::sin(w * static_cast<double>(i));
    }
    const double n = static_cast<double>(b.size() - from);
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

} // namespace sig
