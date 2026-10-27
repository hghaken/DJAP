#include "deck.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ap {

namespace {
// leveller fall rule: no upward move while the 0.4 s level is this far (power ratio) under the 3 s level
const double kFallHoldLu = 3.0;
const double kFallHold = std::pow(10.0, -kFallHoldLu / 10.0);
// catch-up continues while the 0.4 s level rose more than this over the last second (a fade-in; a crescendo inside a
// song is slower), at most kCatchMaxSeconds
const double kRiseCatchDb = 1.0;
const double kCatchMaxSeconds = 15.0;
// fast down: when the gain is more than kFastDownDb above what the 3 s level needs (clearly too loud: a loud entry
// after a quiet intro or a break) it comes down at the preset's fast speed until it is within kFastDownDoneDb
const double kFastDownDb = 3.0;
const double kFastDownDoneDb = 0.5;
// The leveller on a song with quiet verses and loud choruses (client, 2026-10-26, a song whose spoken verses lie
// 10 LU under its choruses: after each chorus the output stayed 6-8 LU under the target for 10-19 s on every preset).
// Measured causes: the gaps between the phrases dipped the 0.4 s level under the hold level and under the fall rule
// (no move for half the verse, and a 1.5 s warm restart after every gap of 1 s), the 3 s level needed 6-8 s to come
// down to the verse, and the normal speed then needed another 7 s. Now:
// - falling is judged on a gated 0.4 s level (as 1 s averages): blocks more than kGateDb under the 1.5 s level (the gaps between
//   phrases, a pause) leave it where it is, so a gap is not a fall. Falling = it dropped more than kDropDb over the
//   last 2 s (a fade of 1.25 dB/s or more, the start of a quiet passage), or it sank more than kSinkDb in each of the
//   last two 2 s (a slow fade, 0.4 dB/s or more: it goes on falling, while a quiet passage is flat again
//   after its drop). No move up while falling (a slow fade at 1 dB/s was turned up by 12 dB without this rule);
// - when it has stopped falling, the 3 s level starts from the level reached instead of its lag;
// - a quiet section under the hold level that stays (kQuietMusicSeconds, not falling, the gated level not more
//   than kSteadyDb under its value 4 s ago, and not more than kQuietMusicLu under the hold level: music, not a noise
//   floor) is levelled like any music; fades, pauses and the first seconds of a breakdown keep the hold;
// - warm restart only after a real pause (at least 1 s of silence), not after a quiet passage;
// - clearly too soft (the gain more than kFastUpDb under what the 3 s level needs, not falling): up at four times the
//   preset's speed but at most kFastUpMax, or at twice its speed when that is more (Gentle and DAB+, 0.5 dB/s: 2;
//   Light, Medium, Web Stream, Loud: 3; FM Station: 4 - 3 dB/s for all brought the quiet endings of three songs up too
//   eagerly in Gentle and DAB+, 2 dB/s left the spoken verses 1 LU more uneven in the 1 dB/s presets), until within
//   kFastUpDoneDb - the counterpart of fast down; meanwhile a gap between two phrases does not stop it (only a fall does).
const double kDropDb = 2.5;
const double kSinkDb = 0.8;
const double kFallKeepSeconds = 1.0;
const double kGateDb = 10.0;
const double kGate = std::pow(10.0, -kGateDb / 10.0);
const double kQuietMusicSeconds = 3.0;
const double kSteadyDb = 3.5;
const double kQuietMusicLu = 12.0;
const double kFastUpDb = 4.0;
const double kFastUpDoneDb = 1.0;
const double kFastUpMax = 3.0; // dB/s

// Crossfade position (-1..0 warm-up, 0..1 fade) to weight: raised cosine, so the fade starts and ends without a
// corner (a linear fade's corner showed as a click on a 120 Hz tone).
inline double crossfadeWeight(double x) { return x <= 0.0 ? 0.0 : 0.5 - 0.5 * std::cos(kPi * x); }

bool sameCoeffs(const BiquadCoeffs& a, const BiquadCoeffs& b) {
    return a.b0 == b.b0 && a.b1 == b.b1 && a.b2 == b.b2 && a.a1 == b.a1 && a.a2 == b.a2;
}
} // namespace

double compressorCurveDb(double x, double t, double ratio, double knee) {
    const double over = x - t;
    if (2.0 * over < -knee) return x;                     // below the knee
    if (knee > 0.0 && 2.0 * std::fabs(over) <= knee) {    // inside: quadratic
        const double k = over + knee / 2.0;
        return x + (1.0 / ratio - 1.0) * k * k / (2.0 * knee);
    }
    return t + over / ratio;                              // above
}

double softClip(double u) {
    const double knee = 0.5, m = std::fabs(u);
    if (m <= knee) return u;
    const double k = 1.0 - knee, y = knee + k * fastTanh((m - knee) / k);
    return u < 0.0 ? -y : y;
}

// The clipper's curve against its ceiling with the limiter off: this far under it - the clipper's hard hold on the
// ceiling then seldom cuts a peak (the clipper in process(); fourth outside review). With the limiter on: 1.
constexpr double kClipOwnHeadroomDb = 1.5;

DeckConfig makeDeckConfig(const PresetValues& in, const DeckControl& ctl, double fs) {
    PresetValues v = in;
    normaliseValues(v);
    DeckConfig c;
    c.fs = fs;
    c.look = lookFrames(ctl.lookMs, fs);
    c.oversampling = snapOversampling(ctl.oversampling);
    c.processing = ctl.processing;
    c.freeze = ctl.freeze;
    auto on = [&](Param p) { return v[p] >= 0.5; };
    c.on[sLeveller] = on(pLevOn);
    c.on[sCompressor] = on(pCompOn);
    c.on[sDensity] = on(pDensOn);
    c.on[sBass] = on(pBassOn);
    c.on[sExciter] = on(pExcOn);
    c.on[sDeEsser] = on(pDessOn);
    c.on[sWidth] = on(pWidthOn);
    c.on[sClipper] = on(pClipOn);
    c.on[sLimiter] = on(pLimOn);
    c.inputGain = dbToLin(v[pInputGain]);
    c.outputGain = dbToLin(v[pOutputGain]);
    c.switchStep = 1.0 / (0.020 * fs); // 20 ms switch ramps
    c.gainSmooth = onePoleCoeff(0.005, fs);
    const double ctrlRate = fs / kControlBlock;

    // leveller
    c.aim = workingLevel(v);
    c.levRange = v[pLevRange];
    c.levStep = v[pLevSpeed] / ctrlRate;
    c.levFastStep = std::max(v[pLevFastDown], v[pLevSpeed]) / ctrlRate; // 0 = the normal speed
    c.levFastUpStep = std::max(2.0 * v[pLevSpeed], std::min(kFastUpMax, 4.0 * v[pLevSpeed])) / ctrlRate;
    // a block far louder than the leveller could ever correct is a glitch, not music: one sample far over full scale
    // (and the low filters ringing after it) must not make fast down pull the gain to the end of its range
    c.levDetMax = lufsToPower(c.aim + c.levRange + 6.0);
    c.holdLufs = c.aim - v[pLevHold];
    c.catchUp = on(pLevCatchUp);
    c.catchStep = 20.0 / ctrlRate; // catch-up after a pause: 20 dB/s
    c.aFast = onePoleCoeff(0.4, ctrlRate);
    c.aMid = onePoleCoeff(1.5, ctrlRate);
    c.aSlow = onePoleCoeff(3.0, ctrlRate);

    // compressor
    c.compHpfOn = v[pCompSideHpf] > 0.0;
    if (c.compHpfOn) c.compHpf = design::highPass(fs, v[pCompSideHpf]);
    c.compThr = c.aim + v[pCompThreshold];
    c.compRatio = v[pCompRatio];
    c.compKnee = v[pCompKnee];
    c.compAtt = onePoleCoeff(v[pCompAttack] / 1000.0, fs);
    c.compRel = onePoleCoeff(v[pCompRelease] / 1000.0, fs);
    c.compDet = onePoleCoeff(0.010, fs);
    c.compMakeup = v[pCompMakeup] < 0.0 ? c.aim - compressorCurveDb(c.aim, c.compThr, c.compRatio, c.compKnee)
                                        : v[pCompMakeup];

    // density
    c.bands = static_cast<int>(v[pDensBands]);
    for (int s = 0; s + 1 < c.bands; ++s) {
        const double f = v[static_cast<size_t>(pDensSplit1 + s)];
        c.densLp[s] = design::lowPass(fs, f);
        c.densHp[s] = design::highPass(fs, f);
        c.densAp[s] = design::allPass(fs, f);
    }
    c.densRatio = 1.0 + 3.0 * v[pDensAmount] / 100.0;
    c.densDown = v[pDensDownAbove];
    c.densLift = v[pDensLiftBelow];
    c.densMaxLift = v[pDensMaxLift];
    c.densAtt = onePoleCoeff(v[pDensAttack] / 1000.0, ctrlRate);
    c.densRel = onePoleCoeff(v[pDensRelease] / 1000.0, ctrlRate);
    c.densLevel = onePoleCoeff(0.010, ctrlRate);
    c.densRef = onePoleCoeff(3.0, ctrlRate);
    c.densRefFast = onePoleCoeff(0.3, ctrlRate);
    c.densGainSmooth = onePoleCoeff(0.005, ctrlRate);
    for (int b = 0; b < kMaxBands; ++b) c.densBandGain[b] = dbToLin(v[static_cast<size_t>(pDensGain1 + b)]);

    // bass enhancer (Larsen & Aarts): rectified mono bass, band-passed to f .. 4f
    const double bf = v[pBassFreq];
    c.bassLp4 = design::lowPass(fs, bf);
    c.bassHp = design::highPass(fs, bf);
    c.bassLp = design::lowPass(fs, 4.0 * bf);
    c.bassGain = 2.4 * v[pBassAmount] / 100.0;

    // exciter
    c.excHp = design::highPass(fs, v[pExcFreq]);
    c.excGain = v[pExcAmount] / 100.0;

    // de-esser
    c.dessHp = design::highPass(fs, v[pDessFreq]);
    c.dessFreq = v[pDessFreq];
    c.dessThr = c.aim + v[pDessThreshold];
    c.dessMax = v[pDessMax];
    c.dessDet = onePoleCoeff(0.002, fs);
    c.dessAtt = onePoleCoeff(0.001, ctrlRate);
    c.dessRel = onePoleCoeff(0.080, ctrlRate);

    // stereo width
    c.widthLp = design::lowPass(fs, v[pWidthSplit]);
    c.widthHp = design::highPass(fs, v[pWidthSplit]);
    c.widthAp = design::allPass(fs, v[pWidthSplit]);
    c.width = v[pWidth] / 100.0;
    c.bassWidth = v[pBassWidth] / 100.0;

    // bass level
    c.colour = makeColourConfig(v, fs, ctrlRate);

    // soft clipper and limiter
    c.clipDrive = dbToLin(v[pClipDrive]);
    c.clipCeiling = dbToLin(v[pLimCeiling]);
    c.clipBend = c.on[sLimiter] ? 1.0 : dbToLin(-kClipOwnHeadroomDb);
    c.limCeiling = c.on[sLimiter] ? dbToLin(v[pLimCeiling]) : 1e9; // off: the gain releases to 1
    c.limRelease = onePoleCoeff(v[pLimRelease] / 1000.0, fs);
    return c;
}

// ---------------------------------------------------------------------------------------------------------------
// Density filter bank: cascade of LR4 splits; every lower band passes the all-passes of the splits above it, so the
// bands sum to an all-pass (handbook 10.4).

struct DeckProcessor::DensityBank {
    static constexpr int kAllPasses = kMaxSplits * (kMaxSplits - 1) / 2;
    int bands = 1;
    StereoBiquad2 lp[kMaxSplits], hp[kMaxSplits]; // both channels in one register
    StereoBiquad ap[kAllPasses];
    double acc[kMaxBands] = {}, level[kMaxBands] = {}, ref[kMaxBands] = {}, gainDb[kMaxBands] = {};
    double fixed[kMaxBands] = {1, 1, 1, 1, 1, 1, 1};
    double gPrev[kMaxBands] = {1, 1, 1, 1, 1, 1, 1}, gNext[kMaxBands] = {1, 1, 1, 1, 1, 1, 1};
    double learnTime = 0.0;
    BiquadCoeffs splitSet[kMaxSplits]; // the low-passes it was set with

    // Same band count and splits as `c`: nothing to crossfade.
    bool matches(const DeckConfig& c) const {
        if (bands != c.bands) return false;
        for (int s = 0; s + 1 < bands; ++s)
            if (!sameCoeffs(splitSet[s], c.densLp[s])) return false;
        return true;
    }

    // Takes over the levels and gains of a bank with the same band count (a split change keeps the band state).
    void copyState(const DensityBank& o) {
        for (int b = 0; b < kMaxBands; ++b) {
            acc[b] = o.acc[b];
            level[b] = o.level[b];
            ref[b] = o.ref[b];
            gainDb[b] = o.gainDb[b];
            fixed[b] = o.fixed[b];
            gPrev[b] = o.gPrev[b];
            gNext[b] = o.gNext[b];
        }
        learnTime = o.learnTime;
    }

    void reset(const DeckConfig& c) {
        for (int s = 0; s < kMaxSplits; ++s) {
            lp[s].reset();
            hp[s].reset();
        }
        for (int k = 0; k < kAllPasses; ++k) ap[k].reset();
        for (int b = 0; b < kMaxBands; ++b) {
            acc[b] = level[b] = ref[b] = gainDb[b] = 0.0;
            fixed[b] = gPrev[b] = gNext[b] = c.densBandGain[b];
        }
        learnTime = 0.0;
    }

    void setCoeffs(const DeckConfig& c) {
        bands = c.bands;
        for (int s = 0; s < kMaxSplits; ++s) splitSet[s] = c.densLp[s];
        for (int s = 0; s + 1 < bands; ++s) {
            lp[s].set(c.densLp[s]);
            hp[s].set(c.densHp[s]);
        }
        int k = 0;
        for (int b = 0; b + 2 < bands; ++b)
            for (int j = b + 1; j + 1 < bands; ++j) ap[k++].set(c.densAp[j]);
    }

    inline void split(Pair x, Pair* part) {
        const int splits = bands - 1;
        Pair rest = x;
        for (int s = 0; s < splits; ++s) {
            part[s] = lp[s].process(rest);
            rest = hp[s].process(rest);
        }
        part[splits] = rest;
        int k = 0;
        for (int b = 0; b + 1 < splits; ++b)
            for (int j = b + 1; j < splits; ++j) part[b] = ap[k++].process(part[b]);
    }

    inline void run(double l, double r, double frac, double& outL, double& outR) {
        Pair p[kMaxBands];
        split(makePair(l, r), p);
        Pair out = _mm_setzero_pd();
        for (int b = 0; b < bands; ++b) {
            const Pair sq = _mm_mul_pd(p[b], p[b]);
            acc[b] += leftOf(sq) + rightOf(sq);
            const double g = gPrev[b] + (gNext[b] - gPrev[b]) * frac;
            out = _mm_add_pd(out, _mm_mul_pd(p[b], _mm_set1_pd(g)));
        }
        outL = leftOf(out);
        outR = rightOf(out);
    }

    void control(const DeckConfig& c, bool quiet, double dt) {
        if (!quiet) learnTime += dt;
        const bool warm = learnTime < 0.5; // fresh reference: learn fast, keep the gain at 0 dB
        const double k = 1.0 - 1.0 / c.densRatio;
        for (int b = 0; b < bands; ++b) {
            const double ms = acc[b] / kControlBlock;
            acc[b] = 0.0;
            level[b] = toward(level[b], ms, c.densLevel);
            if (!quiet) ref[b] = toward(ref[b], level[b], warm ? c.densRefFast : c.densRef);
            if (ref[b] < level[b] * 1e-3) ref[b] = level[b] * 1e-3;
            const double excess = 10.0 * std::log10((level[b] + 1e-20) / (ref[b] + 1e-20));
            double target = 0.0;
            if (!warm) {
                if (excess > c.densDown)
                    target = -std::min(24.0, (excess - c.densDown) * k);
                else if (!quiet && excess < -c.densLift && excess > -40.0)
                    target = std::min(c.densMaxLift, (-c.densLift - excess) * k);
            }
            gainDb[b] = target < gainDb[b] ? toward(gainDb[b], target, c.densAtt) : toward(gainDb[b], target, c.densRel);
            fixed[b] = toward(fixed[b], c.densBandGain[b], c.densGainSmooth);
            gPrev[b] = gNext[b];
            gNext[b] = dbToLin(gainDb[b]) * fixed[b];
        }
    }

    bool finite() const {
        for (int b = 0; b < bands; ++b)
            if (!std::isfinite(level[b]) || !std::isfinite(ref[b]) || !std::isfinite(gainDb[b])) return false;
        for (int s = 0; s + 1 < bands; ++s)
            if (!lp[s].finite() || !hp[s].finite()) return false;
        return true;
    }
};

// ---------------------------------------------------------------------------------------------------------------

DeckProcessor::DeckProcessor() : meters_(new MeterRing()), taps_(new TapRing()) {
    banks_[0].reset(new DensityBank());
    banks_[1].reset(new DensityBank());
}

DeckProcessor::~DeckProcessor() = default;

void DeckProcessor::prepare(double fs) {
    fs_ = fs;
    colour_.prepare(fs);
    maxLook_ = static_cast<int>(std::ceil(kMaxLookMs * fs / 1000.0));
    dryL_.assign(static_cast<size_t>(maxLook_ + 1), 0.0f);
    dryR_.assign(static_cast<size_t>(maxLook_ + 1), 0.0f);
    limiter_.prepare(maxLook_, kTpFactorLimiter, kTpTapsLimiter, kTpBetaLimiter);
    tpOutL_.configure(kTpFactorMeter, kTpTapsMeter);
    tpOutR_.configure(kTpFactorMeter, kTpTapsMeter);
    const KWeightingCoeffs k = kWeightingCoeffs(fs);
    kIn_.set(k);
    kPost_.set(k);
    kOut_.set(k);
    recordFrames_ = static_cast<unsigned>(std::lround(fs / 100.0));
    volGlide_ = onePoleCoeff(kVolumeGlideS, fs);
    volOutS_ = clampd(volOut_.load(std::memory_order_relaxed), 0.0, 1.0);
    volInS_ = clampd(volIn_.load(std::memory_order_relaxed), 0.0, kMaxInputVolume);
    haveConfig_ = false;
    fresh_ = true;
    fadeInStart_ = false;
    fadeOutReq_.store(false, std::memory_order_relaxed);
    fadedOut_.store(false, std::memory_order_relaxed);
    fadingOut_ = false;
    fadeOutPos_ = 0.0;
    applyConfig(makeDeckConfig(defaultValues(), DeckControl(), fs));
    resetStates();
}

void DeckProcessor::configureBank(DensityBank& b, const DeckConfig& c) { b.setCoeffs(c); }

// A new band count or new splits run in the spare bank; the output crossfades to it (20 ms).
void DeckProcessor::startBankFade(const DeckConfig& c) {
    const DensityBank& now = *banks_[activeBank_];
    DensityBank& next = *banks_[1 - activeBank_];
    next.reset(c);
    if (now.bands == c.bands) next.copyState(now);
    configureBank(next, c);
    bankXf_ = -1.0; // 20 ms warm-up: a filter started cold on running audio starts with a transient
    bankFading_ = true;
}

void DeckProcessor::startWidthFade() {
    WidthFilters& next = width_[1 - widthActive_];
    next.reset();
    next.set(cfg_);
    widthXf_ = -1.0; // warm-up, as for the density banks
    widthFading_ = true;
}

void DeckProcessor::applyConfig(const DeckConfig& c) {
    if (c.fs != fs_) return; // made for another rate: ignore
    const bool first = !haveConfig_ || fresh_; // before any audio: take everything at once, no ramps
    cfg_ = c;
    settled_ = false;
    haveConfig_ = true;
    compHpf_.set(c.compHpf);
    if (first || !running_[sBass]) {
        bassLp4_.set(c.bassLp4);
        bassHp_.c = c.bassHp;
        bassLp_.c = c.bassLp;
        bassRetune_ = false;
    } else if (!sameCoeffs(bassHp_.c, c.bassHp)) {
        bassRetune_ = true;
    }
    excHp_.set(c.excHp);
    excHp2_.set(c.excHp);
    // a new oversampling factor: at once when the stages rest (resetStage takes it when they start), else they fade
    // out and back in with it
    if (first) {
        excOs_.setFactor(c.oversampling);
        clipOs_.setFactor(c.oversampling);
        osRetune_ = false;
    } else if ((running_[sExciter] && excOs_.factor() != c.oversampling) ||
               (running_[sClipper] && clipOs_.factor() != c.oversampling)) {
        osRetune_ = true;
    } else {
        osRetune_ = false; // back to the factor they run at (A -> B -> A while fading): they fade straight back in
    }
    dessHp_.set(c.dessHp);
    if (first || !running_[sWidth]) {
        width_[widthActive_].set(c);
        widthFading_ = widthPending_ = false;
    } else if (widthFading_) {
        if (!sameCoeffs(width_[1 - widthActive_].split, c.widthLp)) widthPending_ = true; // after this crossfade
    } else if (!sameCoeffs(width_[widthActive_].split, c.widthLp)) {
        startWidthFade();
    }
    limiter_.setParams(c.limCeiling, c.limRelease);
    wantLook_ = std::min(std::max(0, c.look), maxLook_);

    if (first) {
        curLook_ = wantLook_;
        limiter_.setLook(curLook_);
        // a live stream's first audio fades in over 5 ms (the look-ahead change's fade-in; startFadeIn): a stream
        // started in the middle of the music would begin with a step - a hard click (client, 2026-10-25)
        lookState_ = fadeInStart_ ? 3 : 0;
        lookFade_ = fadeInStart_ ? 0.0 : 1.0;
        banks_[activeBank_]->reset(c);
        configureBank(*banks_[activeBank_], c);
        bankFading_ = false;
        bankPending_ = false;
        for (int s = 0; s < kStageCount; ++s) {
            mix_[s] = c.on[s] ? 1.0 : 0.0;
            running_[s] = c.on[s];
        }
        masterMix_ = c.processing ? 1.0 : 0.0;
        inGain_ = c.inputGain;
        outGain_ = c.outputGain;
        compMakeupS_ = c.compMakeup;
        bassGainS_ = c.bassGain;
        widthS_ = c.width;
        bassWidthS_ = c.bassWidth;
        colour_.reset(c.colour);
        return;
    }

    // density band count / splits
    if (!running_[sDensity]) {
        banks_[activeBank_]->reset(c);
        configureBank(*banks_[activeBank_], c);
        bankFading_ = bankPending_ = false;
    } else if (bankFading_) {
        if (!banks_[1 - activeBank_]->matches(c)) bankPending_ = true; // finish the running crossfade first
    } else if (!banks_[activeBank_]->matches(c)) {
        startBankFade(c);
    }
}

void DeckProcessor::resetStage(int s) {
    switch (s) {
    case sLeveller:
        levGainDb_ = 0.0;
        levGainLin_ = 1.0;
        catching_ = catchPending_ = fastDown_ = fastUp_ = fell_ = false;
        quietSteady_ = 0.0;
        break;
    case sCompressor:
        compHpf_.reset();
        compDet_ = compRed_ = 0.0;
        compMakeupS_ = cfg_.compMakeup;
        break;
    case sDensity:
        banks_[0]->reset(cfg_);
        banks_[1]->reset(cfg_);
        configureBank(*banks_[activeBank_], cfg_);
        bankFading_ = bankPending_ = false;
        break;
    case sBass:
        bassLp4_.set(cfg_.bassLp4);
        bassHp_.c = cfg_.bassHp;
        bassLp_.c = cfg_.bassLp;
        bassRetune_ = false;
        bassGainS_ = cfg_.bassGain;
        bassLp4_.reset();
        bassHp_.reset();
        bassLp_.reset();
        break;
    case sExciter:
        excHp_.reset();
        excHp2_.reset();
        excOs_.setFactor(cfg_.oversampling); // clean filters, the configuration's factor
        break;
    case sClipper:
        clipOs_.setFactor(cfg_.oversampling);
        clipBendS_ = cfg_.clipBend;
        break;
    case sDeEsser:
        dessHp_.reset();
        dessShelf_.reset();
        dessDet_ = dessRed_ = 0.0;
        dessEngaged_ = false;
        break;
    case sWidth:
        width_[0].reset();
        width_[1].reset();
        width_[widthActive_].set(cfg_);
        widthFading_ = widthPending_ = false;
        widthS_ = cfg_.width;
        bassWidthS_ = cfg_.bassWidth;
        break;
    default:
        break;
    }
}

void DeckProcessor::resetStates() {
    for (int s = 0; s < kStageCount; ++s) resetStage(s);
    limiter_.setLook(curLook_);
    std::fill(dryL_.begin(), dryL_.end(), 0.0f);
    std::fill(dryR_.begin(), dryR_.end(), 0.0f);
    dryPos_ = 0;
    kIn_.reset();
    kPost_.reset();
    kOut_.reset();
    tpOutL_.reset();
    tpOutR_.reset();
    blockIn_ = blockPost_ = 0.0;
    colour_.reset(cfg_.colour);
    ctrlPos_ = 0;
    fastIn_ = midIn_ = postFast_ = postMid_ = postGated_ = postSlow_ = 0.0;
    for (double& h : riseHist_) h = kFloorDb;
    riseIdx_ = riseBlocks_ = 0;
    for (double& h : midHist_) h = kFloorDb;
    histIdx_ = histBlocks_ = 0;
    fallTime_ = 0.0;
    pauseSeen_ = true;
    quietTime_ = 1e9;
    silenceTime_ = 1e9;
    longSilence_ = true;
    musicTime_ = 0.0;
    quiet_ = true;
    clipDriveS_ = cfg_.clipDrive;
    clipBendS_ = cfg_.clipBend;
    rec_ = MeterRecord();
    recIn_ = recOut_ = 0.0;
    recClipRatio_ = 1.0;
}

bool DeckProcessor::statesFinite() const {
    if (!std::isfinite(compDet_ + compRed_ + levGainDb_ + postSlow_ + postFast_ + fastIn_ + dessDet_ + dessRed_))
        return false;
    if (!colour_.finite()) return false;
    if (!limiter_.finite() || !kIn_.finite() || !kPost_.finite() || !kOut_.finite()) return false;
    if (!banks_[0]->finite() || !banks_[1]->finite()) return false;
    if (!bassLp4_.finite() || !bassHp_.finite() || !bassLp_.finite() || !excHp_.finite() || !excHp2_.finite() || !excOs_.finite() || !clipOs_.finite()) return false;
    if (!dessHp_.finite() || !dessShelf_.finite() || !width_[0].finite() || !width_[1].finite()) return false;
    return compHpf_.finite();
}

void DeckProcessor::controlUpdate() {
    const DeckConfig& c = cfg_;
    const double dt = static_cast<double>(kControlBlock) / fs_;

    // incoming level (K-weighted, 0.4 s and 1.5 s) -> quiet / silence / warm start
    const double inPow = blockIn_ / kControlBlock;
    fastIn_ = toward(fastIn_, inPow, c.aFast);
    midIn_ = toward(midIn_, inPow, c.aMid);
    blockIn_ = 0.0;
    const double inLufs = powerToLufs(fastIn_);
    const double levDb = levGainDb_ * mix_[sLeveller];
    quiet_ = inLufs + levDb + linToDb(inGain_) < c.holdLufs;
    if (inLufs < -60.0) {
        silenceTime_ += dt;
        if (silenceTime_ >= 1.0) pauseSeen_ = true;
        if (silenceTime_ >= 2.0) longSilence_ = true;
    } else {
        silenceTime_ = 0.0;
    }
    if (quiet_) {
        quietTime_ += dt;
    } else {
        if (quietTime_ >= 1.0 && pauseSeen_) { // music after a pause: warm start (a quiet passage is not a pause)
            musicTime_ = 0.0;
            if (longSilence_ && c.catchUp) catchPending_ = true;
        }
        pauseSeen_ = false;
        quietTime_ = 0.0;
        longSilence_ = false;
        musicTime_ += dt;
    }
    // leveller detector after the width stage, with the leveller gain of the last block taken out: the averages
    // then do not lag behind the leveller's own moves (a closed loop through the 3 s average overshoots)
    const double postPow = std::min(blockPost_ / kControlBlock / (levGainLin_ * levGainLin_), c.levDetMax);
    blockPost_ = 0.0;
    postFast_ = toward(postFast_, postPow, c.aFast);
    postMid_ = toward(postMid_, postPow, c.aMid);
    // the trend detector: 0.4 s, gated - a block more than kGateDb under the 1.5 s level (a gap between two phrases,
    // a pause) leaves it where it is, a fade's blocks pass
    if (postPow > postMid_ * kGate) postGated_ = toward(postGated_, postPow, c.aFast);
    // the gated level every 0.25 s for the last 6 s; compared as 1 s averages (the beat's ups and downs averaged
    // out) now, 2 s ago and 4 s ago: dropping (fast), sinking (slow and still going: a fade), steady
    if (++histBlocks_ >= static_cast<int>(0.25 / dt)) {
        histBlocks_ = 0;
        histIdx_ = (histIdx_ + 1) % kLevHist;
        midHist_[histIdx_] = powerToLufs(postGated_);
    }
    auto avg = [this](int ago) { // the four values from `ago` quarter seconds back
        double s = 0;
        for (int k = 0; k < 4; ++k) s += midHist_[(histIdx_ + 2 * kLevHist - ago - k) % kLevHist];
        return s / 4.0;
    };
    const double aNow = avg(0), a2 = avg(8), a4 = avg(16);
    const bool dropping = aNow < a2 - kDropDb;
    const bool sinking = aNow < a2 - kSinkDb && a2 < a4 - kSinkDb; // in each of the last two 2 s
    if (dropping || sinking) fallTime_ = kFallKeepSeconds; // a fall counts for at least this long (no creeping up
    else fallTime_ = std::max(0.0, fallTime_ - dt);         // between two detections of a slow fade)
    const bool falling = fallTime_ > 0.0;
    const bool steady = !falling && aNow >= a4 - kSteadyDb;
    if (falling) fell_ = true;
    // a quiet section that stays is music to level; the hold keeps fades, pauses and a breakdown's first seconds
    const double midOut = powerToLufs(midIn_) + levDb + linToDb(inGain_);
    const bool wasReleased = quietSteady_ >= kQuietMusicSeconds;
    if (midOut < c.holdLufs && midOut > c.holdLufs - kQuietMusicLu && steady && inLufs >= -60.0)
        quietSteady_ += dt;
    else
        quietSteady_ = 0.0;
    const bool released = quietSteady_ >= kQuietMusicSeconds;
    if (released && !wasReleased) fell_ = true; // the 3 s level starts from the quiet section's level
    const bool hold = quiet_ && !released;
    if (quiet_ && released) musicTime_ += dt; // a released quiet section is music: a warm start ends in it too
    const bool warm = musicTime_ < 1.5;
    if (!hold) postSlow_ = warm ? postFast_ : toward(postSlow_, postPow, c.aSlow);
    // colour: the auto tone holds while the music is quiet or falling (a breakdown, a fade) and learns fast in the
    // first seconds of a song
    colour_.control(c.colour, quiet_ || postFast_ < postSlow_ * kFallHold, musicTime_ < 6.0);
    // the 0.4 s level every 0.25 s for the last second: is the music still rising (fading in)?
    if (++riseBlocks_ >= static_cast<int>(0.25 / dt)) {
        riseBlocks_ = 0;
        riseIdx_ = (riseIdx_ + 1) & 3;
        riseHist_[riseIdx_] = powerToLufs(postFast_);
    }
    const bool rising = powerToLufs(postFast_) > riseHist_[(riseIdx_ + 1) & 3] + kRiseCatchDb; // 0.75..1 s ago

    if (running_[sLeveller]) {
        if (!hold && !warm && !c.freeze) {
            if (catchPending_) {
                catching_ = true;
                catchPending_ = false;
                catchTime_ = 0.0;
            }
            // falling level (fade-out, the start of a quiet passage): never turn up; when it has stopped falling the
            // 3 s level starts from the level reached, not from its lag
            if (!falling && fell_) {
                fell_ = false;
                if (!catching_) postSlow_ = postMid_;
            }
            // catching up follows the 0.4 s level: a track that is still fading in is followed up its fade
            double wanted = clampd(c.aim - powerToLufs(catching_ ? postFast_ : postSlow_), -c.levRange, c.levRange);
            if (falling) {
                wanted = std::min(wanted, levGainDb_);
                fastUp_ = false;
            }
            // and not while the 0.4 s level is far under the 3 s level: the first moment of a fade, before the 1.5 s
            // level shows it (a pause of the step only; after a drop the 3 s level restarts, so this ends with it)
            if (!fastUp_ && postFast_ < postSlow_ * kFallHold) wanted = std::min(wanted, levGainDb_);
            // clearly too loud (judged on the 3 s level, so a chorus does not count): down at the fast speed, following
            // the 0.4 s level (the 3 s average lags a loud entry) and only down, until the gain is there
            if (!catching_ && levGainDb_ - wanted > kFastDownDb) fastDown_ = true;
            if (fastDown_) {
                wanted = std::min(levGainDb_, clampd(c.aim - powerToLufs(postFast_), -c.levRange, c.levRange));
                if (levGainDb_ - wanted < kFastDownDoneDb) {
                    fastDown_ = false;
                    postSlow_ = postFast_; // the slow level starts from the level reached, not from its lag
                }
            }
            // clearly too soft (judged on the 3 s level, not falling): up at the fast-up speed until nearly there
            if (!catching_ && !fastDown_ && !falling && wanted - levGainDb_ > kFastUpDb) fastUp_ = true;
            if (fastUp_ && (fastDown_ || wanted - levGainDb_ < kFastUpDoneDb)) fastUp_ = false;
            const double step = catching_ ? c.catchStep : fastDown_ ? c.levFastStep : fastUp_ ? c.levFastUpStep : c.levStep;
            levGainDb_ = moveToward(levGainDb_, wanted, step);
            if (catching_) {
                catchTime_ += dt;
                if ((!rising && std::fabs(levGainDb_ - wanted) < 0.1) || catchTime_ > kCatchMaxSeconds) {
                    catching_ = false;
                    postSlow_ = postFast_; // the slow level starts from the level caught up to, not from its lag
                }
            }
        }
        levGainLin_ = dbToLin(levGainDb_ * mix_[sLeveller]);
    } else {
        levGainLin_ = 1.0;
    }

    // density
    if (running_[sDensity]) {
        DensityBank& a = *banks_[activeBank_];
        if (a.learnTime > musicTime_) a.learnTime = musicTime_;
        a.control(c, quiet_, dt);
        if (bankFading_) banks_[1 - activeBank_]->control(c, quiet_, dt);
    }

    // de-esser: reduction as a high shelf, recomputed every control block; transparent at 0 dB
    if (running_[sDeEsser]) {
        const double over = powerToLufs(dessDet_) - c.dessThr;
        const double target = clampd(over * 0.75, 0.0, c.dessMax); // 4:1
        dessRed_ = target > dessRed_ ? toward(dessRed_, target, c.dessAtt) : toward(dessRed_, target, c.dessRel);
        if (dessRed_ > 0.01) {
            if (!dessEngaged_) {
                dessShelf_.reset();
                dessEngaged_ = true;
            }
            dessShelf_.set(design::highShelf(fs_, c.dessFreq, -dessRed_));
        } else {
            dessEngaged_ = false;
        }
        if (dessRed_ > rec_.deEsserDb) rec_.deEsserDb = static_cast<float>(dessRed_);
    }
}

void DeckProcessor::process(float* left, float* right, int frames) {
    DenormalGuard denormals;
    if (slot_.take(incoming_)) applyConfig(incoming_);
    if (frames > 0) fresh_ = false;
    if (!fadingOut_ && fadeOutReq_.load(std::memory_order_acquire)) fadingOut_ = true; // the stream will stop
    processed_.fetch_add(static_cast<uint64_t>(std::max(0, frames)), std::memory_order_relaxed);
    const double fadeOutStep = 1000.0 / (kFadeOutMs * fs_);
    const DeckConfig& c = cfg_;
    const double fadeStep = 1.0 / (0.005 * fs_);
    const double volOut = clampd(volOut_.load(std::memory_order_relaxed), 0.0, 1.0);
    const double volIn = clampd(volIn_.load(std::memory_order_relaxed), 0.0, kMaxInputVolume);
    // one-pole glide that lands exactly on the target
    auto glide = [&](double now, double target) {
        const double next = toward(now, target, volGlide_);
        return std::fabs(next - target) < 1e-7 * std::max(1.0, target) ? target : next;
    };

    for (int i = 0; i < frames; ++i) {
        if (volOutS_ != volOut || volInS_ != volIn) {
            volOutS_ = glide(volOutS_, volOut);
            volInS_ = glide(volInS_, volIn);
        }
        // non-finite or absurd input counts as 0; then the input factor of setVolume
        double xl = left[i], xr = right[i];
        if (!(std::fabs(xl) <= 1e6)) xl = 0.0;
        if (!(std::fabs(xr) <= 1e6)) xr = 0.0;
        if (volInS_ != 1.0) {
            xl *= volInS_;
            xr *= volInS_;
        }

        // the signal-chain bars: the level at each point of the chain (MeterRecord::chain)
        auto tap = [this](int k, double a, double b) { recChain_[k] += a * a + b * b; };
        tap(ctInput, xl, xr);

        // dry path for the bypass: the input delayed by the look-ahead, bit for bit
        float dl = static_cast<float>(xl), dr = static_cast<float>(xr);
        if (curLook_ > 0) {
            const float tl = dryL_[static_cast<size_t>(dryPos_)], tr = dryR_[static_cast<size_t>(dryPos_)];
            dryL_[static_cast<size_t>(dryPos_)] = dl;
            dryR_[static_cast<size_t>(dryPos_)] = dr;
            dryPos_ = dryPos_ + 1 >= curLook_ ? 0 : dryPos_ + 1;
            dl = tl;
            dr = tr;
        }

        // input measurements
        {
            const Pair k = kIn_.process(makePair(xl, xr));
            const double a = leftOf(k), b = rightOf(k);
            const double e = a * a + b * b;
            blockIn_ += e;
            recIn_ += e;
            rec_.peakIn[0] = std::max(rec_.peakIn[0], static_cast<float>(std::fabs(xl)));
            rec_.peakIn[1] = std::max(rec_.peakIn[1], static_cast<float>(std::fabs(xr)));
        }

        // switches (20 ms ramps); a stage that starts from rest starts with a clean state. Skipped once everything
        // has arrived (settled_), until the next configuration.
        if (!settled_) {
            bool done = !bassRetune_ && !osRetune_;
            for (int s = 0; s < kStageCount; ++s) {
                const bool want = c.on[s] && !(s == sBass && bassRetune_) &&
                                  !((s == sExciter || s == sClipper) && osRetune_);
                if (want && !running_[s]) {
                    resetStage(s);
                    running_[s] = true;
                }
                if (mix_[s] != (want ? 1.0 : 0.0)) mix_[s] = moveToward(mix_[s], want ? 1.0 : 0.0, c.switchStep);
                if (!want && mix_[s] <= 0.0 && s != sLeveller && running_[s]) {
                    running_[s] = false;
                    if (s == sBass) bassRetune_ = false; // back in with the new filters
                }
                if (osRetune_ && !running_[sExciter] && !running_[sClipper]) osRetune_ = false; // back in: new factor
                done = done && mix_[s] == (want ? 1.0 : 0.0) && (running_[s] == want || s == sLeveller);
            }
            if (!c.on[sLeveller] && mix_[sLeveller] <= 0.0 && running_[sLeveller]) {
                running_[sLeveller] = false;
                resetStage(sLeveller);
            }
            masterMix_ = moveToward(masterMix_, c.processing ? 1.0 : 0.0, c.switchStep);
            inGain_ = toward(inGain_, c.inputGain, c.gainSmooth);
            outGain_ = toward(outGain_, c.outputGain, c.gainSmooth);
            settled_ = done && running_[sLeveller] == c.on[sLeveller] && masterMix_ == (c.processing ? 1.0 : 0.0) &&
                       inGain_ == c.inputGain && outGain_ == c.outputGain;
        }

        if (ctrlPos_ == 0) controlUpdate();
        const double frac = static_cast<double>(ctrlPos_ + 1) / kControlBlock;
        ctrlPos_ = ctrlPos_ + 1 == kControlBlock ? 0 : ctrlPos_ + 1;

        // leveller gain + input gain
        const double g = levGainLin_ * inGain_;
        double l = xl * g, r = xr * g;
        tap(ctLeveller, l, r);

        // colour: the auto tone measures the deck's input; sub bass, tone, loudness
        colour_.measure(0.5 * (xl + xr));
        colour_.process(c.colour, l, r);
        tap(ctColour, l, r);

        // compressor
        if (running_[sCompressor]) {
            double sl = l, sr = r;
            if (c.compHpfOn) {
                const Pair s = compHpf_.process(makePair(l, r));
                sl = leftOf(s);
                sr = rightOf(s);
            }
            compDet_ = toward(compDet_, sl * sl + sr * sr, c.compDet);
            const double lvl = fastPowerToLufs(compDet_);
            const double red = lvl - compressorCurveDb(lvl, c.compThr, c.compRatio, c.compKnee);
            compRed_ = red > compRed_ ? toward(compRed_, red, c.compAtt) : toward(compRed_, red, c.compRel);
            compMakeupS_ = toward(compMakeupS_, c.compMakeup, c.gainSmooth);
            const double gc = fastDbToLin(compMakeupS_ - compRed_);
            const double ge = 1.0 + mix_[sCompressor] * (gc - 1.0);
            l *= ge;
            r *= ge;
            if (compRed_ > rec_.compressorDb) rec_.compressorDb = static_cast<float>(compRed_);
        }
        tap(ctCompressor, l, r);

        // density
        if (running_[sDensity]) {
            double yl, yr;
            banks_[activeBank_]->run(l, r, frac, yl, yr);
            if (bankFading_) {
                double zl, zr;
                banks_[1 - activeBank_]->run(l, r, frac, zl, zr);
                bankXf_ = std::min(1.0, bankXf_ + c.switchStep);
                const double xf = crossfadeWeight(bankXf_);
                yl += xf * (zl - yl);
                yr += xf * (zr - yr);
                if (bankXf_ >= 1.0) {
                    activeBank_ = 1 - activeBank_;
                    bankFading_ = false;
                    if (bankPending_) {
                        bankPending_ = false;
                        if (!banks_[activeBank_]->matches(c)) startBankFade(c);
                    }
                }
            }
            const double m = mix_[sDensity];
            l += m * (yl - l);
            r += m * (yr - r);
        }
        tap(ctDensity, l, r);

        // bass enhancer
        if (running_[sBass]) {
            const double low = bassLp4_.process(0.5 * (l + r));
            bassGainS_ = toward(bassGainS_, c.bassGain, c.gainSmooth);
            const double add = mix_[sBass] * bassGainS_ * bassLp_.process(bassHp_.process(std::fabs(low)));
            l += add;
            r += add;
        }
        tap(ctBass, l, r);

        // exciter: only what the saturation adds, high-passed again; the saturation at 2x, 4x or 8x the rate, so its
        // overtones above the Nyquist frequency do not fold back (dsp.h, StereoOversampler)
        if (running_[sExciter]) {
            const Pair h = excHp_.process(makePair(l, r));
            Pair os[StereoOversampler::kMaxFactor];
            excOs_.up(h, os);
            for (int k = 0; k < excOs_.factor(); ++k) {
                const double x = leftOf(os[k]), y = rightOf(os[k]);
                os[k] = makePair(x - fastTanh(4.0 * x) / 4.0, y - fastTanh(4.0 * y) / 4.0);
            }
            const Pair e = excHp2_.process(excOs_.down(os));
            const double el = leftOf(e), er = rightOf(e);
            const double m = mix_[sExciter] * c.excGain;
            l += m * el;
            r += m * er;
        }
        tap(ctExciter, l, r);

        // de-esser
        if (running_[sDeEsser]) {
            const Pair h = dessHp_.process(makePair(l, r));
            const double hl = leftOf(h), hr = rightOf(h);
            dessDet_ = toward(dessDet_, hl * hl + hr * hr, c.dessDet);
            if (dessEngaged_) {
                const double m = mix_[sDeEsser];
                const Pair s = dessShelf_.process(makePair(l, r));
                l += m * (leftOf(s) - l);
                r += m * (rightOf(s) - r);
            }
        }
        tap(ctDeEsser, l, r);

        // stereo width (mid / side, in-phase LR4 split of the side, mid through the same all-pass)
        if (running_[sWidth]) {
            widthS_ = toward(widthS_, c.width, c.gainSmooth);
            bassWidthS_ = toward(bassWidthS_, c.bassWidth, c.gainSmooth);
            const double m0 = 0.5 * (l + r), side = 0.5 * (l - r);
            WidthFilters& wa = width_[widthActive_];
            double mid = wa.ap.process(m0);
            double s2 = bassWidthS_ * wa.lp.process(side) + widthS_ * wa.hp.process(side);
            if (widthFading_) {
                WidthFilters& wb = width_[1 - widthActive_];
                const double midB = wb.ap.process(m0);
                const double s2B = bassWidthS_ * wb.lp.process(side) + widthS_ * wb.hp.process(side);
                widthXf_ = std::min(1.0, widthXf_ + c.switchStep);
                const double xf = crossfadeWeight(widthXf_);
                mid += xf * (midB - mid);
                s2 += xf * (s2B - s2);
                if (widthXf_ >= 1.0) {
                    widthActive_ = 1 - widthActive_;
                    widthFading_ = false;
                    if (widthPending_) {
                        widthPending_ = false;
                        if (!sameCoeffs(width_[widthActive_].split, c.widthLp)) startWidthFade();
                    }
                }
            }
            const double m = mix_[sWidth];
            const double wl = mid + s2, wr = mid - s2;
            l += m * (wl - l);
            r += m * (wr - r);
        }
        tap(ctWidth, l, r);

        // leveller detector: after colour, compressor, density, the colour stages and width (handbook 9.2)
        {
            const Pair k = kPost_.process(makePair(l, r));
            const double a = leftOf(k), b = rightOf(k);
            blockPost_ += a * a + b * b;
        }

        // soft clipper at 2, 4 or 8 times the rate (no fold-back of its overtones; about 2.6 / 4.0 / 4.75 frames of
        // phase delay at low frequencies - an IIR filter's phase, like the crossovers', no look-ahead)
        if (running_[sClipper]) {
            clipDriveS_ = toward(clipDriveS_, c.clipDrive, c.gainSmooth);
            clipBendS_ = toward(clipBendS_, c.clipBend, c.gainSmooth);
            // `bend`: the curve's own ceiling - the configured one, or under it with the limiter off (see below)
            const double ceil = c.clipCeiling, bend = ceil * clipBendS_, d = clipDriveS_ / bend, inv = 1.0 / bend;
            const double ul = l * d, ur = r * d;
            // the filters see the driven signal only; the ceiling enters per sample in the curve itself, so a new
            // ceiling does not meet samples of the old scale in the filters' memory (a click)
            Pair os[StereoOversampler::kMaxFactor];
            clipOs_.up(makePair(l * clipDriveS_, r * clipDriveS_), os);
            for (int k = 0; k < clipOs_.factor(); ++k)
                os[k] = makePair(bend * softClip(leftOf(os[k]) * inv), bend * softClip(rightOf(os[k]) * inv));
            // a flattened peak, its overtones above the audio band removed, rises a little over the ceiling again
            // (0.36 dB on a 3 kHz tone 6 dB in): the limiter after it holds the ceiling. Holding it here as well
            // brought the fold-back of the highs back (-24 dBc on 13 kHz), so only a loudness preset with the
            // limiter off gets that hold (outside review: the clipper's ceiling must stand on its own then) - and
            // its curve bends 1.5 dB under the ceiling, so that the hold seldom cuts a peak: the fold-back of a tone
            // 6 dB in went from -25 to about -60 dBc (fourth outside review, measured; 1 dB under gave -36 dBc)
            const Pair y = clipOs_.down(os);
            double yl = leftOf(y), yr = rightOf(y);
            if (!running_[sLimiter]) {
                yl = std::max(-ceil, std::min(ceil, yl));
                yr = std::max(-ceil, std::min(ceil, yr));
            }
            const double m = mix_[sClipper];
            const double au = std::max(std::fabs(ul), std::fabs(ur));
            if (au > 0.5) {
                const double ay = std::max(std::fabs(softClip(ul)), std::fabs(softClip(ur)));
                if (au / ay > recClipRatio_) recClipRatio_ = au / ay;
            }
            l += m * (yl - l);
            r += m * (yr - r);
        }
        tap(ctClipper, l, r);

        // look-ahead true-peak limiter: delays by curLook_ frames
        limiter_.process(l, r);
        tap(ctLimiter, l, r);

        // output gain (only down), bypass crossfade, look-ahead reconfiguration fade, clip guard
        l *= outGain_;
        r *= outGain_;
        float ol, orr;
        if (masterMix_ >= 1.0) {
            ol = static_cast<float>(l);
            orr = static_cast<float>(r);
        } else if (masterMix_ <= 0.0) {
            ol = dl;
            orr = dr;
        } else {
            ol = static_cast<float>(dl + masterMix_ * (l - dl));
            orr = static_cast<float>(dr + masterMix_ * (r - dr));
        }
        if (lookFade_ < 1.0) {
            ol = static_cast<float>(ol * lookFade_);
            orr = static_cast<float>(orr * lookFade_);
        }
        if (masterMix_ > 0.0) { // never acts on a valid signal; a full bypass stays bit-exact
            ol = std::isfinite(ol) ? std::min(1.0f, std::max(-1.0f, ol)) : 0.0f;
            orr = std::isfinite(orr) ? std::min(1.0f, std::max(-1.0f, orr)) : 0.0f;
        }
        if (fadingOut_) { // before a stop: down to silence, then silent until the next prepare()
            const double fg = fadeOutPos_ >= 1.0 ? 0.0 : 0.5 + 0.5 * std::cos(kPi * fadeOutPos_);
            ol = static_cast<float>(ol * fg);
            orr = static_cast<float>(orr * fg);
            if (fadeOutPos_ < 1.0) fadeOutPos_ = std::min(1.0, fadeOutPos_ + fadeOutStep);
            else if (!fadedOut_.load(std::memory_order_relaxed)) fadedOut_.store(true, std::memory_order_release);
        }
        tap(ctOutput, ol, orr);
        // the volume of setVolume, after everything (only down: the ceiling holds); taps and meters before it
        left[i] = volOutS_ == 1.0 ? ol : static_cast<float>(ol * volOutS_);
        right[i] = volOutS_ == 1.0 ? orr : static_cast<float>(orr * volOutS_);
        taps_->push({static_cast<float>(xl), static_cast<float>(xr), ol, orr});

        // output measurements
        {
            const Pair k = kOut_.process(makePair(ol, orr));
            const double a = leftOf(k), b = rightOf(k);
            recOut_ += a * a + b * b;
            float pl, pr;
            TruePeakDetector::pushPair(tpOutL_, tpOutR_, ol, orr, pl, pr);
            rec_.tpOut[0] = std::max(rec_.tpOut[0], pl);
            rec_.tpOut[1] = std::max(rec_.tpOut[1], pr);
        }
        if (++rec_.frames >= recordFrames_) {
            rec_.energyIn = static_cast<float>(recIn_);
            rec_.energyOut = static_cast<float>(recOut_);
            for (int k = 0; k < kChainTaps; ++k) rec_.chain[k] = static_cast<float>(recChain_[k]);
            for (int s = 0; s < kStageCount; ++s)
                if (running_[s] && mix_[s] > 0.0) rec_.active = static_cast<uint16_t>(rec_.active | 1u << s);
            if (c.colour.any()) rec_.active = static_cast<uint16_t>(rec_.active | 1u << kChainColourBit);
            rec_.levellerDb = static_cast<float>(levGainDb_ * mix_[sLeveller]);
            rec_.compressorIn = static_cast<float>(running_[sCompressor] ? fastPowerToLufs(compDet_) : kFloorDb);
            const DensityBank& bank = *banks_[activeBank_];
            rec_.bands = static_cast<uint8_t>(running_[sDensity] ? bank.bands : 0);
            for (int b = 0; b < kMaxBands; ++b)
                rec_.densityDb[b] = running_[sDensity] && b < bank.bands ? static_cast<float>(bank.gainDb[b]) : 0.0f;
            for (int s = 0; s < 4; ++s) rec_.autoToneDb[s] = static_cast<float>(colour_.correctionDb(s));
            rec_.clipperDb = static_cast<float>(linToDb(recClipRatio_));
            rec_.limiterDb = static_cast<float>(-linToDb(limiter_.takeMinGain()));
            rec_.flags = static_cast<uint8_t>((quiet_ ? mfQuiet : 0) | (c.freeze ? mfFrozen : 0) |
                                              (musicTime_ < 1.5 ? mfWarm : 0) | (c.processing ? 0 : mfBypassed) |
                                              (lookState_ != 0 ? mfReconfiguring : 0));
            meters_->push(rec_);
            rec_ = MeterRecord();
            recIn_ = recOut_ = 0.0;
            std::fill(std::begin(recChain_), std::end(recChain_), 0.0);
            recClipRatio_ = 1.0;
        }

        // look-ahead change: fade out 5 ms, reconfigure, refill the delay, fade in 5 ms (handbook 9.4)
        switch (lookState_) {
        case 0:
            if (wantLook_ != curLook_) lookState_ = 1;
            break;
        case 1:
            if (wantLook_ == curLook_) {
                lookState_ = 3;
                break;
            }
            lookFade_ -= fadeStep;
            if (lookFade_ <= 0.0) {
                lookFade_ = 0.0;
                curLook_ = wantLook_;
                limiter_.setLook(curLook_);
                std::fill(dryL_.begin(), dryL_.end(), 0.0f);
                std::fill(dryR_.begin(), dryR_.end(), 0.0f);
                dryPos_ = 0;
                refill_ = curLook_;
                lookState_ = 2;
            }
            break;
        case 2:
            if (wantLook_ != curLook_)
                lookState_ = 1;
            else if (refill_-- <= 0)
                lookState_ = 3;
            break;
        default:
            if (wantLook_ != curLook_) {
                lookState_ = 1;
                break;
            }
            lookFade_ += fadeStep;
            if (lookFade_ >= 1.0) {
                lookFade_ = 1.0;
                lookState_ = 0;
            }
            break;
        }
    }

    // a non-finite state (one NaN from a broken source) would silence the output for good: reset
    if (!statesFinite()) {
        ++resets_;
        resetStates();
    }
}

} // namespace ap
