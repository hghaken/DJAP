// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.h"

#include <algorithm>
#include <cmath>

namespace ap {

DeckMonitor::DeckMonitor() {
    in_.reset();
    out_.reset();
}

void DeckMonitor::poll(DeckProcessor& deck, double fs, double dt) {
    DeckMeters& m = m_;
    MeterRecord recs[64];
    int n, total = 0;
    float tp[2] = {0, 0}, pin[2] = {0, 0};
    double comp = 0, dess = 0, clip = 0, lim = 0;
    while ((n = deck.meters().pop(recs, 64)) > 0) {
        for (int i = 0; i < n; ++i) {
            const MeterRecord& r = recs[i];
            in_.addRecord(r.energyIn, r.frames);
            out_.addRecord(r.energyOut, r.frames);
            tp[0] = std::max(tp[0], r.tpOut[0]);
            tp[1] = std::max(tp[1], r.tpOut[1]);
            pin[0] = std::max(pin[0], r.peakIn[0]);
            pin[1] = std::max(pin[1], r.peakIn[1]);
            comp = std::max(comp, static_cast<double>(r.compressorDb));
            dess = std::max(dess, static_cast<double>(r.deEsserDb));
            clip = std::max(clip, static_cast<double>(r.clipperDb));
            lim = std::max(lim, static_cast<double>(r.limiterDb));
            m.levellerDb = r.levellerDb;
            m.compressorIn = r.compressorIn;
            m.bands = r.bands;
            for (int b = 0; b < kMaxBands; ++b) m.densityDb[b] = r.densityDb[b];
            for (int k = 0; k < 4; ++k) m.autoToneDb[k] = r.autoToneDb[k];
            m.flags = r.flags;
            m.seconds += r.frames / std::max(1.0, fs);
            // the signal-chain bars: each point's level, a 0.4 s one-pole over the records
            if (r.frames > 0) {
                const double a = std::min(1.0, r.frames / (0.4 * std::max(1.0, fs)));
                for (int k = 0; k < kChainTaps; ++k) m.chain[k] += a * (r.chain[k] / r.frames - m.chain[k]);
            }
            m.active = r.active;
        }
        total += n;
    }
    quietFor_ = total > 0 ? 0.0 : quietFor_ + dt;
    m.live = quietFor_ < 0.5;
    if (total > 0) {
        m.compressorDb = comp;
        m.deEsserDb = dess;
        m.clipperDb = clip;
        m.limiterDb = lim;
    }
    // true peak: the newest value, falling 20 dB/s when lower
    for (int c = 0; c < 2; ++c) {
        const double now = linToDb(tp[c]);
        m.tpNow[c] = std::max(now, m.tpNow[c] - 20.0 * dt);
        m.tpMax = std::max(m.tpMax, now);
        m.inPeak[c] = std::max(linToDb(pin[c]), m.inPeak[c] - 20.0 * dt);
    }
    m.inM = in_.momentary();
    m.inI = in_.integrated();
    m.outM = out_.momentary();
    m.outS = out_.shortTerm();
    m.outI = out_.integrated();
    m.outLra = out_.loudnessRange();
    m.outMaxM = out_.maxMomentary();
    if (!m.live) {
        m.tpNow[0] = m.tpNow[1] = kFloorDb;
        m.inPeak[0] = m.inPeak[1] = kFloorDb;
        m.levellerDb = m.compressorDb = m.deEsserDb = m.clipperDb = m.limiterDb = 0;
        m.compressorIn = kFloorDb;
        for (double& x : m.densityDb) x = 0;
        for (double& x : m.chain) x = 0;
    }
    drainTaps(deck, fs);

    // graph history: a point every 0.1 s
    historyTime_ += dt;
    if (historyTime_ >= 0.1 - 1e-9) {
        historyTime_ = 0;
        DeckAnalysis::Point p;
        p.live = m.live;
        p.inM = static_cast<float>(m.inM);
        p.outM = static_cast<float>(m.outM);
        p.leveller = static_cast<float>(m.levellerDb);
        p.compressor = static_cast<float>(-m.compressorDb);
        double dens = 0;
        for (int b = 0; b < m.bands; ++b) dens += m.densityDb[b];
        p.density = static_cast<float>(m.bands > 0 ? dens / m.bands : 0.0);
        a_.history[static_cast<size_t>(a_.historyPos)] = p;
        a_.historyPos = (a_.historyPos + 1) % DeckAnalysis::kHistory;
    }
}

void WaveScope::reset(double rate) {
    fs = rate > 0 ? rate : 48000.0;
    buckets.assign(kBuckets, Bucket());
    pos = 0;
    for (auto& r : raw) r.assign(kRaw, 0.0f);
    rawWritten = 0;
    cur_ = Bucket();
    curFrames_ = 0;
    perBucket_ = std::max(1, static_cast<int>(fs * kSeconds / kBuckets + 0.5));
}

void WaveScope::feed(const float* left, const float* right, int n) {
    for (int i = 0; i < n; ++i) {
        // (a non-finite input sample counts as 0 - ninth outside review; tenth: an infinity would make its bucket's
        // range infinite, a NaN only fails the bucket's comparisons and is dropped, and the raw samples drawn would
        // carry either)
        const float x[2] = {std::isfinite(left[i]) ? left[i] : 0.0f, std::isfinite(right[i]) ? right[i] : 0.0f};
        raw[0][static_cast<size_t>(rawWritten & (kRaw - 1))] = x[0];
        raw[1][static_cast<size_t>(rawWritten & (kRaw - 1))] = x[1];
        ++rawWritten;
        for (int c = 0; c < 2; ++c) {
            if (curFrames_ == 0 || x[c] < cur_.lo[c]) cur_.lo[c] = x[c];
            if (curFrames_ == 0 || x[c] > cur_.hi[c]) cur_.hi[c] = x[c];
            if (std::fabs(x[c]) >= kClipLevel) ++cur_.clipped[c];
        }
        if (++curFrames_ >= perBucket_) {
            buckets[static_cast<size_t>(pos)] = cur_;
            pos = (pos + 1) % kBuckets;
            cur_ = Bucket();
            curFrames_ = 0;
        }
    }
}

void DeckMonitor::drainTaps(DeckProcessor& deck, double fs) {
    DeckAnalysis& a = a_;
    if (fs > 0 && fs != analysisRate_) { // a new rate: the analyzers start again
        analysisRate_ = fs;
        a.specIn.reset(fs);
        a.specOut.reset(fs);
        a.stereoIn.reset(fs);
        a.stereoOut.reset(fs);
        a.scopeIn.reset(fs);
        a.scopeOut.reset(fs);
    }
    constexpr int kBlock = 1024;
    TapFrame frames[kBlock];
    float inL[kBlock], inR[kBlock], outL[kBlock], outR[kBlock], inMono[kBlock], outMono[kBlock];
    int n;
    while ((n = deck.taps().pop(frames, kBlock)) > 0) {
        for (int i = 0; i < n; ++i) {
            inL[i] = frames[i].inL;
            inR[i] = frames[i].inR;
            outL[i] = frames[i].outL;
            outR[i] = frames[i].outR;
            inMono[i] = 0.5f * (inL[i] + inR[i]);
            outMono[i] = 0.5f * (outL[i] + outR[i]);
        }
        a.specIn.feed(inMono, n);
        a.specOut.feed(outMono, n);
        a.stereoIn.feed(inL, inR, n);
        a.stereoOut.feed(outL, outR, n);
        a.scopeIn.feed(inL, inR, n);
        a.scopeOut.feed(outL, outR, n);
    }
}

void DeckMonitor::reset() {
    in_.reset();
    out_.reset();
    a_.history.assign(DeckAnalysis::kHistory, DeckAnalysis::Point());
    if (analysisRate_ > 0) {
        a_.scopeIn.reset(analysisRate_);
        a_.scopeOut.reset(analysisRate_);
    }
    DeckMeters fresh;
    fresh.live = m_.live;
    m_ = fresh;
}

} // namespace ap
