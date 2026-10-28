// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers to run one deck processor on test signals.
#pragma once

#include "deck.h"
#include "loudness.h"
#include "signals.h"

#include <memory>

namespace tdeck {

using ap::PresetValues;
using sig::Buffer;

// Every stage off (a neutral chain), the rest of the values at their defaults.
inline PresetValues allOff() {
    PresetValues v = ap::defaultValues();
    for (int p = 0; p < ap::kParamCount; ++p)
        if (ap::paramInfo(p).isSwitch) v[static_cast<size_t>(p)] = 0.0;
    return v;
}

struct Stereo {
    Buffer l, r;
};

// Runs a deck over a signal in chunks; align = true removes the look-ahead delay.
inline Stereo run(double fs, const PresetValues& v, const Buffer& l, const Buffer& r, double lookMs = 1.5,
                  bool align = true, int chunk = 256, ap::DeckProcessor* external = nullptr) {
    std::unique_ptr<ap::DeckProcessor> own;
    ap::DeckProcessor* d = external;
    if (!d) {
        own.reset(new ap::DeckProcessor());
        d = own.get();
        d->prepare(fs);
    }
    ap::DeckControl ctl;
    ctl.lookMs = lookMs;
    d->publish(ap::makeDeckConfig(v, ctl, fs));
    d->process(nullptr, nullptr, 0);
    const int lat = d->latency();
    const size_t n = l.size(), total = n + (align ? static_cast<size_t>(lat) : 0);
    Stereo o;
    o.l.assign(total, 0.0f);
    o.r.assign(total, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        o.l[i] = l[i];
        o.r[i] = r[i];
    }
    for (size_t pos = 0; pos < total; pos += static_cast<size_t>(chunk)) {
        const int m = static_cast<int>(std::min(static_cast<size_t>(chunk), total - pos));
        d->process(o.l.data() + pos, o.r.data() + pos, m);
    }
    if (align) {
        o.l.erase(o.l.begin(), o.l.begin() + lat);
        o.r.erase(o.r.begin(), o.r.begin() + lat);
    }
    return o;
}

inline double loudness(double fs, const Buffer& l, const Buffer& r, size_t from = 0, size_t to = 0) {
    if (!to || to > l.size()) to = l.size();
    ap::LoudnessMeter m;
    ap::measureStereo(m, fs, l.data() + from, r.data() + from, to - from);
    return m.integrated();
}

// Scales a stereo pair so its integrated loudness is `lufs`.
inline void setLoudness(double fs, Buffer& l, Buffer& r, double lufs) {
    const double now = loudness(fs, l, r);
    const float g = static_cast<float>(ap::dbToLin(lufs - now));
    for (auto& x : l) x *= g;
    for (auto& x : r) x *= g;
}

// RMS level in dB over [from, from + n).
inline double rmsDb(const Buffer& b, size_t from, size_t n) {
    double s = 0.0;
    for (size_t i = from; i < from + n && i < b.size(); ++i) s += static_cast<double>(b[i]) * b[i];
    return 10.0 * std::log10(s / static_cast<double>(n) + 1e-30);
}

} // namespace tdeck
