// The user-visible parameters of one deck's chain, their metadata, and the shipped presets.
#pragma once

#include <array>
#include <string>

namespace ap {

enum Param : int {
    // general
    pTarget,
    pInputGain,
    pOutputGain,
    // leveller
    pLevOn,
    pLevRange,
    pLevSpeed,
    pLevAttack,    // s per 10 dB down while clearly too loud (was "Fast down", dB/s)
    pLevRelease,   // s per 10 dB up while clearly too soft, and after a pause (was a fixed fast-up and "Catch up")
    pLevHold,
    pLevQuietWait, // s a steady quiet section waits before it is levelled
    // compressor
    pCompOn,
    pCompThreshold,
    pCompRatio,
    pCompKnee,
    pCompAttack,
    pCompRelease,
    pCompSideHpf,
    pCompMakeup, // dB; negative = automatic
    // density
    pDensOn,
    pDensBands,
    pDensAmount,
    pDensDownAbove,
    pDensLiftBelow,
    pDensMaxLift,
    pDensAttack,
    pDensRelease,
    pDensSplit1,
    pDensSplit2,
    pDensSplit3,
    pDensSplit4,
    pDensSplit5,
    pDensSplit6,
    pDensGain1,
    pDensGain2,
    pDensGain3,
    pDensGain4,
    pDensGain5,
    pDensGain6,
    pDensGain7,
    // bass enhancer
    pBassOn,
    pBassFreq,
    pBassAmount,
    // exciter
    pExcOn,
    pExcFreq,
    pExcAmount,
    // de-esser
    pDessOn,
    pDessFreq,
    pDessThreshold,
    pDessMax,
    // stereo width
    pWidthOn,
    pWidth,
    pBassWidth,
    pWidthSplit,
    // soft clipper
    pClipOn,
    pClipDrive,
    // limiter
    pLimOn,
    pLimCeiling,
    pLimRelease,
    // ---- colour (a colour preset, chosen per deck apart from the preset above; docs/design.md 3.3) ----
    pToneBass,          // tone EQ: low shelf, low-mid bell, presence bell, high shelf
    pToneBassFreq,
    pToneLowMid,
    pToneLowMidFreq,
    pTonePresence,
    pTonePresenceFreq,
    pToneTreble,
    pToneTrebleFreq,
    pAutoAmount,        // auto tone: the balance per band pulled towards the reference
    pAutoMax,
    pAutoTime,
    pSubAmount,         // sub bass: an octave under the bass
    pSubFreq,
    pLoudness,          // loudness contour for quiet listening
    kParamCount
};
constexpr int kFirstColourParam = pToneBass;
inline bool isColourParam(int p) { return p >= kFirstColourParam; }

constexpr int kMaxBands = 7;
constexpr int kMaxSplits = kMaxBands - 1;

struct ParamInfo {
    const char* key;   // ini key
    const char* stage; // stage name shown in the GUI
    const char* name;  // parameter name shown in the GUI
    const char* unit;  // "" for counts and switches
    double min, max, def;
    double step;       // GUI step
    int decimals;
    bool isSwitch;
    const char* help;  // one line: what it does
};

const ParamInfo& paramInfo(int p);

// A parameter as positions 0 .. paramPositions(p) - the GUI's sliders and the plug-in's host steps: linear in the
// parameter's steps; a log scale for times and frequencies that span a factor of 8 or more (density splits 20 Hz -
// 16 kHz, releases: 300 positions); position 0 is "auto" for the compressor make-up and "off" for its side-chain
// high-pass.
int paramPositions(int p);
double paramValueAt(int p, int position); // rounded to the parameter's decimals, within its range
int paramPositionOf(int p, double value); // the nearest position

using PresetValues = std::array<double, kParamCount>;

// Generic defaults (the Medium preset).
PresetValues defaultValues();

// Gentle, Light, Medium, Loud, Very Loud (a ladder of strength), then FM Station, DAB+ and Web Stream (the sound and
// the norms of a medium; docs/design.md 3.1)
constexpr int kShippedPresetCount = 8;
constexpr int kStrengthPresetCount = 5;
const char* shippedPresetName(int i);
// Index of a shipped preset by name, -1 if none.
int shippedPresetIndex(const std::string& name);
PresetValues shippedPresetValues(int i);

// Colour presets: the same value array, of which only the colour parameters count (the rest stays at the defaults).
constexpr int kShippedColourCount = 9;
const char* shippedColourName(int i);
int shippedColourIndex(const std::string& name);
PresetValues shippedColourValues(int i);
// `dynamics` with the colour parameters of `colour`: what a deck runs.
PresetValues withColour(const PresetValues& dynamics, const PresetValues& colour);

// Auto tone (docs/design.md 3.3): the measured bands' centres and the reference balance - each band's long-term
// level against the 1 kHz band, in dB, as measured on recent club music (calibrated in session 3).
constexpr int kToneBands = 5;
extern const double kToneBandHz[kToneBands];
extern const double kToneReference[kToneBands];

// Clamps every value to its range, rounds counts and switches, and enforces the split rules for the band count.
void normaliseValues(PresetValues& v);

// The versions before Attack / Release (2026-10-27) had "LevellerFastDown" in dB/s (0 = the preset's Speed): the
// Attack in seconds per 10 dB it stands for. Saved presets (Settings) and plug-in projects use this one rule.
double attackFromFastDown(double fastDownDbPerS, double speedDbPerS);
// Default split frequencies for a band count (1..7) into split slots 1..6.
void loadDefaultSplits(PresetValues& v, int bands);
// Moves the other splits when one split was changed, so splits keep rising and >= 1.25x apart.
void enforceSplits(PresetValues& v, int changedSplit);

// The level the leveller aims at before the clipper: target + input gain - clipper drive (when the clipper is on).
// Compressor and de-esser thresholds are relative to it; the clipper's drive brings the output back to the target.
double workingLevel(const PresetValues& v);

} // namespace ap
