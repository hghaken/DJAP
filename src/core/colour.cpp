#include "colour.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ap {

namespace {

constexpr double kBandQ = 1.0;              // measuring band-passes: about 1.4 octaves wide
constexpr double kBellQ = 0.9, kShelfQ = 0.70710678118654752;
constexpr double kLoudBassDb = 9.0, kLoudTrebleDb = 4.0; // loudness contour at 100 %
constexpr double kLoudBassHz = 100.0, kLoudTrebleHz = 10000.0;
constexpr double kRedesignDb = 0.005, kRedesignHz = 0.05; // a section is designed again after this much glide

SvfCoeffs::Mode modeOf(int s) {
    return s == 0 || s == 4 ? SvfCoeffs::LowShelf : s == 3 || s == 5 ? SvfCoeffs::HighShelf : SvfCoeffs::Bell;
}

// The section the auto tone's band b corrects (band 2, 1 kHz, is the reference): -1 for none.
int bandOfSection(int s) { return s == 0 ? 0 : s == 1 ? 1 : s == 2 ? 3 : s == 3 ? 4 : -1; }

// One sample's step from `from` to `to` in kColourBlock samples, and taking it.
SvfCoeffs stepTo(const SvfCoeffs& from, const SvfCoeffs& to) {
    const double n = kColourBlock;
    SvfCoeffs s;
    s.a1 = (to.a1 - from.a1) / n;
    s.a2 = (to.a2 - from.a2) / n;
    s.a3 = (to.a3 - from.a3) / n;
    s.m0 = (to.m0 - from.m0) / n;
    s.m1 = (to.m1 - from.m1) / n;
    s.m2 = (to.m2 - from.m2) / n;
    return s;
}
inline void addStep(SvfCoeffs& c, const SvfCoeffs& s) {
    c.a1 += s.a1;
    c.a2 += s.a2;
    c.a3 += s.a3;
    c.m0 += s.m0;
    c.m1 += s.m1;
    c.m2 += s.m2;
}

} // namespace

bool ColourConfig::any() const {
    for (double g : gainDb)
        if (g != 0.0) return true;
    return autoAmount > 0.0 || subGain > 0.0;
}

ColourConfig makeColourConfig(const PresetValues& v, double fs, double ctrlRate) {
    ColourConfig c;
    const Param gain[4] = {pToneBass, pToneLowMid, pTonePresence, pToneTreble};
    const Param freq[4] = {pToneBassFreq, pToneLowMidFreq, pTonePresenceFreq, pToneTrebleFreq};
    for (int s = 0; s < 4; ++s) {
        c.gainDb[s] = v[gain[s]];
        c.hz[s] = v[freq[s]];
    }
    const double loud = v[pLoudness] / 100.0;
    c.gainDb[4] = kLoudBassDb * loud;
    c.hz[4] = kLoudBassHz;
    c.gainDb[5] = kLoudTrebleDb * loud;
    c.hz[5] = kLoudTrebleHz;
    c.autoAmount = v[pAutoAmount] / 100.0;
    c.autoMax = v[pAutoMax];
    c.autoA = onePoleCoeff(v[pAutoTime], ctrlRate);
    c.autoFastA = onePoleCoeff(1.5, ctrlRate);
    c.subGain = v[pSubAmount] / 100.0;
    c.subHz = v[pSubFreq];
    c.smooth = onePoleCoeff(0.025, ctrlRate);
    for (int b = 0; b < kToneBands; ++b) c.band[b] = design::bandPass(fs, kToneBandHz[b], kBandQ);
    return c;
}

void ColourProcessor::reset(const ColourConfig& c) {
    for (int s = 0; s < ColourConfig::kSections; ++s) {
        gainS_[s] = c.gainDb[s];
        hzS_[s] = c.hz[s];
        st_[s][0].reset();
        st_[s][1].reset();
        setSection(s, false);
        on_[s] = gainS_[s] != 0.0;
    }
    for (int b = 0; b < kToneBands; ++b) {
        band_[b].c = c.band[b];
        band_[b].reset();
        acc_[b] = power_[b] = 0.0;
    }
    accN_ = 0;
    havePower_ = false;
    measuring_ = c.autoAmount > 0.0;
    for (double& x : corr_) x = 0.0;
    subGainS_ = subGainNow_ = c.subGain;
    subHzS_ = c.subHz;
    envAtt_ = onePoleCoeff(0.002, fs_);
    envRel_ = onePoleCoeff(0.040, fs_);
    resetSub();
    subOn_ = subGainS_ > 0.0;
}

void ColourProcessor::resetSub() {
    subIn_ = subInNext_ = SvfCoeffs::design(SvfCoeffs::LowPass, fs_, subHzS_, 0.0, kShelfQ);
    subOut_ = subOutNext_ = SvfCoeffs::design(SvfCoeffs::LowPass, fs_, 0.6 * subHzS_, 0.0, kShelfQ);
    subRamp_ = 0;
    for (SvfState& s : subSt_) s.reset();
    env_ = 0.0;
    flip_ = 1.0;
    positive_ = false;
}

void ColourProcessor::setSection(int s, bool ramp) {
    designedDb_[s] = gainS_[s];
    designedHz_[s] = hzS_[s];
    next_[s] = SvfCoeffs::design(modeOf(s), fs_, hzS_[s], gainS_[s], modeOf(s) == SvfCoeffs::Bell ? kBellQ : kShelfQ);
    if (ramp) {
        step_[s] = stepTo(coef_[s], next_[s]);
        ramp_[s] = kColourBlock;
    } else {
        coef_[s] = next_[s];
        ramp_[s] = 0;
    }
}

void ColourProcessor::control(const ColourConfig& c, bool hold, bool learn) {
    // auto tone: each band's long-term power against the 1 kHz band, compared with the reference
    if (c.autoAmount > 0.0) {
        if (!measuring_) { // switched on: start measuring from scratch
            for (int b = 0; b < kToneBands; ++b) {
                band_[b].reset();
                acc_[b] = 0.0;
            }
            accN_ = 0;
            havePower_ = false;
            measuring_ = true;
        }
        if (accN_ > 0 && !hold) {
            const double a = learn ? c.autoFastA : c.autoA;
            for (int b = 0; b < kToneBands; ++b) {
                const double p = acc_[b] / accN_;
                power_[b] = havePower_ ? toward(power_[b], p, a) : p;
            }
            havePower_ = true;
        }
        if (havePower_ && power_[2] > 1e-10) {
            for (int s = 0; s < 4; ++s) {
                const int b = bandOfSection(s);
                const double rel = 10.0 * std::log10(std::max(power_[b], 1e-20) / power_[2]);
                // treble at most half: the top of an old recording is often its hiss
                const double max = s == 3 ? 0.5 * c.autoMax : c.autoMax;
                corr_[s] = clampd((kToneReference[b] - rel) * c.autoAmount, -max, max);
            }
        }
    } else {
        measuring_ = false;
        for (double& x : corr_) x = 0.0; // the sections glide back to the fixed tone
    }
    for (int b = 0; b < kToneBands; ++b) acc_[b] = 0.0;
    accN_ = 0;

    // tone sections: glide, design again when they moved enough, skip at 0 dB
    for (int s = 0; s < ColourConfig::kSections; ++s) {
        const double target = c.gainDb[s] + (s < 4 ? corr_[s] : 0.0);
        if (gainS_[s] != target || hzS_[s] != c.hz[s]) {
            gainS_[s] = toward(gainS_[s], target, c.smooth);
            hzS_[s] = toward(hzS_[s], c.hz[s], c.smooth);
            if (std::fabs(gainS_[s] - target) < 1e-3) gainS_[s] = target;
            if (std::fabs(hzS_[s] - c.hz[s]) < 1e-2) hzS_[s] = c.hz[s];
        }
        const bool arrived = gainS_[s] == target && hzS_[s] == c.hz[s];
        if (std::fabs(gainS_[s] - designedDb_[s]) >= kRedesignDb || std::fabs(hzS_[s] - designedHz_[s]) >= kRedesignHz ||
            (arrived && (gainS_[s] != designedDb_[s] || hzS_[s] != designedHz_[s])))
            setSection(s, true);
        // on while it has a gain or is still ramping; arrived at 0 dB (the identity): off, with a clean state
        const bool on = gainS_[s] != 0.0 || ramp_[s] > 0;
        if (on_[s] && !on) {
            st_[s][0].reset();
            st_[s][1].reset();
        }
        on_[s] = on;
    }

    // sub bass: amount and frequency glide; gain and filters reached sample by sample over the block
    if (!subOn_ && c.subGain > 0.0) { // starting: a clean generator, from silence
        resetSub();
        subGainNow_ = 0.0;
        subOn_ = true;
    }
    if (subGainS_ != c.subGain || subHzS_ != c.subHz || subGainNow_ != subGainS_) {
        subGainS_ = toward(subGainS_, c.subGain, c.smooth);
        subHzS_ = toward(subHzS_, c.subHz, c.smooth);
        if (std::fabs(subGainS_ - c.subGain) < 1e-4) subGainS_ = c.subGain;
        if (std::fabs(subHzS_ - c.subHz) < 1e-2) subHzS_ = c.subHz;
        subInNext_ = SvfCoeffs::design(SvfCoeffs::LowPass, fs_, subHzS_, 0.0, kShelfQ);
        subOutNext_ = SvfCoeffs::design(SvfCoeffs::LowPass, fs_, 0.6 * subHzS_, 0.0, kShelfQ);
        subInStep_ = stepTo(subIn_, subInNext_);
        subOutStep_ = stepTo(subOut_, subOutNext_);
        subGainStep_ = (subGainS_ - subGainNow_) / kColourBlock;
        subRamp_ = kColourBlock;
    }
    if (subOn_ && subGainS_ == 0.0 && subGainNow_ == 0.0 && subRamp_ == 0) subOn_ = false;
}

void ColourProcessor::process(const ColourConfig&, double& l, double& r) {
    if (subOn_) {
        if (subRamp_ > 0) {
            if (--subRamp_ == 0) {
                subIn_ = subInNext_;
                subOut_ = subOutNext_;
                subGainNow_ = subGainS_;
            } else {
                addStep(subIn_, subInStep_);
                addStep(subOut_, subOutStep_);
                subGainNow_ += subGainStep_;
            }
        }
        // the bass, low-passed; a flip-flop at every rising zero crossing halves its frequency; a square of the
        // bass's envelope, low-passed again, is a tone an octave down that follows the bass
        const double low = subSt_[1].process(subIn_, subSt_[0].process(subIn_, 0.5 * (l + r)));
        const double m = std::fabs(low);
        env_ = toward(env_, m, m > env_ ? envAtt_ : envRel_);
        const double hyst = 0.1 * env_;
        if (!positive_ && low > hyst) {
            positive_ = true;
            flip_ = -flip_;
        } else if (positive_ && low < -hyst) {
            positive_ = false;
        }
        const double sub = subSt_[3].process(subOut_, subSt_[2].process(subOut_, flip_ * env_));
        l += subGainNow_ * sub;
        r += subGainNow_ * sub;
    }
    for (int s = 0; s < ColourConfig::kSections; ++s) {
        if (!on_[s]) continue;
        if (ramp_[s] > 0) {
            if (--ramp_[s] == 0) coef_[s] = next_[s];
            else addStep(coef_[s], step_[s]);
        }
        l = st_[s][0].process(coef_[s], l);
        r = st_[s][1].process(coef_[s], r);
    }
}

bool ColourProcessor::finite() const {
    for (int s = 0; s < ColourConfig::kSections; ++s)
        if (!st_[s][0].finite() || !st_[s][1].finite()) return false;
    for (const SvfState& s : subSt_)
        if (!s.finite()) return false;
    for (const Biquad& b : band_)
        if (!b.finite()) return false;
    return std::isfinite(env_ + power_[0] + power_[1] + power_[2] + power_[3] + power_[4]);
}

void toneBalance(const float* mono, size_t n, double fs, double out[kToneBands]) {
    Biquad band[kToneBands];
    for (int b = 0; b < kToneBands; ++b) band[b].c = design::bandPass(fs, kToneBandHz[b], kBandQ);
    const size_t block = static_cast<size_t>(0.1 * fs);
    double sum[kToneBands] = {};
    for (size_t pos = 0; pos + block <= n; pos += block) {
        double total = 0.0, part[kToneBands] = {};
        for (size_t i = pos; i < pos + block; ++i) {
            const double x = mono[i];
            total += x * x;
            for (int b = 0; b < kToneBands; ++b) {
                const double y = band[b].process(x);
                part[b] += y * y;
            }
        }
        if (total / block < 1e-5) continue; // under -50 dBFS: silence, a gap, a fade's tail
        for (int b = 0; b < kToneBands; ++b) sum[b] += part[b];
    }
    for (int b = 0; b < kToneBands; ++b)
        out[b] = sum[2] > 0.0 && sum[b] > 0.0 ? 10.0 * std::log10(sum[b] / sum[2]) : 0.0;
}

} // namespace ap
