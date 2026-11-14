// SPDX-License-Identifier: GPL-3.0-or-later
// The graph of the EQ window's Tone tab (client, 2026-11-14: "make these graphical too"): the EQ curve over the
// deck's spectrum, one panel in a strip (graphstrip.h) - the four tone sections (bass shelf, low-mid bell, presence
// bell, treble shelf) each a thin line in its colour and a point to drag both ways (left / right its frequency, up /
// down its gain), together the thick line; the auto tone's corrections now dashed. The colours are the spectrum
// window's, which can drag the same points.
#pragma once

#include "graphstrip.h"

namespace ap {

class ToneGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Tone"; }
    bool owns(int param) const override;

protected:
    int markCount() const override { return 4; }
    int markParam(int mark) const override;  // the point's frequency
    int markParam2(int mark) const override; // its gain
    bool markUpDown(int mark) const override { (void)mark; return true; }
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
};

} // namespace ap
