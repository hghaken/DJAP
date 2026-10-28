// SPDX-License-Identifier: GPL-3.0-or-later
// Loudness measurement after ITU-R BS.1770-5 and EBU Tech 3341 / 3342.
#pragma once

#include "dsp.h"

#include <cstddef>
#include <vector>

namespace ap {

// K-weighting coefficients (pre-filter shelf and RLB high-pass) for any sample rate.
struct KWeightingCoeffs {
    BiquadCoeffs shelf, highPass;
};
KWeightingCoeffs kWeightingCoeffs(double fs);

// K-weighting of one channel.
struct KWeighting {
    Biquad shelf, highPass;
    void set(const KWeightingCoeffs& c) {
        shelf.c = c.shelf;
        highPass.c = c.highPass;
    }
    inline double process(double x) { return highPass.process(shelf.process(x)); }
    void reset() {
        shelf.reset();
        highPass.reset();
    }
    bool finite() const { return shelf.finite() && highPass.finite(); }
};

// K-weighting of a stereo pair (each lane bit-identical to KWeighting).
struct StereoKWeighting {
    StereoBiquad shelf, highPass;
    void set(const KWeightingCoeffs& c) {
        shelf.set(c.shelf);
        highPass.set(c.highPass);
    }
    inline Pair process(Pair x) { return highPass.process(shelf.process(x)); }
    void reset() {
        shelf.reset();
        highPass.reset();
    }
    bool finite() const { return shelf.finite() && highPass.finite(); }
};

// EBU-mode loudness meter fed with 10 ms records: each record is the sum over its frames of the K-weighted,
// channel-weighted squares (L^2 + R^2 for stereo) and the number of frames. Momentary (400 ms), short-term (3 s),
// integrated (gated, 400 ms blocks every 100 ms) and loudness range (EBU Tech 3342). Not real-time safe: the
// integrated and LRA histories grow. Runs on the GUI or offline thread.
class LoudnessMeter {
public:
    LoudnessMeter();
    void reset();
    void addRecord(double energy, unsigned frames);

    double momentary() const;  // LUFS, kFloorDb before the first full window
    double shortTerm() const;  // LUFS
    double integrated() const; // LUFS, kFloorDb when nothing passed the gates
    double loudnessRange() const; // LU
    double maxMomentary() const { return maxM_; }
    double maxShortTerm() const { return maxS_; }
    double seconds() const { return static_cast<double>(records_) * 0.01; }

private:
    double windowPower(int records) const;

    static constexpr int kRing = 300; // 3 s of 10 ms records
    double energy_[kRing];
    unsigned frames_[kRing];
    long long records_ = 0;
    double maxM_ = kFloorDb, maxS_ = kFloorDb;
    std::vector<double> blocks_;     // 400 ms block powers every 100 ms
    std::vector<double> shortTerms_; // 3 s powers every 100 ms
};

// Offline helper: K-weights a stereo float signal and feeds a meter with 10 ms records.
// `rightOrNull` = null measures a single channel (weight 1.0).
void measureStereo(LoudnessMeter& meter, double fs, const float* left, const float* rightOrNull, std::size_t frames);

} // namespace ap
