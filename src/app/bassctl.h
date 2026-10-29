// SPDX-License-Identifier: GPL-3.0-or-later
// The four bass sliders (client, 2026-10-25; on the main window's strips until 2026-10-26, now in the spectrum
// window - its sliders and its graph's bass point): bass level (the colour's bass shelf), bass enhancer
// and bass width (the loudness preset), bass frequency (both). One rule: the trackbar's positions, the value text,
// and what a position changes.
#pragma once

#include "settings.h"

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace ap::bass {

enum Slider { kLevel, kEnhancer, kWidth, kFreq, kSliders };
extern const wchar_t* const kName[kSliders];
extern const wchar_t* const kTip[kSliders];

// A trackbar for slider k: range (0.5 dB / 5 % / 5 Hz steps), ticks, arrow-key and page steps.
void setup(HWND bar, int k);
// The position and the value text of slider k for a deck's values (Settings::deckValues).
int position(const PresetValues& v, int k);
std::wstring text(const PresetValues& v, int k);
// What slider k at `pos` sets: (parameter, value) pairs, each value within its parameter's range - the colour's
// parameters (bass level, bass corner) and the loudness preset's (enhancer, width, their frequencies). The program
// writes them into the deck's preset and colour (apply), the plug-in into its parameters.
std::vector<std::pair<int, double>> changes(int k, int pos);
// Sets the deck's loudness preset and / or colour from slider k at `pos` (every deck using them changes). False when
// the deck has no such preset or colour.
bool apply(Settings& s, int deck, int k, int pos);

} // namespace ap::bass
