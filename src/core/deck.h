// One deck's processing chain (docs/design.md section 2):
// leveller gain + input gain -> colour (sub bass, tone, loudness) -> compressor -> density (1-7 bands) -> bass
// enhancer -> exciter -> de-esser -> stereo width -> [leveller detector] -> soft clipper -> look-ahead true-peak
// limiter -> output gain -> clip guard -> [volume].
#pragma once

#include "colour.h"
#include "dsp.h"
#include "limiter.h"
#include "loudness.h"
#include "params.h"
#include "realtime.h"
#include "truepeak.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace ap {

enum Stage : int { sLeveller, sCompressor, sDensity, sBass, sExciter, sDeEsser, sWidth, sClipper, sLimiter, kStageCount };

constexpr double kMaxLookMs = 5.0;
constexpr double kMaxInputVolume = 100.0; // largest input factor of setVolume (+40 dB)
constexpr double kVolumeGlideS = 0.020;   // setVolume's values glide with this one-pole
constexpr double kFadeOutMs = 10.0;       // startFadeOut: raised-cosine fade to silence before a stream stops
constexpr int kControlBlock = 16;      // frames per control-rate update
static_assert(kControlBlock == kColourBlock, "the colour stage ramps its coefficients over one control block");
// Limiter's true-peak detector. Measured on full-band noise bursts (limiter_test --sweep): 8x with 32 taps per
// phase let the true peak 1.07 dB over the ceiling, 64 taps 0.27-0.34 dB, 96 taps 0.08-0.09 dB (Kaiser beta 5).
constexpr int kTpFactorLimiter = 8;
constexpr int kTpTapsLimiter = 96;     // taps per phase -> 48 frames of detector delay
constexpr double kTpBetaLimiter = 5.0;
// The shortest look-ahead above 0: the detector's delay (taps / 2 frames) plus a limiter window of kMinTpWindow frames.
// A shorter one let the limiter fall back to the sample peak without a word - true peaks up to 3 dB over the ceiling
// (outside review, 2026-10-26; client: "do it") - and a window of one frame lost the smoothing (+0.18 dB on noise).
// 16 frames: 1.43 ms at 44.1 kHz, 1.31 ms at 48 kHz; the default 1.5 ms is above it at every rate.
constexpr int kMinTpWindow = 16;
constexpr int kMinLookFrames = kTpTapsLimiter / 2 + kMinTpWindow - 1;
// The look-ahead in frames for `lookMs` at `fs`: 0 stays 0 (sample-peak limiting, no delay - the user's choice);
// anything else is at least kMinLookFrames. One rule for the deck, the latency a plug-in reports and the status bar.
inline int lookFrames(double lookMs, double fs) {
    const double ms = lookMs >= 0.0 ? (lookMs <= kMaxLookMs ? lookMs : kMaxLookMs) : 0.0; // NaN: 0
    if (ms <= 0.0) return 0;
    const long f = std::lround(ms * fs / 1000.0);
    return static_cast<int>(f > kMinLookFrames ? f : kMinLookFrames);
}
constexpr int kTpFactorMeter = 4;      // output meter (EBU Tech 3341 tolerance)
constexpr int kTpTapsMeter = 32;

// Settings that are not part of a preset.
struct DeckControl {
    double lookMs = 1.5;     // limiter look-ahead, the same for both decks
    bool processing = true;  // false = bypass (input delayed by the look-ahead, bit-exact)
    bool freeze = false;     // leveller holds its gain
};

// Everything the audio thread needs, computed on the GUI thread (no coefficient design on the audio thread except
// the de-esser's shelf). Trivially copyable: delivered by sequence lock.
struct DeckConfig {
    double fs = 48000.0;
    int look = 0;
    bool processing = true, freeze = false;
    bool on[kStageCount] = {};
    double inputGain = 1.0, outputGain = 1.0;
    double switchStep = 0.0, gainSmooth = 0.0;
    // leveller (control rate)
    double aim = -14.0, levRange = 9.0, levStep = 0.0, levFastStep = 0.0, levFastUpStep = 0.0, holdLufs = -24.0,
           catchStep = 0.0;
    double levDetMax = 1e30; // the most power one control block may add to the leveller detector
    bool catchUp = true;
    double aFast = 0.0, aMid = 0.0, aSlow = 0.0;
    // compressor (per sample)
    BiquadCoeffs compHpf;
    bool compHpfOn = true;
    double compThr = -12.0, compRatio = 2.0, compKnee = 6.0, compAtt = 0.0, compRel = 0.0, compDet = 0.0, compMakeup = 0.0;
    // density (control rate)
    int bands = 5;
    BiquadCoeffs densLp[kMaxSplits], densHp[kMaxSplits], densAp[kMaxSplits];
    double densRatio = 2.0, densDown = 3.0, densLift = 6.0, densMaxLift = 6.0, densAtt = 0.0, densRel = 0.0;
    double densLevel = 0.0, densRef = 0.0, densRefFast = 0.0, densGainSmooth = 0.0;
    double densBandGain[kMaxBands] = {1, 1, 1, 1, 1, 1, 1};
    // bass enhancer
    BiquadCoeffs bassLp4, bassHp, bassLp;
    double bassGain = 0.0;
    // exciter
    BiquadCoeffs excHp;
    double excGain = 0.0;
    // de-esser
    BiquadCoeffs dessHp;
    double dessFreq = 5500.0, dessThr = -24.0, dessMax = 6.0, dessDet = 0.0, dessAtt = 0.0, dessRel = 0.0;
    // stereo width
    BiquadCoeffs widthLp, widthHp, widthAp;
    double width = 1.0, bassWidth = 1.0;
    // colour: tone, auto tone, sub bass, loudness (colour.h)
    ColourConfig colour;
    // soft clipper
    double clipDrive = 1.0, clipCeiling = 1.0;
    // limiter
    double limCeiling = 1.0, limRelease = 0.0;
};

DeckConfig makeDeckConfig(const PresetValues& values, const DeckControl& control, double fs);

// The soft-knee static curve of the compressor (Giannoulis, Massberg & Reiss 2012), in dB.
double compressorCurveDb(double x, double threshold, double ratio, double knee);
// Soft clipper curve, input scaled to the ceiling (1 = ceiling): straight to 0.5, then tanh towards 1.
double softClip(double u);

// 10 ms of measurements from the audio thread (handbook 5.3).
// The points of the chain whose level the analysis window's signal-chain bars compare (client, 2026-10-25: "for
// every step a bar: does it do anything?"): the input (after the input volume), then after each step, in chain order.
enum ChainTap : int {
    ctInput,
    ctLeveller, // leveller gain and input gain
    ctColour,
    ctCompressor,
    ctDensity,
    ctBass,
    ctExciter,
    ctDeEsser,
    ctWidth,
    ctClipper,
    ctLimiter,  // delayed by the look-ahead against the points before it
    ctOutput,   // after the output gain and the bypass (what leaves the deck, before the volume of setVolume)
    kChainTaps
};
constexpr int kChainColourBit = kStageCount; // MeterRecord::active: the colour has something to do

struct MeterRecord {
    uint32_t frames = 0;
    float chain[kChainTaps] = {};      // sums of L^2 + R^2 (not weighted) at each ChainTap
    uint16_t active = 0;               // bit s: stage s runs (Stage); kChainColourBit: the colour
    float energyIn = 0, energyOut = 0; // sums of K-weighted L^2 + R^2 over the frames
    float peakIn[2] = {0, 0};          // sample peak of the input
    float tpOut[2] = {0, 0};           // true peak of the output
    float levellerDb = 0;              // leveller gain
    float compressorDb = 0;            // largest compressor reduction (dB, >= 0)
    float compressorIn = -120;         // compressor detector level at the record's end (same scale as compThr)
    float densityDb[kMaxBands] = {};   // density band gains (dynamic part)
    float autoToneDb[4] = {};          // the auto tone's corrections now: bass, low-mid, presence, treble
    float deEsserDb = 0;               // largest de-esser reduction
    float clipperDb = 0;               // largest clipper reduction
    float limiterDb = 0;               // largest limiter reduction
    uint8_t bands = 1;
    uint8_t flags = 0;                 // MeterFlag bits
};
enum MeterFlag : uint8_t { mfQuiet = 1, mfFrozen = 2, mfWarm = 4, mfBypassed = 8, mfReconfiguring = 16 };

using MeterRing = SpscRing<MeterRecord, 512>;

// Every frame's input and output, for the GUI's spectrum and stereo graphs (analysis.h). 0.34 s at 48 kHz: the GUI
// drains it 20 times a second.
struct TapFrame {
    float inL, inR, outL, outR;
};
using TapRing = SpscRing<TapFrame, 16384>;

class DeckProcessor {
public:
    DeckProcessor();
    ~DeckProcessor();
    DeckProcessor(const DeckProcessor&) = delete;
    DeckProcessor& operator=(const DeckProcessor&) = delete;
    // Sets the sample rate and allocates everything. Not real-time safe; call before processing starts.
    void prepare(double fs);
    // After prepare(), before the first audio: the first 5 ms of output fade in (a live stream that starts in the
    // middle of the music; AudioEngine::prepare). Offline renders and tests start bit-exact without it.
    void startFadeIn() {
        fadeInStart_ = true;
        lookState_ = 3;
        lookFade_ = 0.0;
    }
    // GUI thread, before a live stream stops: the output fades out over kFadeOutMs and stays silent (until the next
    // prepare()); fadedOut() turns true when it is silent, so the stream can stop without a step - a click (client,
    // 2026-10-25). processedFrames() tells whether the deck runs at all (a deck without devices never fades).
    void startFadeOut() { fadeOutReq_.store(true, std::memory_order_release); }
    bool fadedOut() const { return fadedOut_.load(std::memory_order_acquire); }
    uint64_t processedFrames() const { return processed_.load(std::memory_order_relaxed); }
    double sampleRate() const { return fs_; }
    int maxLook() const { return maxLook_; }

    // GUI thread: publish a new configuration (from makeDeckConfig).
    void publish(const DeckConfig& config) { slot_.publish(config); }
    // GUI thread: a volume after the whole chain (0..1, e.g. Windows' volume; meters and taps measure before it, the
    // ceiling holds) and a factor on the input (0..kMaxInputVolume: undoes a volume Windows already applied to the
    // deck's input, so the processing sees the music's own level). Both glide (kVolumeGlideS); 1 and 1 change nothing,
    // bit for bit. Set before prepare(), they apply from the first sample.
    void setVolume(double output, double input = 1.0) {
        volOut_.store(output, std::memory_order_relaxed);
        volIn_.store(input, std::memory_order_relaxed);
    }

    // Audio thread: processes `frames` stereo frames in place. No allocation, no lock, no I/O.
    void process(float* left, float* right, int frames);

    // Current latency in frames (the look-ahead in use).
    int latency() const { return curLook_; }
    MeterRing& meters() { return *meters_; }
    TapRing& taps() { return *taps_; }
    // Number of times a non-finite state was found and the chain reset.
    int resets() const { return resets_; }

private:
    struct DensityBank;
    void applyConfig(const DeckConfig& c);
    void resetStates();
    void resetStage(int s);
    void controlUpdate();
    bool statesFinite() const;
    void configureBank(DensityBank& b, const DeckConfig& c);
    void startBankFade(const DeckConfig& c);

    double fs_ = 48000.0;
    int maxLook_ = 0;
    SeqLockSlot<DeckConfig> slot_;
    DeckConfig cfg_;
    bool haveConfig_ = false;
    bool fresh_ = true; // no audio processed since prepare()
    bool fadeInStart_ = false; // startFadeIn(): the first configuration keeps the fade-in

    // switches
    double mix_[kStageCount] = {};
    bool running_[kStageCount] = {};
    double masterMix_ = 1.0;
    bool settled_ = false; // no switch ramp, gain smoothing or retune running: the switch loop is skipped
    double inGain_ = 1.0, outGain_ = 1.0;

    // look-ahead reconfiguration (handbook 9.4)
    int curLook_ = 0, wantLook_ = 0, lookState_ = 0, refill_ = 0;
    double lookFade_ = 1.0;
    std::vector<float> dryL_, dryR_;
    int dryPos_ = 0;

    // meters / detectors on the input and after the width stage
    StereoKWeighting kIn_, kPost_, kOut_;
    TruePeakDetector tpOutL_, tpOutR_;
    double blockIn_ = 0.0, blockPost_ = 0.0;

    // control state
    int ctrlPos_ = 0;
    double fastIn_ = 0.0, midIn_ = 0.0; // incoming K-weighted power, 0.4 s and 1.5 s
    double postFast_ = 0.0, postMid_ = 0.0, postSlow_ = 0.0; // the leveller detector: 0.4 s, 1.5 s, 3 s
    double postGated_ = 0.0; // 0.4 s, blocks far under the 1.5 s level left out: the trend (falling, steady)
    double quietTime_ = 1e9, silenceTime_ = 1e9, musicTime_ = 0.0;
    bool quiet_ = true, longSilence_ = true, catchPending_ = false, catching_ = false;
    bool pauseSeen_ = true; // the quiet time so far held at least 1 s of silence (a pause: warm start after it)
    bool fastDown_ = false; // the leveller comes down at the fast speed until it reaches the wanted gain
    bool fastUp_ = false;   // ... or goes up at the fast-up speed (clearly too soft, not falling)
    bool fell_ = false;     // the level fell (or a quiet section was released): the 3 s level restarts after it
    double quietSteady_ = 0.0; // seconds the music has stayed under the hold level, steady (a quiet section)
    static constexpr int kLevHist = 24;
    double midHist_[kLevHist] = {}; // the gated level (LUFS) every 0.25 s for the last 6 s
    double fallTime_ = 0.0;         // seconds a detected fall still counts
    int histIdx_ = 0, histBlocks_ = 0;
    double catchTime_ = 0.0; // seconds since the catch-up started
    double riseHist_[4] = {kFloorDb, kFloorDb, kFloorDb, kFloorDb}; // 0.4 s level (LUFS) every 0.25 s
    int riseIdx_ = 0, riseBlocks_ = 0;
    double levGainDb_ = 0.0, levGainLin_ = 1.0;

    // compressor
    StereoBiquad compHpf_;
    // make-up smoothed: the automatic make-up moves with threshold, ratio and knee
    double compDet_ = 0.0, compRed_ = 0.0, compMakeupS_ = 0.0;

    // density: two banks for click-free band-count changes
    std::unique_ptr<DensityBank> banks_[2];
    int activeBank_ = 0;
    bool bankFading_ = false;  // crossfading from the active bank to the other one
    double bankXf_ = 0.0;     // -1..0: the new bank warms up unheard; 0..1: crossfade
    bool bankPending_ = false; // another band-count change waits for the running crossfade

    // bass enhancer
    Biquad2 bassLp4_;
    Biquad bassHp_, bassLp_;
    double bassGainS_ = 0.0;
    bool bassRetune_ = false; // new frequency: the stage fades out, takes the new filters, fades back in
    // exciter
    StereoBiquad2 excHp_, excHp2_;
    StereoOversampler2x excOs_, clipOs_; // the exciter's saturation and the soft clipper run at twice the rate
    // de-esser
    StereoBiquad2 dessHp_;
    StereoBiquad dessShelf_;
    double dessDet_ = 0.0, dessRed_ = 0.0;
    bool dessEngaged_ = false;
    // stereo width: a split change runs in the spare filter set and crossfades (20 ms), like the density banks
    struct WidthFilters {
        Biquad2 lp, hp;
        Biquad ap;
        BiquadCoeffs split; // the low-pass it was set with
        void set(const DeckConfig& c) {
            lp.set(c.widthLp);
            hp.set(c.widthHp);
            ap.c = c.widthAp;
            split = c.widthLp;
        }
        void reset() {
            lp.reset();
            hp.reset();
            ap.reset();
        }
        bool finite() const { return lp.finite() && hp.finite() && ap.finite(); }
    };
    void startWidthFade();
    WidthFilters width_[2];
    int widthActive_ = 0;
    bool widthFading_ = false, widthPending_ = false;
    double widthXf_ = 0.0, widthS_ = 1.0, bassWidthS_ = 1.0; // crossfade (as bankXf_), smoothed widths
    // colour (tone, auto tone, sub bass, loudness), right after the leveller and input gain
    ColourProcessor colour_;
    // soft clipper
    double clipDriveS_ = 1.0;
    // limiter
    Limiter limiter_;
    // volume after the chain and factor on the input (setVolume), smoothed
    std::atomic<double> volOut_{1.0}, volIn_{1.0};
    // fade-out before a stop (startFadeOut): request and result shared with the GUI, the position the audio thread's
    std::atomic<bool> fadeOutReq_{false}, fadedOut_{false};
    std::atomic<uint64_t> processed_{0};
    bool fadingOut_ = false;
    double fadeOutPos_ = 0.0; // 0..1 through the fade; 1 = silent
    double volOutS_ = 1.0, volInS_ = 1.0, volGlide_ = 0.0;

    // meter record being filled
    MeterRecord rec_;
    unsigned recordFrames_ = 480;
    double recIn_ = 0.0, recOut_ = 0.0, recClipRatio_ = 1.0;
    double recChain_[kChainTaps] = {}; // MeterRecord::chain, summed in double
    DeckConfig incoming_;
    std::unique_ptr<MeterRing> meters_;
    std::unique_ptr<TapRing> taps_;
    int resets_ = 0;
};

} // namespace ap
