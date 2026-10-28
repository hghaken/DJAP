#include "params.h"

#include <algorithm>
#include <cmath>

namespace ap {

namespace {

// Ranges and generic defaults: docs/design.md section 3 (handbook 18, adapted for DJ use where marked there).
// The generic default of every value is the Medium preset's value.
const ParamInfo kInfo[kParamCount] = {
    {"Target", "General", "Target loudness", "LUFS", -24, -8, -14, 0.5, 1, false,
     "Output loudness the chain aims at (ITU-R BS.1770), at input gain 0"},
    {"InputGain", "General", "Input gain", "dB", -15, 18, 0, 0.5, 1, false,
     "Drives compressor, clipper and limiter harder; the output gets louder by the same amount"},
    {"OutputGain", "General", "Output gain", "dB", -15, 0, 0, 0.5, 1, false,
     "Last trim after the limiter; only down, so the ceiling holds"},

    {"LevellerOn", "Leveller", "On", "", 0, 1, 1, 1, 0, true, "Slow automatic gain that brings each song to the target"},
    {"LevellerRange", "Leveller", "Range", "dB", 0, 15, 9, 0.5, 1, false, "Largest boost or cut of the leveller"},
    {"LevellerSpeed", "Leveller", "Speed", "dB/s", 0.1, 6, 1.0, 0.1, 1, false,
     "How fast the leveller gain may move"},
    {"LevellerAttack", "Leveller", "Attack", "s", 0.2, 20, 1.0, 0.1, 1, false,
     "Seconds per 10 dB down while the music is clearly too loud (the gain more than 3 dB above what the 3 s level "
     "needs: a loud start after a quiet intro or a break); short = the loud part is down at once, long = a gentler "
     "turn of the knob. Never slower than Speed"},
    {"LevellerRelease", "Leveller", "Release", "s", 0.2, 20, 1.5, 0.1, 1, false,
     "Seconds per 10 dB up while the music is clearly too soft (a soft intro, a quiet song after a loud one) and "
     "when a new song starts after a pause; short = on level sooner, long = a gentler turn of the knob. Never "
     "slower than Speed"},
    {"LevellerHold", "Leveller", "Hold below", "LU", 5, 20, 10, 1, 0, false,
     "Holds the gain while the music is this far under the target (fades, pauses); a quiet part that stays is "
     "levelled after Quiet wait"},
    {"LevellerQuietWait", "Leveller", "Quiet wait", "s", 0.5, 10, 1, 0.5, 1, false,
     "How long a steady quiet part (more than Hold below under the target: a soft intro) waits before it is "
     "levelled; short = a soft intro comes up sooner, long = fades and breakdowns are surer to be left alone"},

    {"CompressorOn", "Compressor", "On", "", 0, 1, 1, 1, 0, true, "Wide-band compressor (soft knee, feed-forward)"},
    {"CompressorThreshold", "Compressor", "Threshold", "LU", -10, 15, 2, 0.5, 1, false,
     "Relative to the target: material louder than this is compressed"},
    {"CompressorRatio", "Compressor", "Ratio", ":1", 1, 8, 2.0, 0.1, 1, false, "Compression above the threshold"},
    {"CompressorKnee", "Compressor", "Knee", "dB", 0, 12, 6, 0.5, 1, false, "Width of the soft knee"},
    {"CompressorAttack", "Compressor", "Attack", "ms", 1, 100, 10, 1, 0, false, "Time to compress more"},
    {"CompressorRelease", "Compressor", "Release", "ms", 20, 1000, 200, 10, 0, false, "Time to let go"},
    {"CompressorSideHpf", "Compressor", "Side-chain high-pass", "Hz", 0, 200, 80, 5, 0, false,
     "Bass under this frequency does not trigger the compressor (0 = off)"},
    {"CompressorMakeup", "Compressor", "Make-up", "dB", -1, 12, -1, 0.5, 1, false,
     "Gain after compression; auto = the target level passes unchanged (written as -1)"},

    {"DensityOn", "Density", "On", "", 0, 1, 1, 1, 0, true,
     "Multiband stage: each band is compared with its own recent level; peaks down, details up"},
    {"DensityBands", "Density", "Bands", "", 1, 7, 5, 1, 0, false, "Number of bands (1 = wide-band)"},
    {"DensityAmount", "Density", "Amount", "%", 0, 100, 40, 5, 0, false, "Strength: ratio 1:1 at 0 % to 4:1 at 100 %"},
    {"DensityDownAbove", "Density", "Down above", "dB", 0, 12, 3, 0.5, 1, false,
     "A band this far above its recent level is turned down"},
    {"DensityLiftBelow", "Density", "Lift below", "dB", 0, 20, 6, 0.5, 1, false,
     "A band this far below its recent level is lifted"},
    {"DensityMaxLift", "Density", "Maximum lift", "dB", 0, 12, 6, 0.5, 1, false, "Largest lift of a band"},
    {"DensityAttack", "Density", "Attack", "ms", 1, 200, 20, 1, 0, false, "Time to turn a band down"},
    {"DensityRelease", "Density", "Release", "ms", 20, 2000, 300, 10, 0, false, "Time to turn a band back up"},
    {"DensitySplit1", "Density", "Split 1", "Hz", 20, 16000, 60, 5, 0, false, "Between band 1 and band 2"},
    {"DensitySplit2", "Density", "Split 2", "Hz", 20, 16000, 200, 5, 0, false, "Between band 2 and band 3"},
    {"DensitySplit3", "Density", "Split 3", "Hz", 20, 16000, 2500, 10, 0, false, "Between band 3 and band 4"},
    {"DensitySplit4", "Density", "Split 4", "Hz", 20, 16000, 6000, 50, 0, false, "Between band 4 and band 5"},
    {"DensitySplit5", "Density", "Split 5", "Hz", 20, 16000, 9000, 50, 0, false, "Between band 5 and band 6"},
    {"DensitySplit6", "Density", "Split 6", "Hz", 20, 16000, 12000, 50, 0, false, "Between band 6 and band 7"},
    {"DensityGain1", "Density", "Band 1 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 1"},
    {"DensityGain2", "Density", "Band 2 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 2"},
    {"DensityGain3", "Density", "Band 3 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 3"},
    {"DensityGain4", "Density", "Band 4 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 4"},
    {"DensityGain5", "Density", "Band 5 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 5"},
    {"DensityGain6", "Density", "Band 6 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 6"},
    {"DensityGain7", "Density", "Band 7 gain", "dB", -12, 12, 0, 0.5, 1, false, "Fixed gain of band 7"},

    {"BassOn", "Bass enhancer", "On", "", 0, 1, 0, 1, 0, true,
     "Adds overtones of the lowest bass so small speakers seem to play deeper"},
    {"BassFrequency", "Bass enhancer", "Frequency", "Hz", 60, 160, 100, 5, 0, false, "Bass below this gets overtones"},
    {"BassAmount", "Bass enhancer", "Amount", "%", 0, 100, 30, 5, 0, false, "Strength of the overtones"},

    {"ExciterOn", "Exciter", "On", "", 0, 1, 1, 1, 0, true, "Adds overtones to the highs (shine, air)"},
    {"ExciterFrequency", "Exciter", "Frequency", "Hz", 1500, 8000, 3500, 100, 0, false, "Highs above this are excited"},
    {"ExciterAmount", "Exciter", "Amount", "%", 0, 100, 15, 5, 0, false, "Strength of the added overtones"},

    {"DeEsserOn", "De-esser", "On", "", 0, 1, 1, 1, 0, true, "Turns sharp 's' sounds down with a high shelf"},
    {"DeEsserFrequency", "De-esser", "Frequency", "Hz", 3000, 10000, 5500, 100, 0, false, "Detector and shelf frequency"},
    {"DeEsserThreshold", "De-esser", "Threshold", "LU", -30, 0, -10, 1, 0, false,
     "Relative to the target: sibilance louder than this is reduced 4:1"},
    {"DeEsserMax", "De-esser", "Maximum reduction", "dB", 0, 12, 6, 0.5, 1, false, "Largest reduction"},

    {"WidthOn", "Stereo width", "On", "", 0, 1, 0, 1, 0, true, "Mid / side stereo width"},
    {"Width", "Stereo width", "Width", "%", 0, 200, 100, 5, 0, false, "100 % = as recorded, 0 % = mono"},
    {"BassWidth", "Stereo width", "Bass width", "%", 0, 100, 100, 5, 0, false,
     "Width of the bass below the split; 0 % = bass in mono"},
    {"WidthSplit", "Stereo width", "Bass split", "Hz", 40, 400, 150, 5, 0, false, "Where the bass width starts"},

    {"ClipperOn", "Soft clipper", "On", "", 0, 1, 1, 1, 0, true,
     "Rounds peaks without delay; denser sound at the same loudness"},
    {"ClipperDrive", "Soft clipper", "Drive", "dB", 0, 9, 2, 0.5, 1, false, "How hard peaks are driven into the curve"},

    {"LimiterOn", "Limiter", "On", "", 0, 1, 1, 1, 0, true, "Look-ahead true-peak limiter: the output never passes the ceiling"},
    {"LimiterCeiling", "Limiter", "Ceiling", "dBTP", -6, 0, -1.0, 0.1, 1, false, "Highest true-peak level of the output"},
    {"LimiterRelease", "Limiter", "Release", "ms", 10, 500, 80, 5, 0, false, "Time to let go after a peak"},

    {"Bass", "Tone", "Bass", "dB", -12, 12, 0, 0.5, 1, false,
     "Low shelf: more or less bass below its corner; the leveller keeps the loudness, the balance changes (0 = off)"},
    {"BassCorner", "Tone", "Bass corner", "Hz", 40, 250, 100, 5, 0, false, "Where the bass shelf starts"},
    {"LowMid", "Tone", "Low-mid", "dB", -12, 12, 0, 0.5, 1, false,
     "Bell around its frequency: less of it takes out boom and mud (0 = off)"},
    {"LowMidFrequency", "Tone", "Low-mid frequency", "Hz", 150, 1000, 350, 10, 0, false, "Centre of the low-mid bell"},
    {"Presence", "Tone", "Presence", "dB", -12, 12, 0, 0.5, 1, false,
     "Bell around its frequency: voices and leads forward or back (0 = off)"},
    {"PresenceFrequency", "Tone", "Presence frequency", "Hz", 1000, 6000, 3000, 100, 0, false,
     "Centre of the presence bell"},
    {"Treble", "Tone", "Treble", "dB", -12, 12, 0, 0.5, 1, false, "High shelf: more or less highs above its corner (0 = off)"},
    {"TrebleCorner", "Tone", "Treble corner", "Hz", 3000, 16000, 8000, 100, 0, false, "Where the treble shelf starts"},
    {"AutoTone", "Auto tone", "Amount", "%", 0, 100, 0, 5, 0, false,
     "Pulls each song's balance (bass, low-mid, presence, treble against the mids) towards that of late-80s dance "
     "and pop productions: a thin old recording gets more bass, a dull one more highs (0 = off)"},
    {"AutoToneMax", "Auto tone", "Maximum", "dB", 0, 9, 6, 0.5, 1, false,
     "Largest correction of a band (treble: half of it - the top of an old recording is often its hiss)"},
    {"AutoToneTime", "Auto tone", "Time", "s", 2, 60, 10, 1, 0, false,
     "How long a band is measured before it is corrected; faster for the first seconds of a song"},
    {"SubBass", "Sub bass", "Amount", "%", 0, 100, 0, 5, 0, false,
     "Adds a tone an octave under the bass - bass a recording does not have (0 = off; use sparingly)"},
    {"SubBassFrequency", "Sub bass", "Frequency", "Hz", 40, 120, 80, 5, 0, false,
     "Bass below this is followed an octave lower"},
    {"Loudness", "Contour", "Amount", "%", 0, 100, 0, 5, 0, false,
     "Equal-loudness contour for quiet listening: up to +9 dB of deep bass and +4 dB of highs (0 = off)"},
};

const char* const kShippedColourNames[kShippedColourCount] = {"Neutral", "Warm",  "Bass+", "Less mid", "Bright",
                                                             "Vocal",   "Oldies", "Auto",  "Quiet"};

const char* const kShippedNames[kShippedPresetCount] = {"Gentle", "Light",      "Medium", "Loud",
                                                        "Very Loud", "FM Station", "DAB+",  "Web Stream"};

// Default splits per band count (docs/design.md 3.2).
const double kSplits[kMaxBands + 1][kMaxSplits] = {
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0},
    {200, 0, 0, 0, 0, 0},
    {200, 2500, 0, 0, 0, 0},
    {120, 600, 4000, 0, 0, 0},
    {60, 200, 2500, 6000, 0, 0},
    {60, 200, 800, 2500, 6000, 0},
    {60, 150, 400, 1000, 2500, 6000},
};

constexpr double kSplitMin = 20.0, kSplitMax = 16000.0, kSplitRatio = 1.25;

} // namespace

const ParamInfo& paramInfo(int p) { return kInfo[p]; }

PresetValues defaultValues() {
    PresetValues v{};
    for (int i = 0; i < kParamCount; ++i) v[static_cast<size_t>(i)] = kInfo[i].def;
    return v;
}

const char* shippedPresetName(int i) { return i >= 0 && i < kShippedPresetCount ? kShippedNames[i] : ""; }

int shippedPresetIndex(const std::string& name) {
    for (int i = 0; i < kShippedPresetCount; ++i)
        if (name == kShippedNames[i]) return i;
    return -1;
}

PresetValues shippedPresetValues(int i) {
    PresetValues v = defaultValues();
    // Table 3.1 of docs/design.md: target, leveller range / speed, compressor threshold / ratio / attack / release,
    // density bands / amount / max lift, bass / exciter / de-esser, clipper drive, limiter release.
    struct Row {
        double target, levRange, levSpeed, thr, ratio, att, rel;
        int bands;
        double amount, maxLift, bass, exciter;
        bool deEsser;
        double drive, limRelease;
    };
    static const Row rows[kShippedPresetCount] = {
        {-18, 12, 0.5, 6, 1.5, 20, 300, 3, 20, 3, 0, 0, false, 0, 120},
        {-16, 9, 1.0, 4, 1.8, 15, 250, 4, 30, 4, 0, 0, true, 1, 100},
        {-14, 9, 1.0, 2, 2.0, 10, 200, 5, 40, 6, 0, 15, true, 2, 80},
        {-12, 12, 1.5, 0, 2.5, 10, 200, 6, 55, 6, 20, 20, true, 3, 60},
        {-10, 12, 1.5, -2, 3.0, 5, 150, 7, 70, 8, 30, 25, true, 4.5, 50},
        {-9, 12, 2.0, -3, 3.5, 5, 120, 5, 80, 10, 40, 10, true, 6, 40},   // FM Station (more below)
        {-23, 15, 0.5, 5, 2.0, 20, 300, 3, 0, 0, 0, 0, true, 0, 150},     // DAB+ (more below)
        {-16, 9, 1.0, 4, 1.8, 15, 250, 4, 30, 4, 0, 0, true, 0, 100},     // Web Stream (more below)
    };
    if (i < 0 || i >= kShippedPresetCount) return v;
    const Row& r = rows[i];
    v[pTarget] = r.target;
    v[pLevRange] = r.levRange;
    v[pLevSpeed] = r.levSpeed;
    // Quiet wait and Release (sliders since 2026-10-27; client: "set them to a reasonable average for all the
    // soft-to-loud songs"): 1 s and 1.5 s per 10 dB, chosen on 18 songs with soft intros through Loud - the time until
    // on level summed 322 -> 249 s and the seconds more than 6 LU too soft 601 -> 497 against the former 3 s / 3 dB/s,
    // the steepest rise 6.7 dB/s (1 s per 10 dB took 234 s, but at 10 dB/s - the "turn of the knob"), no song more
    // over the target. Gentle and DAB+ keep their calm: 3 s and 5 s (2 dB/s - faster brought quiet song endings up too
    // eagerly there)
    if (r.levSpeed <= 0.5) {
        v[pLevQuietWait] = 3.0;
        v[pLevRelease] = 5.0;
    }
    v[pCompThreshold] = r.thr;
    v[pCompRatio] = r.ratio;
    v[pCompAttack] = r.att;
    v[pCompRelease] = r.rel;
    v[pDensBands] = r.bands;
    loadDefaultSplits(v, r.bands);
    v[pDensAmount] = r.amount;
    v[pDensMaxLift] = r.maxLift;
    v[pBassOn] = r.bass > 0 ? 1 : 0;
    if (r.bass > 0) v[pBassAmount] = r.bass;
    v[pExcOn] = r.exciter > 0 ? 1 : 0;
    if (r.exciter > 0) v[pExcAmount] = r.exciter;
    v[pDessOn] = r.deEsser ? 1 : 0;
    v[pClipOn] = r.drive > 0 ? 1 : 0;
    if (r.drive > 0) v[pClipDrive] = r.drive;
    v[pLimRelease] = r.limRelease;
    if (i == 5) {
        // FM Station: the sound of a loud FM station - processed like one at -9 LUFS (a fast, dense 5-band, a full
        // low end, a wider image, hard clipping, the highs held down: FM's pre-emphasis leaves little room up there
        // and the de-esser stands in for the transmitter's high-frequency limiter), then 4 dB down to about -13 LUFS:
        // FM stations measure about -12 .. -15 LUFS, as the multiplex power limit (ITU-R BS.412) caps the average
        // level, while the sound stays as dense as the processing made it (peaks about 8-9 dB over the loudness).
        // Not a transmitter-ready signal: no 15 kHz low-pass, no stereo coder.
        v[pOutputGain] = -4;
        v[pDensLiftBelow] = 8;
        v[pDensAttack] = 10;
        v[pDensRelease] = 200;
        v[pDessThreshold] = -14;
        v[pDessMax] = 9;
        v[pWidthOn] = 1;
        v[pWidth] = 115;
        v[pBassWidth] = 50;
    } else if (i == 6) {
        // DAB+: the EBU R 128 way of digital radio - programme loudness -23 LUFS, true peak at most -1 dBTP, and the
        // music's dynamics left wide: songs are shifted to the target slowly (range 15 dB, 0.5 dB/s) rather than
        // squashed. The compressor only acts above -18 LUFS (5 LU over the target: R 128 s1's short-term maximum);
        // no density, no clipping, no exciter (a low bit-rate HE-AAC codec smears clipping and spends its bits on
        // added highs), sibilance kept down.
        v[pDensOn] = 0;
        v[pDessThreshold] = -12;
    } else if (i == 7) {
        // Web Stream: internet radio, podcasts and streams - -16 LUFS (AES TD1008 for portable listening; music
        // services normalise to -14 .. -16 and turn louder streams down anyway, so more density buys nothing),
        // processed like Light but without the clipper and with 2 dB below full scale (-2 dBTP): MP3 / AAC coding
        // raises the peaks a little and turns clipping into audible distortion.
        v[pLimCeiling] = -2.0;
    }
    return v;
}

// Measured bands of the auto tone and the reference balance: see params.h.
const double kToneBandHz[kToneBands] = {60, 250, 1000, 3000, 10000};
// The median of the late-1980s dance and pop productions among the client's test music (DJAPCmd --tone,
// session 3: Double Trouble, Milli Vanilli x2, New Kids "Hanging Tough", Petra & Co, UB40, Expose, Janet Jackson);
// the whole set of 45 songs spreads -6.4 .. +8.2 dB at 60 Hz and -17.5 .. -3.2 dB at 10 kHz.
const double kToneReference[kToneBands] = {5.9, 3.4, 0.0, -1.5, -5.2};

const char* shippedColourName(int i) { return i >= 0 && i < kShippedColourCount ? kShippedColourNames[i] : ""; }

int shippedColourIndex(const std::string& name) {
    for (int i = 0; i < kShippedColourCount; ++i)
        if (name == kShippedColourNames[i]) return i;
    return -1;
}

PresetValues shippedColourValues(int i) {
    PresetValues v = defaultValues();
    // bass, low-mid, presence, treble (dB); auto tone %, sub bass %, loudness % (docs/design.md 3.3)
    struct Row {
        double bass, lowMid, presence, treble, autoTone, sub, loudness;
    };
    static const Row rows[kShippedColourCount] = {
        {0, 0, 0, 0, 0, 0, 0},       // Neutral: nothing
        {3, -1, 0, -1.5, 0, 0, 0},   // Warm
        {5, 0, 0, 0, 0, 0, 0},       // Bass+
        {0, -3, -1.5, 0, 0, 0, 0},   // Less mid
        {0, 0, 2, 3, 0, 0, 0},       // Bright
        {-1, -2, 2.5, 0, 0, 0, 0},   // Vocal
        {1, 0, 0, 1, 70, 20, 0},     // Oldies: auto tone, a little sub bass
        {0, 0, 0, 0, 100, 0, 0},     // Auto: only the auto tone
        {0, 0, 0, 0, 0, 0, 60},      // Quiet: loudness contour
    };
    if (i < 0 || i >= kShippedColourCount) return v;
    const Row& r = rows[i];
    v[pToneBass] = r.bass;
    v[pToneLowMid] = r.lowMid;
    v[pTonePresence] = r.presence;
    v[pToneTreble] = r.treble;
    v[pAutoAmount] = r.autoTone;
    v[pSubAmount] = r.sub;
    v[pLoudness] = r.loudness;
    return v;
}

PresetValues withColour(const PresetValues& dynamics, const PresetValues& colour) {
    PresetValues v = dynamics;
    for (int p = kFirstColourParam; p < kParamCount; ++p) v[static_cast<size_t>(p)] = colour[static_cast<size_t>(p)];
    return v;
}

void loadDefaultSplits(PresetValues& v, int bands) {
    if (bands < 1) bands = 1;
    if (bands > kMaxBands) bands = kMaxBands;
    for (int s = 0; s < kMaxSplits; ++s) {
        const double d = kSplits[bands][s];
        v[static_cast<size_t>(pDensSplit1 + s)] = d > 0 ? d : kInfo[pDensSplit1 + s].def;
    }
}

void enforceSplits(PresetValues& v, int changed) {
    const int used = static_cast<int>(v[pDensBands]) - 1;
    if (used <= 0) return;
    if (changed < 0) changed = 0;
    if (changed >= used) changed = used - 1;
    auto s = [&](int k) -> double& { return v[static_cast<size_t>(pDensSplit1 + k)]; };
    const double lo = kSplitMin * std::pow(kSplitRatio, changed);
    const double hi = kSplitMax / std::pow(kSplitRatio, used - 1 - changed);
    if (s(changed) < lo) s(changed) = lo;
    if (s(changed) > hi) s(changed) = hi;
    for (int j = changed + 1; j < used; ++j)
        if (s(j) < s(j - 1) * kSplitRatio) s(j) = s(j - 1) * kSplitRatio;
    for (int j = changed - 1; j >= 0; --j)
        if (s(j) > s(j + 1) / kSplitRatio) s(j) = s(j + 1) / kSplitRatio;
    // Keep the outermost splits inside the range (pushing may have moved them out).
    for (int j = used - 1; j >= 0; --j) {
        double cap = kSplitMax / std::pow(kSplitRatio, used - 1 - j);
        if (j + 1 < used && s(j + 1) / kSplitRatio < cap) cap = s(j + 1) / kSplitRatio;
        if (s(j) > cap) s(j) = cap;
    }
    for (int j = 0; j < used; ++j) {
        double floor = kSplitMin * std::pow(kSplitRatio, j);
        if (j > 0 && s(j - 1) * kSplitRatio > floor) floor = s(j - 1) * kSplitRatio;
        if (s(j) < floor) s(j) = floor;
    }
}

double attackFromFastDown(double fastDown, double speed) {
    return 10.0 / (fastDown > 0.0 ? fastDown : std::max(0.1, speed));
}

void normaliseValues(PresetValues& v) {
    for (int i = 0; i < kParamCount; ++i) {
        const ParamInfo& info = kInfo[i];
        double& x = v[static_cast<size_t>(i)];
        if (!std::isfinite(x)) x = info.def;
        if (x < info.min) x = info.min;
        if (x > info.max) x = info.max;
        if (info.isSwitch) x = x >= 0.5 ? 1.0 : 0.0;
    }
    v[pDensBands] = std::round(v[pDensBands]);
    if (v[pCompSideHpf] > 0 && v[pCompSideHpf] < 40) v[pCompSideHpf] = 40;
    if (v[pCompMakeup] < 0) v[pCompMakeup] = -1;
    enforceSplits(v, 0);
}

namespace {
constexpr int kLogPositions = 300;

struct Scale {
    bool log = false, special = false;
    double lo = 0.0, hi = 1.0;
    int positions = 1; // the highest position
};

Scale scaleOf(int p) {
    const ParamInfo& in = paramInfo(p);
    Scale s;
    s.lo = in.min;
    s.hi = in.max;
    if (p == pCompMakeup) s.special = true, s.lo = 0.0;
    if (p == pCompSideHpf) s.special = true, s.lo = 40.0;
    const std::string unit = in.unit;
    s.log = (unit == "Hz" || unit == "ms" || unit == "s") && s.lo > 0.0 && s.hi / s.lo >= 8.0;
    s.positions = s.log ? kLogPositions : static_cast<int>(std::lround((s.hi - s.lo) / in.step));
    if (s.special) ++s.positions;
    return s;
}
} // namespace

int paramPositions(int p) { return scaleOf(p).positions; }

double paramValueAt(int p, int pos) {
    const ParamInfo& in = paramInfo(p);
    const Scale s = scaleOf(p);
    pos = std::min(s.positions, std::max(0, pos));
    if (s.special) {
        if (pos <= 0) return p == pCompMakeup ? -1.0 : 0.0;
        --pos;
    }
    const int n = s.positions - (s.special ? 1 : 0);
    double v = s.log ? std::round(s.lo * std::pow(s.hi / s.lo, static_cast<double>(pos) / n) / in.step) * in.step
                     : s.lo + pos * in.step;
    const double scale = std::pow(10.0, in.decimals);
    v = std::round(v * scale) / scale; // no 0.30000000000000004
    return std::min(s.hi, std::max(s.lo, v));
}

int paramPositionOf(int p, double v) {
    const Scale s = scaleOf(p);
    int off = 0;
    if (s.special) {
        if ((p == pCompMakeup && v < 0.0) || (p == pCompSideHpf && v <= 0.0)) return 0;
        off = 1;
    }
    const int n = s.positions - off;
    const double t = s.log ? std::log(std::max(v, s.lo) / s.lo) / std::log(s.hi / s.lo) * n
                           : (v - s.lo) / paramInfo(p).step;
    return off + static_cast<int>(std::min<long>(n, std::max<long>(0, std::lround(t))));
}

double workingLevel(const PresetValues& v) {
    return v[pTarget] + v[pInputGain] - (v[pClipOn] >= 0.5 ? v[pClipDrive] : 0.0);
}

} // namespace ap
