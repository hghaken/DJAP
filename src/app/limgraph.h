// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's Limiter tab (client, 2026-11-14: "make these graphical too"): three panels in
// one strip (graphstrip.h) -
//   the peaks of the last 4 s: going in (about: what came out plus what the limiter took), coming out, and the
//     ceiling as a line across them - what the limiter cuts is orange;
//   the release: the gain after a peak the limiter turned down 6 dB;
//   peaks in -> out: straight on under the ceiling, flat at it.
// The tab keeps the stage's switch alone; its look-ahead is in the Setup window.
#pragma once

#include "graphstrip.h"

namespace ap {

class LimiterGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Limiter"; }
    bool owns(int param) const override { return param == pLimCeiling || param == pLimRelease; }

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override { return mark != mRelease; }
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the ceiling's line over the peaks
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;
    double stepped(int param, int steps) const override;

private:
    static constexpr int kPanels = 3; // the peaks, the release, in -> out
    enum Mark { mCeiling, mRelease, mCeilingCurve, kMarks };

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_; // the ceiling, in the peaks panel's header
    int titleW_ = 120, numberW_ = 46;
    std::vector<Label> layerNames_;
};

} // namespace ap
