#include "loudness.h"

#include <algorithm>

namespace ap {

KWeightingCoeffs kWeightingCoeffs(double fs) {
    // ITU-R BS.1770-5, published for 48 kHz.
    BiquadCoeffs shelf48;
    shelf48.b0 = 1.53512485958697;
    shelf48.b1 = -2.69169618940638;
    shelf48.b2 = 1.19839281085285;
    shelf48.a1 = -1.69065929318241;
    shelf48.a2 = 0.73248077421585;
    BiquadCoeffs hp48;
    hp48.b0 = 1.0;
    hp48.b1 = -2.0;
    hp48.b2 = 1.0;
    hp48.a1 = -1.99004745483398;
    hp48.a2 = 0.99007225036621;
    KWeightingCoeffs k;
    if (fs == 48000.0) {
        k.shelf = shelf48;
        k.highPass = hp48;
    } else {
        k.shelf = design::rerate(shelf48, 48000.0, fs);
        k.highPass = design::rerate(hp48, 48000.0, fs);
    }
    return k;
}

LoudnessMeter::LoudnessMeter() { reset(); }

void LoudnessMeter::reset() {
    std::fill(energy_, energy_ + kRing, 0.0);
    std::fill(frames_, frames_ + kRing, 0u);
    records_ = 0;
    maxM_ = maxS_ = kFloorDb;
    blocks_.clear();
    shortTerms_.clear();
}

double LoudnessMeter::windowPower(int records) const {
    double e = 0.0;
    unsigned long long f = 0;
    for (int i = 0; i < records; ++i) {
        const int slot = static_cast<int>((records_ - 1 - i) % kRing);
        e += energy_[slot];
        f += frames_[slot];
    }
    return f ? e / static_cast<double>(f) : 0.0;
}

void LoudnessMeter::addRecord(double energy, unsigned frames) {
    const int slot = static_cast<int>(records_ % kRing);
    energy_[slot] = energy;
    frames_[slot] = frames;
    ++records_;
    if (records_ >= 40) maxM_ = std::max(maxM_, powerToLufs(windowPower(40)));
    if (records_ >= 300) maxS_ = std::max(maxS_, powerToLufs(windowPower(300)));
    if (records_ % 10 == 0) {
        if (records_ >= 40) blocks_.push_back(windowPower(40));
        if (records_ >= 300) shortTerms_.push_back(windowPower(300));
    }
}

double LoudnessMeter::momentary() const { return records_ >= 40 ? powerToLufs(windowPower(40)) : kFloorDb; }

double LoudnessMeter::shortTerm() const { return records_ >= 300 ? powerToLufs(windowPower(300)) : kFloorDb; }

double LoudnessMeter::integrated() const {
    const double absGate = lufsToPower(-70.0);
    double sum = 0.0;
    std::size_t n = 0;
    for (double p : blocks_)
        if (p > absGate) {
            sum += p;
            ++n;
        }
    if (!n) return kFloorDb;
    const double relGate = sum / static_cast<double>(n) * 0.1; // mean power - 10 LU
    double gated = 0.0;
    std::size_t m = 0;
    for (double p : blocks_)
        if (p > absGate && p > relGate) {
            gated += p;
            ++m;
        }
    return m ? powerToLufs(gated / static_cast<double>(m)) : kFloorDb;
}

double LoudnessMeter::loudnessRange() const {
    const double absGate = lufsToPower(-70.0);
    std::vector<double> kept;
    kept.reserve(shortTerms_.size());
    double sum = 0.0;
    for (double p : shortTerms_)
        if (p > absGate) {
            kept.push_back(p);
            sum += p;
        }
    if (kept.empty()) return 0.0;
    const double relGate = sum / static_cast<double>(kept.size()) * 0.01; // mean power - 20 LU
    std::vector<double> lu;
    lu.reserve(kept.size());
    for (double p : kept)
        if (p > relGate) lu.push_back(powerToLufs(p));
    if (lu.size() < 2) return 0.0;
    std::sort(lu.begin(), lu.end());
    auto percentile = [&](double q) {
        const double pos = q * static_cast<double>(lu.size() - 1);
        const std::size_t i = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(i);
        return i + 1 < lu.size() ? lu[i] + frac * (lu[i + 1] - lu[i]) : lu[i];
    };
    return percentile(0.95) - percentile(0.10);
}

void measureStereo(LoudnessMeter& meter, double fs, const float* left, const float* rightOrNull, std::size_t frames) {
    const KWeightingCoeffs c = kWeightingCoeffs(fs);
    KWeighting kl, kr;
    kl.set(c);
    kr.set(c);
    const unsigned recordFrames = static_cast<unsigned>(fs / 100.0 + 0.5);
    double energy = 0.0;
    unsigned n = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double a = kl.process(left[i]);
        double e = a * a;
        if (rightOrNull) {
            const double b = kr.process(rightOrNull[i]);
            e += b * b;
        }
        energy += e;
        if (++n == recordFrames) {
            meter.addRecord(energy, n);
            energy = 0.0;
            n = 0;
        }
    }
}

} // namespace ap
