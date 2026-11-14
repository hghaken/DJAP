// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the EQ window's Auto tone tab (client, 2026-11-14: "make these graphical too"): four panels in one
// strip (graphstrip.h) -
//   the corrections of the last 60 s, a line per tone section (bass, low-mid, presence, treble; the spectrum
//     window's colours), with the maximum as lines above and under them;
//   the amount: the correction against how far the song is from the reference in that section - a slope, cut off
//     at the maximum -, with each section where it is now;
//   the time: how fast the correction follows a change in the song's balance;
//   the correction now as an EQ curve.
#pragma once

#include "graphstrip.h"

namespace ap {

class AutoToneGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Auto tone"; }
    bool owns(int param) const override { return param == pAutoAmount || param == pAutoMax || param == pAutoTime; }

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override { return mark != mTime; }
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the maximum's lines over the corrections
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;
    double stepped(int param, int steps) const override;

private:
    static constexpr int kPanels = 4; // corrections, amount, time, correction now
    enum Mark { mMax, mAmount, mTime, kMarks };
    std::wstring valueText(int j) const;

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_; // amount, maximum, time: in the first panel's header
    int titleW_ = 120;
    std::vector<Label> layerNames_;
};

} // namespace ap
