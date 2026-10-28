// What a deck's meters and graphs show, read from its meter and tap rings on the GUI thread: loudness (EBU M / S /
// I / LRA), true peak, the gain of each stage, the signal-chain levels, spectra, stereo statistics and the last 60 s
// of loudness and gains. Shared by the program (AudioIO, both decks) and the plug-in (one deck). Not real-time
// safe (the loudness histories grow): never on the audio thread.
#pragma once

#include "analysis.h"
#include "deck.h"
#include "loudness.h"

#include <cstdint>
#include <vector>

namespace ap {

struct DeckMeters {
    bool live = false;                    // records arrived during the last 0.5 s
    double inM = kFloorDb, inI = kFloorDb; // input loudness (LUFS)
    double outM = kFloorDb, outS = kFloorDb, outI = kFloorDb, outLra = 0.0;
    double outMaxM = kFloorDb;
    double tpNow[2] = {kFloorDb, kFloorDb}; // output true peak, held (dBTP)
    double inPeak[2] = {kFloorDb, kFloorDb}; // input sample peak, held the same way (dBFS)
    double tpMax = kFloorDb;              // output true peak since the last reset
    double levellerDb = 0, compressorDb = 0, deEsserDb = 0, clipperDb = 0, limiterDb = 0;
    double densityDb[kMaxBands] = {};
    int bands = 0;
    double autoToneDb[4] = {};            // the auto tone's corrections now (bass, low-mid, presence, treble)
    uint8_t flags = 0;                    // MeterFlag bits of the newest record
    double seconds = 0;                   // measured since the last reset
    double compressorIn = kFloorDb;       // compressor detector level (same scale as its threshold)
    double chain[kChainTaps] = {};        // mean L^2 + R^2 per frame at each point of the chain, over about 0.4 s
    uint16_t active = 0;                  // MeterRecord::active of the newest record
};

// A waveform of the last kSeconds for the Graphs window's scope (client, 2026-10-27: "an oscilloscope, L and R, to
// see whether it clips - of the input and the output, chosen with a right-click"): per bucket of time each channel's
// lowest and highest sample and how many samples stood at full scale (the output's clip guard holds them there; at
// the input it is the source that clips).
struct WaveScope {
    static constexpr int kBuckets = 400;
    static constexpr double kSeconds = 2.0;
    static constexpr float kClipLevel = 0.9999f; // |x| at or over this: at full scale (-0.001 dBFS)
    struct Bucket {
        float lo[2] = {0.0f, 0.0f}, hi[2] = {0.0f, 0.0f};
        int clipped[2] = {0, 0};
    };
    std::vector<Bucket> buckets = std::vector<Bucket>(kBuckets); // ring
    int pos = 0;                                                  // the oldest
    // the last samples themselves, for the short time spans (client, 2026-10-27: "the sines are too close together"):
    // kRaw frames, 0.68 s at 48 kHz
    static constexpr int kRaw = 32768;
    static_assert((kRaw & (kRaw - 1)) == 0, "kRaw is a power of two: sample() masks with it");
    std::vector<float> raw[2] = {std::vector<float>(kRaw), std::vector<float>(kRaw)};
    long long rawWritten = 0; // frames written so far: the newest is rawWritten - 1
    double fs = 48000.0;
    void reset(double fs);
    void feed(const float* left, const float* right, int n);
    // the buckets oldest first
    const Bucket& at(int i) const { return buckets[static_cast<size_t>((pos + i) % kBuckets)]; }
    // sample `i` (absolute: rawWritten - kRaw <= i < rawWritten) of channel c
    float sample(int c, long long i) const { return raw[c][static_cast<size_t>(i & (kRaw - 1))]; }

private:
    Bucket cur_;
    int curFrames_ = 0, perBucket_ = 240;
};

// What the graphs show (analysis.h): spectra and stereo statistics of a deck's input and output, the last 60 s of
// loudness and gains, a point every 0.1 s, and the input's and output's waveform of the last 2 s.
struct DeckAnalysis {
    static constexpr int kHistory = 600;
    struct Point {
        float inM = static_cast<float>(kFloorDb), outM = static_cast<float>(kFloorDb); // LUFS
        float leveller = 0, compressor = 0, density = 0;                                 // dB (compressor <= 0)
        bool live = false;
    };
    SpectrumAnalyzer specIn, specOut;
    StereoAnalyzer stereoIn, stereoOut;
    std::vector<Point> history = std::vector<Point>(kHistory); // ring
    int historyPos = 0;
    WaveScope scopeIn, scopeOut;
    // the history oldest first
    const Point& at(int i) const { return history[static_cast<size_t>((historyPos + i) % kHistory)]; }
};

class DeckMonitor {
public:
    DeckMonitor();
    // GUI timer: drains the deck's rings (the only reader of them). `fs` = the deck's sample rate (0: none yet),
    // `dt` = seconds since the last call (a history point every 0.1 s).
    void poll(DeckProcessor& deck, double fs, double dt);
    // Starts the meters and the history again (the live flag stays).
    void reset();
    const DeckMeters& meters() const { return m_; }
    const DeckAnalysis& analysis() const { return a_; }

private:
    void drainTaps(DeckProcessor& deck, double fs);

    LoudnessMeter in_, out_;
    DeckMeters m_;
    DeckAnalysis a_;
    double quietFor_ = 1e9, analysisRate_ = 0, historyTime_ = 0;
};

} // namespace ap
