// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's Stereo width tab (client, 2026-11-14: "make these graphical too"): four panels
// in one strip (graphstrip.h) -
//   the width over the frequency: the bass width under the split, the width over it, the crossover between them -
//     three lines to drag, the deck's spectrum behind for where the bass is;
//   the stereo picture of the deck's input and output (a goniometer: mono is upright, wide spreads sideways);
//   side / mid of the last 60 s, in and out: how wide the music is;
//   the correlation of the last 60 s, in and out: under 0 the channels cancel when played in mono.
// The tab keeps the stage's switch alone.
#pragma once

#include "graphstrip.h"

namespace ap {

class WidthGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Stereo width"; }
    bool owns(int param) const override { return param == pWidth || param == pBassWidth || param == pWidthSplit; }

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override { return mark != mSplit; }
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the width's lines over the spectrum
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;

private:
    static constexpr int kPanels = 4; // width over frequency, stereo picture, side / mid, correlation
    enum Mark { mWidth, mBass, mSplit, kMarks };
    double widthAt(double hz) const; // the stage's width at hz, in % (as the deck's crossover sums the two parts)
    std::wstring valueText(int j) const;

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_; // width, bass width, split: in the first panel's header
    int titleW_ = 120;
    std::vector<Label> layerNames_;
};

} // namespace ap
