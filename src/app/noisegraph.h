// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's Noise tab (client, 2026-11-13, after the Leveller, Compressor and MB Compressor
// tabs: "make a graphical view of the noise tab too"): four panels in one strip (graphstrip.h) -
//   the bands over the music's spectrum: the splits as upright dashed lines (dragged left / right), the threshold as
//     a level line on a scale of LU to the target (up / down), each band's level at this moment as a thick line and
//     what the stage takes off it; the number of bands (1 or 4) with - and + in the panel's header;
//   the curve per band: the gain a band gets against its level - the threshold an upright line, the depth (the
//     floor) and the ratio (the slope, through its mark 5 LU under the threshold) level lines -, and a dot per band
//     where it is now;
//   the attack: a band that was turned down by the depth opens when the music comes back;
//   the hold and the release: the music stops, the band stays open for the hold and then goes down at the release.
// The tab keeps the stage's switch alone. The curve's three values have a legend in the bands panel's header (the
// small curve panel keeps its height), the hold and the release one in their panel's header, the attack an icon in
// its corner; the splits get a small reset icon beside their value when
// it differs from its reset value. Lines and marks are taken anywhere along them; names carry their values.
#pragma once

#include "graphstrip.h"

namespace ap {

class NoiseGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Noise"; }
    bool owns(int param) const override;

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override;
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the bands graph's lines, over the spectrum
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;
    double stepped(int param, int steps) const override;

private:
    static constexpr int kPanels = 4; // bands, curve per band, attack, hold and release
    // the marks: the splits' at the top of their lines, the threshold's in the bands graph and again on the curve,
    // the depth's and the ratio's, the attack's, the hold's and the release's
    enum Mark { mSplit1, mSplit2, mSplit3, mThreshold, mCurveThreshold, mDepth, mRatio, mAttack, mHold, mRelease, kMarks };
    int bands() const;             // 1 or 4
    double split(int k) const;     // Hz; split k lies between band k and band k + 1 (from 0)
    double edgeHz(int b) const;    // band b's lower edge: the scale's start, or the split under it
    double aim() const;            // LUFS: the level the threshold is relative to (the deck's working level)
    double gainAt(double level) const; // dB (<= 0): what the stage does, settled, to a band `level` LU from the aim
    // The legends' values as text: in the legend and behind the name at the line - the curve's (0 threshold, 1 depth,
    // 2 ratio) and the times' (0 hold, 1 release).
    std::wstring curveValue(int j) const;
    std::wstring timeValue(int j) const;
    double holdX(double ms) const; // the hold-and-release graph's axis: ms after the music stopped -> pixels
    void drawCurve(Gdiplus::Graphics& g, std::vector<Label>& all);
    void drawOpening(Gdiplus::Graphics& g, std::vector<Label>& all); // the attack's graph
    void drawClosing(Gdiplus::Graphics& g, std::vector<Label>& all); // the hold-and-release graph

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_;              // threshold, depth, ratio: in the bands panel's header, behind the number of bands
    Legend times_;               // hold, release: in the last panel's header
    RECT bandsItem_ = {};        // "Bands 4" with its steppers, in the first panel's header (its icon: icons_[0])
    int bandsHeaderBottom_ = 0;
    int bandsTitleW_ = 30, timesTitleW_ = 120, digitW_ = 8, hintW_ = 100, offW_ = 100;
    std::vector<Label> bandNames_; // the texts at the bands graph's lines, written after the layer
};

} // namespace ap
