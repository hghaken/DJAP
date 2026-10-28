#include "asyncinput.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    steadyBlock_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    writes_.store(0, std::memory_order_relaxed);
    repeats_.store(0, std::memory_order_relaxed);
    recentRepeats_.store(0, std::memory_order_relaxed);
    lastWriteFrames_ = repeatRun_ = 0;
    repeatsNow_ = repeatsBefore_ = 0;
    repeatWindow_ = -1.0;
    writeBursts_ = Bursts();
    playBursts_ = Bursts();
    fadeOut_ = fadeRing_ = 0;
    lastL_ = lastR_ = 0.0f;
    targetS_ = 0.0;
    devicePeriod_.store(0, std::memory_order_relaxed);
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
    playGaps_ = 0;
    playStart_ = -1.0;
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
int AsyncInput::Bursts::add(int frames, double time, double fs, int cap, bool merge, bool count) {
    const bool merged = merge && lastTime >= 0.0 && time - lastTime < kBurstGap * lastFrames / fs;
    const int finished = !merged && lastTime >= 0.0 ? burst : 0; // the burst this block ends
    burst = merged ? burst + frames : frames;
    lastTime = time;
    lastFrames = frames;
    if (!count) return std::max(now, before); // seen (for the gaps), not remembered
    if (windowStart < 0.0 || time - windowStart >= kBurstWindowS) {
        before = windowStart < 0.0 ? 0 : now;
        histBefore = windowStart < 0.0 ? Histogram() : histNow;
        now = 0;
        histNow = Histogram();
        windowStart = time;
    }
    if (finished > 0) {
        const int bin = std::min(kBins - 1, static_cast<int>(finished * 4000.0 / fs));
        ++histNow.count[bin];
        histNow.sum[bin] += finished;
    }
    now = std::max(now, std::min(burst, cap));
    return std::max(now, before);
}

int AsyncInput::Bursts::steady() const {
    long long n = 0;
    for (int b = 0; b < kBins; ++b) n += histNow.count[b] + histBefore.count[b];
    if (n == 0) return 0;
    long long seen = 0;
    for (int b = 0; b < kBins; ++b) {
        const int c = histNow.count[b] + histBefore.count[b];
        seen += c;
        if (2 * seen >= n) // the median's bin: the mean burst in it
            return static_cast<int>((histNow.sum[b] + histBefore.sum[b] + c / 2) / c);
    }
    return 0;
}

int AsyncInput::burstCap(int frames) const {
    const int dp = devicePeriod_.load(std::memory_order_relaxed);
    return std::max({frames, static_cast<int>(kBurstCapMs * fs_ / 1000.0), dp + dp / 2});
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
    steadyBlock_.store(writeBursts_.steady(), std::memory_order_relaxed);
    float* ring = ring_.get();
    // the same block again, bit for bit: the device was read faster than it was filled (live, 2026-10-28: a virtual
    // cable at a 1 ms period with its own timer at 3 ms; ninth outside review: count it, so Health can say so). Tenth
    // review: a periodic signal (a 1 kHz test tone in 480-frame blocks, DC) repeats block for block and a clipped
    // plateau is flat - neither is a fault: a flat block never counts, and a run of equal blocks counts when a new
    // block ends it after at most kRepeatRunMax repeats (a device read 2-4 times per fill); a longer run is a period
    bool same = false, flat = true;
    if (frames >= 16 && frames == lastWriteFrames_ && w >= frames) { // (else: another length, or too short to tell)
        same = true;
        for (int i = 0; i < frames && same; ++i) {
            const int64_t idx = (w - frames + i) & kMask;
            same = ring[2 * idx] == in[2 * i] && ring[2 * idx + 1] == in[2 * i + 1];
            flat = flat && in[2 * i] == in[0] && in[2 * i + 1] == in[1];
        }
    }
    if (same && !flat) {
        ++repeatRun_;
    } else {
        if (!same && repeatRun_ > 0 && repeatRun_ <= kRepeatRunMax) { // a new block ends a short run: count it
            bump(repeats_);
            ++repeatsNow_;
        }
        repeatRun_ = 0; // (a flat repeat ends a run uncounted, as it starts none)
    }
    lastWriteFrames_ = frames;
    if (repeatWindow_ < 0.0 || now - repeatWindow_ >= kBurstWindowS) { // the recent ones: two windows, as the bursts
        repeatsBefore_ = repeatWindow_ < 0.0 ? 0 : repeatsNow_;
        repeatsNow_ = 0;
        repeatWindow_ = now;
    }
    recentRepeats_.store(repeatsNow_ + repeatsBefore_, std::memory_order_relaxed);
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
    // no merge while priming: the output's callbacks merge from its first real gap on, and reading starts after the
    // second, when one whole burst has been seen (seventh outside review: "no input yet" missed a priming that came
    // after the first capture block - the target stood 20 ms higher for 20-45 s; started at once, the priming then
    // read three periods at once and underran; an output cut in two needs its whole burst counted before reading)
    // Eighth review: before the first gap the callbacks are not remembered at all - a DirectSound stream's first
    // callback is its whole buffer, and as a burst it held the target 20 ms higher for 40-65 s. An output that never
    // pauses (callbacks faster than real time for good) starts reading after kPrimeTimeoutS anyway.
    if (playStart_ < 0.0) playStart_ = now;
    if (playGaps_ < 2 && playBursts_.lastTime >= 0.0 &&
        now - playBursts_.lastTime >= kBurstGap * playBursts_.lastFrames / fs_)
        ++playGaps_;
    if (playGaps_ < 2 && now - playStart_ >= kPrimeTimeoutS) playGaps_ = 2;
    period_ = playBursts_.add(frames, now, fs_, burstCap(frames), playGaps_ >= 1, playGaps_ >= 1);
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
        if (block > 0 && playGaps_ >= 2 && written - static_cast<double>(pos_) >= target) { // filled: start at the target
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
    s.blockMs = maxBlock_.load(std::memory_order_relaxed) * 1000.0 / fs_;
    s.steadyMs = steadyBlock_.load(std::memory_order_relaxed) * 1000.0 / fs_;
    s.repeats = repeats_.load(std::memory_order_relaxed);
    s.recentRepeats = recentRepeats_.load(std::memory_order_relaxed);
    return s;
}

// Compared with the device's default period, not a fixed 9 ms (tenth outside review: a step of 144 frames made 8 ms
// 9.00 ms - taken as asked, yet reported as Windows' 10 ms blocks), and only where the low-latency periods apply
// (exclusive mode got advice about Windows' sound settings).
std::string inputNotes(const InputNoteFacts& f) {
    std::string out;
    char b[320];
    if (f.lowLatency && f.requestedMs > 0.0 && f.steadyMs > 0.0) {
        if (f.onStepMs <= 0.0) { // no period on the device's step fits: Windows' default
            if (f.rateDiffers)
                std::snprintf(b, sizeof b, "; the input runs at Windows' default period (%.1f ms) - a shorter one needs "
                              "the device set to %g kHz in Windows' sound settings", f.steadyMs, f.sampleRate / 1000.0);
            else if (f.defaultMs <= 0.0)
                std::snprintf(b, sizeof b, "; the input runs at Windows' default period (%.1f ms) - the device offers "
                              "Windows no shorter one", f.steadyMs);
            else
                std::snprintf(b, sizeof b, "; the input runs at Windows' default period (%.1f ms) - the device's step "
                              "allows no period from %.0f ms up to it", f.steadyMs, f.requestedMs);
            out += b;
        } else if (f.defaultMs > 0.0 && f.onStepMs < f.defaultMs - 0.3 && f.steadyMs >= f.defaultMs - 0.3) {
            std::snprintf(b, sizeof b, "; the input still comes in %.1f ms blocks (Windows' default) - the device did "
                          "not take the %.1f ms period", f.steadyMs, f.onStepMs);
            out += b;
        } else if (std::fabs(f.steadyMs - f.onStepMs) > 0.3) {
            std::snprintf(b, sizeof b, "; input period %.2f ms (%.2f asked)", f.steadyMs, f.onStepMs);
            out += b;
        } else if (std::fabs(f.onStepMs - f.requestedMs) > 0.05) {
            std::snprintf(b, sizeof b, "; input period %.2f ms (%.0f asked: the device's own step)", f.steadyMs,
                          f.requestedMs);
            out += b;
        }
    }
    // bursts of several blocks at once (a virtual cable's timer jitter): they, not the period, set the drift buffer
    if (f.steadyMs > 0.0 && f.blockMs > 1.5 * f.steadyMs) {
        std::snprintf(b, sizeof b, "; blocks came in bursts of up to %.1f ms lately (the drift buffer holds them)",
                      f.blockMs);
        out += b;
    }
    if (f.recentRepeats >= kRepeatsShown) { // the same block twice: read faster than filled (ninth review)
        std::snprintf(b, sizeof b, "; %lld input blocks came twice lately - the device is read faster than it is "
                      "filled: choose a longer input period, or a faster timer in a virtual cable", f.recentRepeats);
        out += b;
    }
    return out;
}

} // namespace ap
