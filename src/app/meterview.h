// SPDX-License-Identifier: GPL-3.0-or-later
// The meter panel of a deck strip: LED bars of the input's and output's peak per channel (In L / R, Out L / R) with
// the loudness of each (momentary LUFS) and the target as lines across them, a dB and a LUFS scale; the gain of each
// stage (leveller up / down, noise reduction, compressor, de-esser, clipper, limiter) and under them the spectrum of input and output
// with the density bands and their gains (spectrumdraw.h; client, 2026-10-26: the density row went into the spectrum,
// which got its room). No buttons: the R (reset meters) and F (freeze leveller) buttons were
// removed at the client's request (2026-10-25: "not needed").
// Custom-drawn, double-buffered, sized by the window's DPI.
#pragma once

#include "monitor.h"
#include "deck.h"

#include <windows.h>

namespace ap {

struct MeterViewData {
    DeckMeters m;
    double target = -14, ceiling = -1, levRange = 12; // of the deck's preset
    double inputGain = 0; // the preset's input gain (0 when bypassed): the In bars show the input after it
    bool stageOn[kStageCount] = {};
    bool processing = true;
    bool audio = false; // a device or a simulation runs
    // spectrum under the stage rows: input (original) and output (processed), dB at SpectrumAnalyzer::pointHz(i);
    // the density splits in use (bands - 1 of them)
    std::array<float, SpectrumAnalyzer::kPoints> specIn{}, specOut{};
    bool spectrum = false; // a spectrum to show
    double splits[kMaxSplits] = {};
    // the colour's EQ curve in the spectrum (client, 2026-10-27: "show the yellow colour line in the main window
    // too"): the deck's values (preset merged with its colour), the auto tone now, the rate in use
    PresetValues colourValues{};
    double autoDb[4] = {};
    double fs = 48000.0;
};

// A click on a stage row or a LED bar (client, 2026-10-25: "open its settings") tells the parent with WM_COMMAND:
// its control id, code kMeterOpenRow + the target (0 leveller, 1 noise reduction (2026-10-29), 2 compressor, 3 density -
// no row of its own since 2026-10-26, 4 de-esser, 5 clipper, 6 limiter; 7 the In bars: General - input gain, target;
// 8 the Out bars: Limiter - their ceiling; 9 the spectrum: the spectrum window (in the plug-in too since 2026-10-29;
// before, the Density tab); meterStage()).
constexpr int kMeterOpenRow = 10;
constexpr int kMeterRows = 7, kMeterTargets = 10, kMeterSpectrum = 9;
const char* meterStage(int target); // the preset window's tab of a target ("Leveller", ...)

void registerMeterView(HINSTANCE instance);
void unregisterMeterView(); // a plug-in DLL, before it is unloaded (the class would point into freed code)
// The panel has its own tooltips, one per part (bars, stage rows, numbers).
HWND createMeterView(HWND parent, int id);
void setMeterView(HWND view, const MeterViewData& data); // copies and repaints
// The smallest panel height at `dpi` that shows every stage row in full and a readable spectrum (client, 2026-10-26:
// the smallest window that still shows the spectrum is the window's minimum).
int meterViewMinHeight(UINT dpi);

} // namespace ap
