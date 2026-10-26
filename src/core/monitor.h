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
    uint8_t flags = 0;                    // MeterFlag bits of the newest record
    double seconds = 0;                   // measured since the last reset
    double compressorIn = kFloorDb;       // compressor detector level (same scale as its threshold)
    double chain[kChainTaps] = {};        // mean L^2 + R^2 per frame at each point of the chain, over about 0.4 s
    uint16_t active = 0;                  // MeterRecord::active of the newest record
};

// What the graphs show (analysis.h): spectra and stereo statistics of a deck's input and output, and the last 60 s
// of loudness and gains, a point every 0.1 s.
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
