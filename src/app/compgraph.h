// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's Compressor tab (client, 2026-11-13: "turn these sliders into a graphical view,
// so it is clearer what they do to the audio - maybe the live audio in the background"; "as compact as possible",
// "sizable"): four panels in one strip (graphstrip.h) that takes whatever size it is given -
//   the level of the last 4 s with the curve (in -> out) over it on the same vertical scale: the threshold, the
//     knee, the ratio and the make-up are level lines across both, the live working point is on the curve;
//   the attack: the first 150 ms of a hit 10 dB over the threshold - what still gets through;
//   the release: the gain on the rest of the music after that hit;
//   the side chain: the music's spectrum up to 1 kHz and what of it the compressor listens to (its high-pass).
// How it came to this, step by step, all on 2026-11-13 and all the client's words:
//   "the attack and release slider can go, make a small reset icon in the graph" - their graphs are where they are
//     set (the mark, the mouse wheel for one step), a small reset icon in the graph's corner;
//   "these three sliders are in the curve graph, they can go too; make-up: a horizontal line in the graph; ratio: a
//     horizontal line in the graph; give all three the same name in the graph and a reset icon" - dashed lines that
//     carry their names and are dragged anywhere along them, and a legend: name, value, reset icon;
//   "remove these two as well and integrate them in the graph; let the side chain graph run to 1 k instead of 10 k" -
//     the knee is the shaded band with a mark and a line in the legend; the side-chain high-pass has its graph. The
//     tab keeps the stage's switch alone;
//   "can the curve graph be integrated in the Level and reduction graph?" ... "not beside each other but in each
//     other" ... "build this one" - one panel, the legend in its header.
#pragma once

#include "graphstrip.h"

namespace ap {

class CompressorGraphs : public GraphStrip {
public:
    const char* stage() const override { return "Compressor"; }
    bool owns(int param) const override;

protected:
    int markCount() const override { return kMarks; }
    int markParam(int mark) const override;
    bool markUpDown(int mark) const override;
    void measure(HDC dc) override;
    void layout(int w, int h) override;
    void fillTips(std::vector<Tip>& out) override;
    void renderStill(HDC dc, int w, int h) override;
    void renderLayer(Gdiplus::Graphics& g, int w, int h) override; // the curve: still, but it lies over the level
    void paintLive(HDC dc) override;
    int lineAt(POINT pt) const override;
    bool dragValue(int mark, POINT pt, double& value) const override;
    int wheelParam(POINT pt) const override;
    double stepped(int param, int steps) const override;

private:
    static constexpr int kPanels = 4; // the level with the curve in it, attack, release, side chain
    enum Mark { mThreshold, mRatio, mMakeup, mAttack, mRelease, mSideHpf, mKnee, kMarks };
    // the compressor's numbers from values_
    double aim() const;
    double threshold() const; // LU to the aim
    double makeup() const;    // dB (the automatic one worked out)
    double out(double in) const; // LU to the aim -> LU to the aim, through the curve and the make-up
    double hitReduction() const; // dB the example hit (10 dB over the threshold) is turned down when settled

    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    Legend legend_;              // threshold, ratio, knee, make-up: in the level-and-curve panel's header
    int titleW_ = 120, numberW_ = 46; // the widest title of that panel and the reduction's number
    std::vector<Label> curveNames_;   // the names at the curve's lines, written after the layer
};

} // namespace ap
