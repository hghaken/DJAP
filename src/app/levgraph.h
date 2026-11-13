// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's Leveller tab (client, 2026-11-13, after the Compressor and MB Compressor tabs
// had become all graphs: "make the same for the leveller"): four panels in one strip (graphstrip.h) -
//   level and gain: the last 30 s of the deck, in dB to the target - the music as it would come out without the
//     leveller, the leveller's gain around the target's line, and the level that comes out (its 3 s average). Five
//     values are lines in it: the range (the two level lines the gain stays between), the window (the band around
//     the target in which the gain rests), "hold below" (the level line under which the music is left alone), the
//     quiet wait (an upright line that long before now) and "after silence" (the level line the gain goes to);
//   the attack, the release and the speed: an example difference - 10 dB too loud, 10 dB too soft, 3 dB too loud -
//     and how it comes out over time, on one logarithmic time axis; the upright line is the time it takes.
// Level lines and upright lines throughout, taken anywhere along them (client, while it was being built: "use
// horizontal and vertical lines here too").
// The tab keeps the stage's switch alone. The five values of the first panel have a legend in its header, the other
// three a reset icon in their corner.
#pragma once

#include "graphstrip.h"

namespace ap {

class LevellerGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Leveller"; }
    bool owns(int param) const override;

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override;
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the level lines and marks, over the audio
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;
    double stepped(int param, int steps) const override;

private:
    static constexpr int kPanels = 4; // level and gain, attack, release, speed
    enum Mark { mRange, mWindow, mHold, mQuietWait, mSilence, mAttack, mRelease, mSpeed, kMarks };
    // An example difference of `fromDb` (over the target: positive) closing at `rate` dB/s, with the mark at
    // `markS` seconds and `value` written at it.
    void drawRamp(Gdiplus::Graphics& g, Plot& q, int mark, double fromDb, double rate, double markS,
                  const std::wstring& value, std::vector<Label>& labels);
    double reference() const;  // LUFS: the level the output is brought to
    double silenceDb() const;  // where "after silence" is drawn (at "hold": under the target's line)
    double silenceMarkX() const; // where its mark stands on its line

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_;                   // range, window, hold below, quiet wait, after silence
    int titleW_ = 120, numberW_ = 90; // the widest title of the first panel and the gain's number
    std::vector<Label> names_;        // the names at the first panel's lines, written after the layer
};

} // namespace ap
