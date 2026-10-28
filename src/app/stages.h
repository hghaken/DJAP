// SPDX-License-Identifier: GPL-3.0-or-later
// The nine stage switches of a preset: their parameters, names and tooltips (preset window check boxes, meter rows).
#pragma once

#include "deck.h"
#include "params.h"

namespace ap {

inline constexpr Param kStageParam[kStageCount] = {pLevOn,  pCompOn,  pDensOn, pBassOn, pExcOn,
                                                   pDessOn, pWidthOn, pClipOn, pLimOn};
inline constexpr const wchar_t* kStageName[kStageCount] = {L"Leveller", L"Compressor", L"Density",
                                                           L"Bass",     L"Exciter",    L"De-esser",
                                                           L"Width",    L"Clipper",    L"Limiter"};
inline constexpr const wchar_t* kStageTip[kStageCount] = {
    L"Leveller: slow automatic gain that brings each song to the loudness preset's target loudness (EBU R 128). It holds "
    L"during fades, breakdowns and pauses, and catches up quickly when the deck starts again after silence. "
    L"Freeze leveller (main window) holds it by hand.",
    L"Compressor: wide-band, soft knee. Its threshold sits relative to the target loudness, so it only works on the "
    L"loudest parts and evens out the level within a song. Bass does not trigger it (side-chain high-pass).",
    L"Density: multiband dynamics (1-7 bands, flat sum). Each band is compared with its own recent level and turned "
    L"down when it sticks out or lifted when it falls away: a denser, more even sound. The band gains also work as "
    L"a tone control.",
    L"Bass enhancer: adds overtones to the bass below its frequency, so the bass is heard on small speakers too "
    L"without more low-frequency energy. The main window's Bass enhancer slider switches it too.",
    L"Exciter: adds overtones to the highs above its frequency - shine and air.",
    L"De-esser: turns sharp 's' and 'sh' sounds down (a high shelf around 5.5 kHz) when they stand out.",
    L"Stereo width: mid / side width, from mono to twice as wide as recorded; the bass below its split can be "
    L"kept narrower (or mono). The main window's Bass width slider switches it on.",
    L"Soft clipper: rounds off the highest peaks before the limiter, without delay, so the limiter has less to do. "
    L"More loudness, at some cost of punch.",
    L"Limiter: look-ahead true-peak limiter. The output never passes the ceiling (-1 dBTP by default). Its look-ahead "
    L"(Setup window) is the only delay the processing adds.",
};
inline constexpr const wchar_t* kStageTipEnd =
    L"\n\nThe switch belongs to this loudness preset: every deck that uses it changes with it. Switching on or off fades in "
    L"20 ms (no click).";

} // namespace ap
