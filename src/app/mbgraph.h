// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's MB Compressor tab (client, 2026-11-13, after the Compressor tab had become all
// graphs: "can a similar graph be made for the MB Compressor? No sliders, everything in the graph", two sketches,
// "yes, build it"): four panels in one strip (graphstrip.h) -
//   the bands over the music's spectrum: the splits as upright dashed lines (dragged left / right), each band's
//     fixed gain as a line with a mark (up / down) and its gain at this moment as a thick line; the number of bands
//     with - and + in the panel's header;
//   the curve per band: a band's level against its own recent level in, what comes out up - the zone in which
//     nothing happens between "lift below" and "down above" (upright lines), the largest lift (a dotted line), the
//     amount (the slope outside the zone), and a dot per band where it is now;
//   the attack and the release, as on the Compressor tab.
// The tab keeps the stage's switch alone. The four values of the curve have a legend in its panel's header and the
// attack and the release an icon in their corner; the splits and the band gains - up to thirteen - get a small
// reset icon beside their value when it differs from its reset value.
#pragma once

#include "graphstrip.h"

namespace ap {

class MbGraphs : public GraphStrip {
public:
    const char* stage() const override { return "MB Compressor"; }
    bool owns(int param) const override;

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override;
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the bands' lines and marks, over the spectrum
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;
    double stepped(int param, int steps) const override;

private:
    static constexpr int kPanels = 4; // bands, curve per band, attack, release
    // the marks: a split's at the top of its line, a band gain's in the middle of its band, then the curve's four
    // and the attack's and the release's
    enum Mark {
        mSplit1 = 0,
        mGain1 = kMaxSplits,
        mDown = mGain1 + kMaxBands,
        mLift,
        mMaxLift,
        mAmount,
        mAttack,
        mRelease,
        kMarks
    };
    int bands() const;                // 1 .. 7
    double split(int k) const;        // Hz; split k lies between band k and band k + 1 (from 0)
    double edgeHz(int b) const;       // band b's lower edge: the scale's start, or the split under it
    double ratio() const;             // 1 .. 4 from the amount
    double gainAt(double x) const;    // dB: what the stage does to a band `x` dB over its own recent level
    double hitReduction() const;      // dB the example hit (10 dB over "down above") is turned down when settled

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_;              // amount, down above, lift below, maximum lift: in the curve panel's header
    RECT bandsItem_ = {};        // "Bands 5" with its steppers, in the first panel's header (its icon: icons_[0])
    int bandsHeaderBottom_ = 0;
    int bandsTitleW_ = 30, curveTitleW_ = 80, digitW_ = 8, hintW_ = 100;
    std::vector<Label> bandNames_; // the texts at the bands' lines, written after the layer
};

} // namespace ap
