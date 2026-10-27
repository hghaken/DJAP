// Offline rendering through the real deck processors, in the same chunks as a device would use.
#pragma once

#include "deck.h"
#include "loudness.h"
#include "wavfile.h"

#include <functional>
#include <string>

namespace ap {

struct RenderDeck {
    bool used = false;
    PresetValues values{};
    DeckControl control;
};

struct RenderStats {
    LoudnessMeter in, out;     // fed from the deck's own meter records
    double maxTruePeakDb = kFloorDb; // output, from the deck's 4x meter detector
    double maxLimiterDb = 0.0, meanLimiterDb = 0.0, maxCompressorDb = 0.0;
    double meanLevellerDb = 0.0;
    long long records = 0;
};

struct RenderResult {
    RenderStats deck[2];
    int latency = 0;           // frames
    double processSeconds = 0; // wall time spent in DeckProcessor::process
    double audioSeconds = 0;
};

// Every meter record of a deck as it comes (DJAPCmd --trace: the stages' gains over time).
using RecordSink = std::function<void(int deck, const MeterRecord& record)>;

// Input: 2 channels = deck A; 4 channels = deck A (1-2) and deck B (3-4). A deck that is not `used` passes through
// unprocessed (bypassed, delayed like the other). align = true removes the look-ahead delay from the output (same
// length as the input); false keeps it like a live device (the output longer by the delay: nothing cut off).
bool renderOffline(const WavData& in, const RenderDeck decks[2], bool align, int chunkFrames, WavData& out,
                   RenderResult& result, std::string& error, const RecordSink& onRecord = {});

} // namespace ap
