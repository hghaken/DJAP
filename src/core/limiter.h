// Look-ahead true-peak limiter (handbook 10.10; DAFX 2nd ed. ch. 4).
//
// Per sample: wanted gain w = min(1, ceiling / peak) -> sliding minimum over W -> moving average over W ->
// instant attack / one-pole release -> applied to the input delayed by `look` frames.
// Window: for the sample leaving the delay line the gain must never exceed its own wanted gain. With a sample-peak
// detector W = look + 1. With the true-peak detector of delay D the wanted gain of sample i is known D frames after
// it arrives (from the reports of the intervals on both sides of it), so W = look - D + 1; the true peak is used
// only when look >= D, else the sample peak.
#pragma once

#include "truepeak.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ap {

class Limiter {
public:
    // Allocates for look-aheads up to maxLook frames. Not real-time safe.
    void prepare(int maxLook, int tpFactor, int tpTapsPerPhase, double kaiserBeta = 7.0);
    int detectorDelay() const { return tpL_.delay(); }
    // Sets the look-ahead (clamped to maxLook) and resets all state. Real-time safe (no allocation).
    void setLook(int look);
    int look() const { return look_; }
    bool usesTruePeak() const { return useTp_; }
    int maxLook() const { return maxLook_; }
    void reset();

    // ceiling as linear amplitude; release as one-pole coefficient per sample.
    void setParams(double ceiling, double releaseCoeff) {
        ceilingSet_ = ceiling;
        ceiling_ = useTp_ ? ceiling * kTruePeakMargin : ceiling;
        release_ = releaseCoeff;
    }
    // In true-peak mode the limiter aims 0.1 dB under its ceiling: with a 1.5 ms look-ahead the gain changes fast
    // enough to make inter-sample peaks of its own; full-band noise measured up to 0.17 dB over without the margin.
    static constexpr double kTruePeakMargin = 0.98855309465693886; // -0.1 dB

    // In: the newest frame. Out (in place): the frame `look` frames old, multiplied by the gain.
    inline void process(double& l, double& r) {
        // 1. wanted gain
        double w;
        if (useTp_) {
            const double rep = TruePeakDetector::pushPair(tpL_, tpR_, static_cast<float>(l), static_cast<float>(r));
            const double peak = std::max(rep, lastReport_);
            lastReport_ = rep;
            w = peak > ceiling_ ? ceiling_ / peak : 1.0;
        } else {
            const double peak = std::max(std::fabs(l), std::fabs(r));
            w = peak > ceiling_ ? ceiling_ / peak : 1.0;
        }
        // 2. sliding minimum over the last W wanted gains (monotonic deque in a fixed ring)
        while (dqCount_ > 0 && dqVal_[back()] >= w) --dqCount_;
        const int slot = wrap(dqHead_ + dqCount_);
        dqVal_[slot] = w;
        dqIdx_[slot] = n_;
        ++dqCount_;
        if (dqIdx_[dqHead_] <= n_ - window_) {
            dqHead_ = wrap(dqHead_ + 1);
            --dqCount_;
        }
        const double mn = dqVal_[dqHead_];
        // 3. moving average of the minima over the same W
        sum_ += mn - avgRing_[avgPos_];
        avgRing_[avgPos_] = mn;
        avgPos_ = avgPos_ + 1 == window_ ? 0 : avgPos_ + 1;
        if (++resum_ >= 8192) {
            resum_ = 0;
            double s = 0.0;
            for (int i = 0; i < window_; ++i) s += avgRing_[i];
            sum_ = s;
        }
        const double avg = sum_ / window_;
        // 4. instant attack, one-pole release (never above the average)
        gain_ = avg < gain_ ? avg : toward(gain_, avg, release_);
        if (gain_ < minGain_) minGain_ = gain_;
        // 5. delay
        if (look_ > 0) { // ring of `look` frames: the slot read was written `look` frames ago
            const double dl = delayL_[delayPos_], dr = delayR_[delayPos_];
            delayL_[delayPos_] = l;
            delayR_[delayPos_] = r;
            delayPos_ = delayPos_ + 1 >= look_ ? 0 : delayPos_ + 1;
            l = dl;
            r = dr;
        }
        l *= gain_;
        r *= gain_;
        ++n_;
    }

    double gain() const { return gain_; }
    // Lowest gain since the last call (for meters).
    double takeMinGain() {
        const double g = minGain_;
        minGain_ = 1.0;
        return g;
    }
    bool finite() const { return std::isfinite(gain_) && std::isfinite(sum_); }

private:
    // index into the deque ring for 0 <= i < 2 * cap_ (no integer division on the audio path)
    int wrap(int i) const { return i >= cap_ ? i - cap_ : i; }
    int back() const { return wrap(dqHead_ + dqCount_ - 1); }
    static double toward(double now, double target, double a) { return target + a * (now - target); }

    int maxLook_ = 0, look_ = 0, window_ = 1, cap_ = 1;
    bool useTp_ = false;
    TruePeakDetector tpL_, tpR_;
    double lastReport_ = 0.0;
    double ceilingSet_ = 1.0, ceiling_ = 1.0, release_ = 0.0, gain_ = 1.0, minGain_ = 1.0, sum_ = 0.0;
    long long n_ = 0;
    int resum_ = 0;
    std::vector<double> dqVal_;
    std::vector<long long> dqIdx_;
    int dqHead_ = 0, dqCount_ = 0;
    std::vector<double> avgRing_;
    int avgPos_ = 0;
    std::vector<double> delayL_, delayR_;
    int delayPos_ = 0;
};

} // namespace ap
