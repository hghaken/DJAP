#include "asyncinput.h"

#include <algorithm>
#include <cmath>

namespace ap {

namespace {

constexpr int kTaps = 2 * AsyncInput::kHalfTaps;
constexpr int64_t kMask = AsyncInput::kCapacity - 1;
constexpr double kAvgTau = 0.5; // s, smoothing of the measured fill (timing jitter of both callbacks)
constexpr double kTp = 4.0;     // s, a fill error is corrected over this time ...
constexpr double kTi = 16.0;    // s, ... and the drift estimate follows it over this (4 x kTp: critically damped)
constexpr double kKaiserBeta = 8.0;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Windowed sinc (cut-off at the Nyquist frequency, Kaiser window over +-kHalfTaps) for the fractional positions
// p / kPhases, p = 0..kPhases. Tap k weighs frame pos - (kHalfTaps - 1) + k. Position 0 is exactly a unit impulse
// (the sinc is zero at every other whole frame), so a read speed of 1 is bit-exact.
struct SincTable {
    float c[AsyncInput::kPhases + 1][kTaps];
    SincTable() {
        const double pi = 3.14159265358979323846;
        const double norm = besselI0(kKaiserBeta);
        for (int p = 0; p <= AsyncInput::kPhases; ++p) {
            const double frac = static_cast<double>(p) / AsyncInput::kPhases;
            double h[kTaps], sum = 0.0;
            for (int k = 0; k < kTaps; ++k) {
                const double x = k - (AsyncInput::kHalfTaps - 1) - frac;
                const double r = x / AsyncInput::kHalfTaps;
                const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / norm;
                const double s = x == 0.0 ? 1.0 : (x == std::floor(x) ? 0.0 : std::sin(pi * x) / (pi * x));
                h[k] = w * s;
                sum += h[k];
            }
            for (int k = 0; k < kTaps; ++k) c[p][k] = static_cast<float>(h[k] / sum); // gain 1 at DC
        }
    }
};

const SincTable& table() {
    static const SincTable t;
    return t;
}

template <class T>
void bump(std::atomic<T>& a) {
    a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // one writer
}

} // namespace

AsyncInput::AsyncInput() : ring_(new float[2 * (kCapacity + kTaps)]()) {
    table(); // built here, not on an audio thread
}

void AsyncInput::reset(double fs) {
    fs_ = fs > 0 ? fs : 48000.0;
    std::fill(ring_.get(), ring_.get() + 2 * (kCapacity + kTaps), 0.0f);
    written_.store(0, std::memory_order_relaxed);
    writeTime_.store(0.0, std::memory_order_relaxed);
    maxBlock_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    writes_.store(0, std::memory_order_relaxed);
    writeBursts_ = Bursts();
    playBursts_ = Bursts();
    fadeOut_ = fadeRing_ = 0;
    lastL_ = lastR_ = 0.0f;
    targetS_ = 0.0;
    devicePeriod_ = 0;
    oldestNeeded_.store(0, std::memory_order_relaxed);
    pos_ = 0;
    frac_ = 0.0;
    ratio_ = 1.0;
    avgFill_ = integ_ = 0.0;
    marginMs_ = kMarginMs;
    marginFloorMs_ = 0.0;
    decayed_ = false;
    calmS_ = 0.0;
    period_ = 0;
    fade_ = 1.0;
    active_ = periodOk_ = haveAvg_ = false;
    activeShown_.store(false, std::memory_order_relaxed);
    targetShown_.store(0, std::memory_order_relaxed);
    fillShown_.store(0, std::memory_order_relaxed);
    driftShown_.store(0, std::memory_order_relaxed);
    corrShown_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    resyncs_.store(0, std::memory_order_release);
}

// A device period cut into short callbacks (a 10 ms packet in 128-frame callbacks, 1024 frames in two of 512)
// arrives at once: counting only the callback's size gave 5-6 underruns after each start (fourth outside review).
// The largest burst decays (two windows) and is capped: a stall's catch-up raised the buffer for good (fifth review).
// Sixth review: half, not a quarter, of the last piece's length - pieces 2.7-4 ms apart (a 10 ms packet) were not
// merged and underran.
int AsyncInput::Bursts::add(int frames, double time, double fs, int cap, bool merge) {
    burst = merge && lastTime >= 0.0 && time - lastTime < kBurstGap * lastFrames / fs ? burst + frames : frames;
    lastTime = time;
    lastFrames = frames;
    if (windowStart < 0.0 || time - windowStart >= kBurstWindowS) {
        before = windowStart < 0.0 ? 0 : now;
        now = 0;
        windowStart = time;
    }
    now = std::max(now, std::min(burst, cap));
    return std::max(now, before);
}

int AsyncInput::burstCap(int frames) const {
    return std::max({frames, static_cast<int>(kBurstCapMs * fs_ / 1000.0), devicePeriod_ + devicePeriod_ / 2});
}

void AsyncInput::write(const float* in, int frames, double now) {
    if (frames <= 0) return;
    bump(writes_);
    const int64_t w = written_.load(std::memory_order_relaxed);
    const int64_t space = kCapacity - (w - oldestNeeded_.load(std::memory_order_acquire));
    if (frames > space) { // the reader is not reading (stopped, or far behind): drop the block
        bump(overruns_);
        writeTime_.store(now, std::memory_order_relaxed); // the next block follows `written_`, a block from now
        return;
    }
    maxBlock_.store(writeBursts_.add(frames, now, fs_, burstCap(frames)), std::memory_order_relaxed); // (a dropped block does not count)
    float* ring = ring_.get();
    for (int i = 0; i < frames; ++i) {
        const int64_t idx = (w + i) & kMask;
        ring[2 * idx] = in[2 * i];
        ring[2 * idx + 1] = in[2 * i + 1];
        if (idx < kTaps) { // mirror after the end: a tap window never wraps
            ring[2 * (kCapacity + idx)] = in[2 * i];
            ring[2 * (kCapacity + idx) + 1] = in[2 * i + 1];
        }
    }
    writeTime_.store(now, std::memory_order_relaxed);
    written_.store(w + frames, std::memory_order_release);
}

void AsyncInput::beginPeriod(int frames, double now) {
    // the output side's bursts as the input's: an output period cut into short callbacks (fifth outside review)
    const int64_t w = written_.load(std::memory_order_acquire);
    period_ = playBursts_.add(frames, now, fs_, burstCap(frames), w > 0); // (no merge while priming: no input yet)
    const double written = static_cast<double>(w);
    const int block = maxBlock_.load(std::memory_order_relaxed);
    // the input's position now: the last block's end, moved on by the time since it came (at most one block), so
    // the fill does not jump with each block
    const double est = written + std::clamp((now - writeTime_.load(std::memory_order_relaxed)) * fs_, 0.0,
                                            static_cast<double>(block));
    const double wanted = block + period_ + kHalfTaps + 1 + marginMs_ * fs_ / 1000.0;
    targetS_ = wanted >= targetS_ ? wanted : std::max(wanted, targetS_ - kTargetFallMsPerS * frames / 1000.0);
    const double target = targetS_;
    periodOk_ = false;
    if (!active_) {
        if (block > 0 && written - static_cast<double>(pos_) >= target) { // filled up: start at the target
            pos_ = std::max(pos_, static_cast<int64_t>(std::floor(est - target)));
            frac_ = 0.0;
            active_ = true;
            haveAvg_ = false;
            fade_ = 0.0;
        }
    } else if (est - (pos_ + frac_) > 2.0 * target + 0.05 * fs_) { // far too full (blocks dropped, a stall)
        pos_ = static_cast<int64_t>(std::floor(est - target));
        frac_ = 0.0;
        haveAvg_ = false;
        bump(resyncs_);
    }
    if (active_) {
        const double fill = est - (static_cast<double>(pos_) + frac_);
        if (!haveAvg_) {
            avgFill_ = fill;
            haveAvg_ = true;
        } else {
            avgFill_ += std::min(1.0, frames / (fs_ * kAvgTau)) * (fill - avgFill_);
        }
        if (fixedRatio_ > 0.0) {
            ratio_ = fixedRatio_;
        } else {
            const double p = (avgFill_ - target) / (fs_ * kTp);
            // the drift estimate learns only while the read speed is inside its limits (anti-windup): a fill far
            // off the target - the buffer coming down after a burst is forgotten - drove it to the limit, and when
            // the fill was back it ran on past the target into an underrun (fifth outside review's stall test)
            if (std::fabs(p + integ_) < kMaxCorrection)
                integ_ = std::clamp(integ_ + p * (frames / fs_) / kTi, -kMaxCorrection, kMaxCorrection);
            ratio_ = 1.0 + std::clamp(p + integ_, -kMaxCorrection, kMaxCorrection);
        }
        // the frames this period reads must be there now
        const double need = frac_ + frames * ratio_ + kHalfTaps + 1;
        if (written - static_cast<double>(pos_) < need) {
            active_ = false; // silence, fill up again with a larger margin
            // a fade to silence (fifth outside review: the music stopped with a step - a click): what is still there,
            // then the last frame held - with nearly nothing left the step came back (sixth review)
            fadeOutTotal_ = fadeOut_ = static_cast<int>(kFadeOutMs * fs_ / 1000.0);
            const double avail = (written - static_cast<double>(pos_) - frac_ - kHalfTaps - 1) / ratio_;
            fadeRing_ = static_cast<int>(std::clamp(std::floor(avail), 0.0, static_cast<double>(fadeOutTotal_)));
            // an underrun after a step down: that step was one too many - the margin never goes under the level
            // that held again (lasting jitter: a plain decay walked back into the failing margin every ~4 minutes)
            if (decayed_) marginFloorMs_ = std::min(kMarginMaxMs, std::max(marginFloorMs_, marginMs_ + kMarginDecayMs));
            decayed_ = false;
            marginMs_ = std::min(kMarginMaxMs, marginMs_ + kMarginStepMs);
            calmS_ = 0.0;
            bump(underruns_);
        } else {
            periodOk_ = true;
            calmS_ += frames / fs_;
            if (calmS_ >= kMarginCalmS) {
                calmS_ = 0.0;
                const double down = std::max(std::max(kMarginMs, marginFloorMs_), marginMs_ - kMarginDecayMs);
                if (down < marginMs_) decayed_ = true;
                marginMs_ = down;
            }
        }
    }
    activeShown_.store(active_, std::memory_order_relaxed);
    targetShown_.store(target / fs_ * 1000.0, std::memory_order_relaxed);
    fillShown_.store(active_ ? avgFill_ / fs_ * 1000.0 : 0.0, std::memory_order_relaxed);
    driftShown_.store((fixedRatio_ > 0.0 ? fixedRatio_ - 1.0 : integ_) * 1e6, std::memory_order_relaxed);
    corrShown_.store((ratio_ - 1.0) * 1e6, std::memory_order_relaxed);
}

void AsyncInput::read(float* left, float* right, int frames) {
    if (frames <= 0) return;
    if (!periodOk_) {
        int n = 0;
        if (fadeOut_ > 0) { // after an underrun, raised-cosine down to silence
            n = std::min(frames, fadeOut_);
            const int fromRing = std::min(n, fadeRing_);
            interpolate(left, right, fromRing);
            fadeRing_ -= fromRing;
            for (int i = fromRing; i < n; ++i) {
                left[i] = lastL_;
                right[i] = lastR_;
            }
            for (int i = 0; i < n; ++i) {
                const double done = static_cast<double>(fadeOutTotal_ - fadeOut_ + i + 1) / (fadeOutTotal_ + 1);
                const float g = static_cast<float>(0.5 + 0.5 * std::cos(3.14159265358979 * done));
                left[i] *= g;
                right[i] *= g;
            }
            fadeOut_ -= n;
        }
        std::fill(left + n, left + frames, 0.0f);
        std::fill(right + n, right + frames, 0.0f);
    } else {
        fadeOut_ = 0;
        interpolate(left, right, frames);
    }
    oldestNeeded_.store(pos_ - (kHalfTaps - 1), std::memory_order_release);
}

void AsyncInput::interpolate(float* left, float* right, int frames) {
    const SincTable& t = table();
    const float* ring = ring_.get();
    for (int i = 0; i < frames; ++i) {
        const double pf = frac_ * kPhases;
        const int ph = static_cast<int>(pf);
        const float a = static_cast<float>(pf - ph);
        const float* c0 = t.c[ph];
        const float* c1 = t.c[ph + 1];
        const float* x = ring + 2 * ((pos_ - (kHalfTaps - 1)) & kMask);
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < kTaps; ++k) {
            const float c = c0[k] + a * (c1[k] - c0[k]);
            l += c * x[2 * k];
            r += c * x[2 * k + 1];
        }
        if (fade_ < 1.0) { // the fade-in after (re)starting
            const float g = static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979 * fade_));
            l *= g;
            r *= g;
            fade_ = std::min(1.0, fade_ + 1000.0 / (kFadeInMs * fs_));
        }
        left[i] = l;
        right[i] = r;
        lastL_ = l;
        lastR_ = r;
        frac_ += ratio_;
        const double whole = std::floor(frac_);
        pos_ += static_cast<int64_t>(whole);
        frac_ -= whole;
    }
}

AsyncInputState AsyncInput::state() const {
    AsyncInputState s;
    s.active = activeShown_.load(std::memory_order_relaxed);
    s.targetMs = targetShown_.load(std::memory_order_relaxed);
    s.fillMs = fillShown_.load(std::memory_order_relaxed);
    s.driftPpm = driftShown_.load(std::memory_order_relaxed);
    s.correctionPpm = corrShown_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.resyncs = resyncs_.load(std::memory_order_relaxed);
    s.writes = writes_.load(std::memory_order_relaxed);
    return s;
}

} // namespace ap
