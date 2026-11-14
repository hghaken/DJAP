// SPDX-License-Identifier: GPL-3.0-or-later
// The graph of the EQ window's Tone tab (client, 2026-11-14: "make these graphical too"): the EQ curve over the
// deck's spectrum, one panel in a strip (graphstrip.h) - the four tone sections (bass shelf, low-mid bell, presence
// bell, treble shelf) each a thin line in its colour and a point to drag both ways (left / right its frequency, up /
// down its gain), together the thick line; the auto tone's corrections now dashed. The colours are the spectrum
// window's, which can drag the same points. Through each point an upright dashed line (its frequency, dragged left
// / right) and a level one (its gain, dragged up / down), and its name with both values beside it (client,
// 2026-11-14: "add horizontal and vertical drag lines, and add the values").
#pragma once

#include "graphstrip.h"

namespace ap {

class ToneGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Tone"; }
    bool owns(int param) const override;

protected:
    // marks 0..3 the points (both ways), 4..7 their upright lines (the frequency), 8..11 their level lines (the gain)
    int markCount() const override { return 12; }
    int markParam(int mark) const override;  // a point's or upright line's frequency, a level line's gain
    int markParam2(int mark) const override; // a point's gain
    bool markUpDown(int mark) const override { return mark >= 8; }
    int lineAt(POINT pt) const override;
    int partner(int param) const override;  // a point's gain <-> its frequency: one reset icon for both
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the curves and points over the spectrum
    void paintLive(HDC dc) override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    bool dragValue2(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;

private:
    std::wstring valueText(int j) const;

    RECT panel_ = {};
    Plot plot_;
    Legend legend_; // the four points: name, gain and frequency
    int titleW_ = 120;
    std::vector<Label> layerNames_;
    // where the lines were drawn (renderLayer): each upright line's x, each level line's y and its left and right end
    double lineX_[4] = {}, lineY_[4] = {}, lineL_[4] = {}, lineR_[4] = {};
};

} // namespace ap
