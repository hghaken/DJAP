// SPDX-License-Identifier: GPL-3.0-or-later
// The spectrum graph, drawn the same way in the meter panel and in the spectrum window (client, 2026-10-26: "all
// together in a separate, big spectrum graph"): the deck's input (orange) and output (turquoise) spectrum, the density
// stage's bands with their gains now - a line per band (client, 2026-10-26: of strip, shade and lines only the lines
// stayed) -, the colour's EQ curve (in the meter panel the total curve only; client, 2026-10-27) and, in the spectrum
// window, a point per tone section and a handle per density band (its fixed gain, the preset's
// "Band N gain"; client, 2026-10-26: "make the tone bands equal to the density bands" - the density bands' own EQ made
// draggable, the colour's points may snap to the band splits) to drag. Custom-drawn with GDI on the dark meter
// palette.
#pragma once

#include "analysis.h"
#include "params.h"

#include <windows.h>

#include <array>

namespace ap::spectrum {

struct Data {
    std::array<float, SpectrumAnalyzer::kPoints> in{}, out{}; // dB at SpectrumAnalyzer::pointHz(i)
    bool live = false;        // a spectrum to show (audio runs)
    bool densityOn = false;   // the density stage runs
    int bands = 1;
    double splits[kMaxSplits] = {};
    double densityDb[kMaxBands] = {};
    // the colour's EQ curve (the deck's values; the auto tone's corrections now)
    bool colour = false;
    PresetValues values{};
    double autoDb[4] = {};
    double fs = 48000.0;
    int hotNode = -1;         // the tone point under the mouse or being dragged (-1: none)
    // the density bands' fixed gains (the preset's Band N gain) as handles on the EQ scale (the spectrum window)
    bool bandHandles = false;
    double bandGainDb[kMaxBands] = {};
    int hotBand = -1;         // the band handle under the mouse or being dragged
    int snapSplit = -1;       // the split a dragged point sits on (drawn brighter), -1: none
};

// The tone sections a point moves: 0 bass, 1 low-mid, 2 presence, 3 treble.
constexpr int kNodes = 4;
extern const Param kNodeFreq[kNodes], kNodeGain[kNodes];
const wchar_t* nodeName(int n);

// Where the graph was painted: for the mouse.
struct Geometry {
    RECT plot{};
    double eqRange = 12.0; // dB at the top (and minus at the bottom) of the EQ / density scale
    POINT node[kNodes] = {};
    bool nodes = false;    // the points were drawn
    RECT handle[kMaxBands] = {}; // the band handles (their line, a few pixels high)
    int handles = 0;
    int splitX[kMaxSplits] = {};  // the band splits' x
    int splits = 0;
    int xOf(double hz) const;
    double hzAt(int x) const;
    int yOfEq(double db) const;
    double eqAt(int y) const;
    int nodeAt(POINT pt, int radius) const; // the point under pt, -1 for none
    int handleAt(POINT pt, int tolerance) const; // the band handle under pt, -1 for none
};

// Paints the graph into `area`. big: the spectrum window (scales on both sides - spectrum dB on the left, EQ dB on the
// right -, the colour curve, wider labels); else the meter panel's compact layout. `small`: the labels' font.
Geometry paint(HDC dc, const RECT& area, const Data& d, HFONT small, UINT dpi, bool big);

} // namespace ap::spectrum
