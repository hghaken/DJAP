// A deck's colour (docs/design.md 3.3): tone EQ (bass shelf, low-mid bell, presence bell, treble shelf), the auto
// tone that pulls each song's balance towards a reference, a sub bass an octave under the bass, and a loudness
// contour for quiet listening. Right after the leveller and input gain, before the compressor: the dynamics take
// the extra energy, the leveller keeps the loudness. Every filter is a state-variable filter whose gain and frequency
// glide (25 ms) - no clicks, no delay; a section at 0 dB is skipped, so a neutral colour is bit-exact.
#pragma once

#include "dsp.h"
#include "params.h"

namespace ap {

constexpr int kColourBlock = 16; // samples per control() call: the deck's control block (deck.h kControlBlock)

// The colour part of a deck's configuration (trivially copyable, inside DeckConfig).
struct ColourConfig {
    static constexpr int kSections = 6; // bass, low-mid, presence, treble, loudness bass, loudness treble
    double gainDb[kSections] = {}, hz[kSections] = {100, 350, 3000, 8000, 100, 10000};
    double autoAmount = 0.0, autoMax = 6.0;   // 0..1, dB
    double autoA = 0.0, autoFastA = 0.0;      // control-rate one-poles of the measurement (slow, first seconds)
    double subGain = 0.0, subHz = 80.0;
    double smooth = 0.0;                      // control-rate one-pole of gains and frequencies (25 ms)
    BiquadCoeffs band[kToneBands];            // the auto tone's measuring band-passes
    bool any() const;                          // anything to do at all
};

ColourConfig makeColourConfig(const PresetValues& v, double fs, double ctrlRate);

class ColourProcessor {
public:
    void prepare(double fs) { fs_ = fs; }
    // Clean state; gains and frequencies at once at their targets (a configuration before the first audio).
    void reset(const ColourConfig& c);
    // Per sample: the deck's input, measured for the auto tone.
    inline void measure(double mono) {
        if (!measuring_) return;
        for (int b = 0; b < kToneBands; ++b) {
            const double y = band_[b].process(mono);
            acc_[b] += y * y;
        }
        ++accN_;
    }
    // Per control block: the auto tone's averages and corrections (hold: the music is quiet or falling - keep them;
    // learn: the first seconds of a song - measure fast), the glides, the coefficients.
    void control(const ColourConfig& c, bool hold, bool learn);
    // Per sample, in place.
    void process(const ColourConfig& c, double& l, double& r);
    bool finite() const;
    double correctionDb(int s) const { return s >= 0 && s < 4 ? corr_[s] : 0.0; } // auto tone, sections 0..3
    double sectionDb(int s) const { return gainS_[s]; }                            // fixed tone + auto tone now

private:
    void setSection(int s, bool ramp);
    void resetSub();

    double fs_ = 48000.0;
    // tone sections. A new design is reached in a straight line over the next control block, sample by sample:
    // a coefficient step from one block to the next would be a small jump on every block (a zipper on a bass tone)
    double gainS_[ColourConfig::kSections] = {}, hzS_[ColourConfig::kSections] = {};
    double designedDb_[ColourConfig::kSections] = {}, designedHz_[ColourConfig::kSections] = {}; // of next_
    bool on_[ColourConfig::kSections] = {};
    SvfCoeffs coef_[ColourConfig::kSections], next_[ColourConfig::kSections], step_[ColourConfig::kSections];
    int ramp_[ColourConfig::kSections] = {}; // samples left to reach next_
    SvfState st_[ColourConfig::kSections][2];
    // auto tone
    bool measuring_ = false, havePower_ = false;
    Biquad band_[kToneBands];
    double acc_[kToneBands] = {}, power_[kToneBands] = {};
    int accN_ = 0;
    double corr_[4] = {}; // dB on sections 0..3
    // sub bass (its gain and filters reached the same way)
    double subGainS_ = 0.0, subHzS_ = 80.0, env_ = 0.0, flip_ = 1.0, envAtt_ = 0.0, envRel_ = 0.0;
    double subGainNow_ = 0.0, subGainStep_ = 0.0;
    int subRamp_ = 0;
    bool subOn_ = false, positive_ = false;
    SvfCoeffs subIn_, subOut_, subInNext_, subOutNext_, subInStep_, subOutStep_;
    SvfState subSt_[4];
};

// Long-term balance of a mono signal in the auto tone's bands: each band's mean power against the 1 kHz band, in dB
// (silent stretches left out). For calibrating the reference and checking it (DJAPCmd --tone).
void toneBalance(const float* mono, size_t n, double fs, double out[kToneBands]);

} // namespace ap
