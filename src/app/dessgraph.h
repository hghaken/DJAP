// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's De-esser tab (client, 2026-11-14: "make these graphical too", after the
// Leveller, Noise, Compressor and MB Compressor tabs): three panels in one strip (graphstrip.h) -
//   the high band of the last 4 s - what the de-esser listens to, on the scale of its threshold - with the threshold
//     as a line across it and, from the top, how much it turns the highs down;
//   the reduction against how far the high band is over the threshold: 4:1, never more than the maximum;
//   the frequency: the music's spectrum from 1 kHz, the part the detector hears (its high-pass) and the shelf it
//     turns down - now and at the most.
// The tab keeps the stage's switch alone.
#pragma once

#include "graphstrip.h"

namespace ap {

class DeEsserGraphs : public GraphStrip {
public:
    const char* stage() const override { return "De-esser"; }
    bool owns(int param) const override;

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override { return mark != mFrequency; }
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the threshold's line over the level
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;

private:
    static constexpr int kPanels = 3; // the high band, the reduction curve, the frequency
    enum Mark { mThreshold, mMax, mFrequency, kMarks };
    double aim() const; // the level the threshold is relative to
    std::wstring valueText(int j) const;

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_; // threshold, maximum, frequency: in the high band panel's header
    int titleW_ = 120, numberW_ = 46;
    std::vector<Label> layerNames_; // the names at the lines of the layer, written after it
};

} // namespace ap
