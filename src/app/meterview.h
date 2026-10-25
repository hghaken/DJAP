// The meter panel of a deck strip: LED bars of the input's and output's peak per channel (In L / R, Out L / R) with
// the loudness of each (momentary LUFS) and the target as lines across them, a dB and a LUFS scale; the gain of each
// stage (leveller up / down, compressor, density per band, de-esser, clipper, limiter) and the spectrum of input and
// output under them with the density splits. No buttons: the R (reset meters) and F (freeze leveller) buttons were
// removed at the client's request (2026-10-25: "not needed").
// Custom-drawn, double-buffered, sized by the window's DPI.
#pragma once

#include "audioio.h"
#include "deck.h"

#include <windows.h>

namespace ap {

struct MeterViewData {
    DeckMeters m;
    double target = -14, ceiling = -1, levRange = 9; // of the deck's preset
    double inputGain = 0; // the preset's input gain (0 when bypassed): the In bars show the input after it
    bool stageOn[kStageCount] = {};
    bool processing = true;
    bool audio = false; // a device or a simulation runs
    // spectrum under the stage rows: input (original) and output (processed), dB at SpectrumAnalyzer::pointHz(i);
    // the density splits in use (bands - 1 of them)
    std::array<float, SpectrumAnalyzer::kPoints> specIn{}, specOut{};
    bool spectrum = false; // a spectrum to show
    double splits[kMaxSplits] = {};
};

// A click on a stage row or a LED bar (client, 2026-10-25: "open its settings") tells the parent with WM_COMMAND:
// its control id, code kMeterOpenRow + the target (0 leveller, 1 compressor, 2 density, 3 de-esser, 4 clipper,
// 5 limiter; 6 the In bars: General - input gain, target; 7 the Out bars: Limiter - their ceiling; meterStage()).
constexpr int kMeterOpenRow = 10;
constexpr int kMeterRows = 6, kMeterTargets = 8;
const char* meterStage(int target); // the preset window's tab of a target ("Leveller", ...)

void registerMeterView(HINSTANCE instance);
// The panel has its own tooltips, one per part (bars, stage rows, numbers).
HWND createMeterView(HWND parent, int id);
void setMeterView(HWND view, const MeterViewData& data); // copies and repaints

} // namespace ap
