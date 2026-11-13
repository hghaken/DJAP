// SPDX-License-Identifier: GPL-3.0-or-later
#include "compgraph.h"

#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

// the level-and-curve graph: LU to the level the threshold is relative to (the threshold goes up to +15)
const double kLo = -30.0, kHi = 18.0;
const double kRatioMarkLu = 1.0;  // the ratio's mark sits this far under the right end
const double kMakeupMarkLu = 5.0; // the make-up's mark this far above the left end (under any knee)
// the attack and release graphs: a hit this far over the threshold
const double kHitDb = 10.0, kAttackSpanMs = 160.0;
const double kReleaseFromMs = -100.0, kReleaseToMs = 1100.0;
const int kTimePoints = 400; // the level: 4 s of 10 ms points
// the side chain's graph: up to 1 kHz (client, 2026-11-13: "to 1 k instead of 10 k" - the high-pass ends at 200 Hz,
// so the bass it is about gets the width)
const double kSpecLoHz = 30.0, kSpecHiHz = 1000.0, kSpecRangeDb = 54.0, kFilterRangeDb = 36.0;

// The side chain's high-pass (2nd order, as design::highPass) in dB at `hz`; 0 when it is off.
double sideChainDb(double hz, double corner) {
    if (corner <= 0.0) return 0.0;
    const double r = hz / corner, r4 = r * r * r * r;
    return 10.0 * std::log10(r4 / (1.0 + r4) + 1e-12);
}

// the three graphs with a reset icon in their corner, and the legend's four
const int kCornerParam[3] = {pCompAttack, pCompRelease, pCompSideHpf};
const int kLegendParam[4] = {pCompThreshold, pCompRatio, pCompKnee, pCompMakeup};

} // namespace

bool CompressorGraphs::owns(int param) const {
    for (int p : kCornerParam)
        if (p == param) return true;
    for (int p : kLegendParam)
        if (p == param) return true;
    return false;
}

int CompressorGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pCompThreshold, pCompRatio, pCompMakeup, pCompAttack,
                                         pCompRelease,   pCompSideHpf, pCompKnee};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

// The level lines go up / down, the marks of the attack, the release and the high-pass left / right.
bool CompressorGraphs::markUpDown(int mark) const { return mark != mAttack && mark != mRelease && mark != mSideHpf; }

void CompressorGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[4] = {L"Threshold", L"Ratio", L"Knee", L"Make-up"};
    static const wchar_t* const kWidest[4] = {L"+15.0 LU", L"8.0:1", L"12.0 dB", L"12.0 dB"};
    legend_.count = 4;
    for (int j = 0; j < 4; ++j) {
        legend_.param[j] = kLegendParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"Level and curve (off)");
    numberW_ = textWidth(dc, L"-12.0 dB");
}

// The level graph with the curve in it wants the width: it lies across the top, with the attack, the release and
// the side chain in a row under it, the three equally wide (client, 2026-11-13: "the side chain graph can be
// narrower", then "make the attack, release and side chain windows equally wide"); all four side by side in a wide
// strip (layoutFour). The reset icons of the attack, the release and the side chain's high-pass stand in their
// panels' title line; the legend of the first panel - threshold, ratio, knee, make-up - in its header, off the graph.
void CompressorGraphs::layout(int w, int h) {
    layoutFour(w, h, panel_);
    for (int k = 0; k < 3; ++k) icons_.push_back({cornerIcon(panel_[1 + k]), kCornerParam[k]});
    layoutLegend(legend_, panel_[0], titleW_, numberW_);
}

// The rectangles do not overlap (the tooltip control keeps the tool the mouse is in): the level-and-curve panel is
// three around its legend (legendTips); a panel with an icon in its corner is two rectangles around the icon - what
// is left of it, what is under it; then the legend's items and the seven icons.
void CompressorGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kLevel =
        L"Behind: the last 4 seconds of this deck - the level at the compressor (grey: what its side chain hears), the "
        L"level it lets out (thin line) and, from the top, how much it turns down (orange; the number is the largest "
        L"reduction). In front: the compressor's curve on the same scale - the level going in (to the right) against "
        L"the level coming out (up), in LU to the target. Under the threshold it follows the dotted diagonal; above "
        L"it, it bends - flatter with a higher ratio, rounder with a wider knee. The white dot is the level now."
        L"\n\nFour values are set here, each a line across the graph: drag a line anywhere along it, or its mark, up "
        L"or down. Threshold: music above this line is compressed. Knee: the shaded band around it. Ratio: the level "
        L"the loudest music comes out at - lower is a higher ratio. Make-up: the quiet end of the curve, and the "
        L"whole curve with it; all the way down = auto. The legend names their values; the arrow behind a value sets "
        L"it back.";
    static const wchar_t* const kCorner[3] = {
        L"Attack (1 .. 100 ms): a hit 10 dB over the threshold in its first 150 ms. Dashed: the hit as it comes in; "
        L"the line: what comes out. What is above the line before the mark still gets through - the punch. Shorter "
        L"catches the hit sooner, longer lets it through.\n\nSet it here: drag the mark, or turn the mouse wheel over "
        L"this graph for one step of 1 ms. The arrow in the corner sets it back.",
        L"Release (20 .. 1000 ms): the gain on the rest of the music after that hit. It is still down when the hit "
        L"ends and comes back in the release time. Short: back before the next beat - lively, but the music goes up "
        L"and down with every beat (pumping). Long: smooth.\n\nSet it here: drag the mark, or turn the mouse wheel "
        L"over this graph for one step of 10 ms. The arrow in the corner sets it back.",
        L"Side-chain high-pass (off, 40 .. 200 Hz): the music's spectrum up to 1 kHz (grey) and the part of it the "
        L"compressor listens to (coloured) - what the high-pass leaves of the bass. Bass under its frequency does not "
        L"make the compressor work; a kick the compressor hears pulls the whole mix down with every beat (pumping)."
        L"\n\nSet it here: drag the mark left or right (all the way left = off), or turn the mouse wheel over this "
        L"graph for one step of 5 Hz. The arrow in the corner sets it back.",
    };
    legendTips(legend_, panel_[0], kLevel, out);
    for (int k = 0; k < 3; ++k) {
        const RECT& p = panel_[1 + k];
        const RECT& icon = icons_[static_cast<size_t>(k)].rect;
        out.push_back({RECT{p.left, p.top, icon.left, p.bottom}, kCorner[k], -1, -1});
        out.push_back({RECT{icon.left, icon.bottom, p.right, p.bottom}, kCorner[k], -1, -1});
    }
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

double CompressorGraphs::aim() const { return workingLevel(values_); }

double CompressorGraphs::threshold() const { return values_[pCompThreshold]; }

double CompressorGraphs::makeup() const {
    if (values_[pCompMakeup] >= 0.0) return values_[pCompMakeup];
    return 0.0 - compressorCurveDb(0.0, threshold(), values_[pCompRatio], values_[pCompKnee]); // the aim passes unchanged
}

double CompressorGraphs::out(double in) const {
    return compressorCurveDb(in, threshold(), values_[pCompRatio], values_[pCompKnee]) + makeup();
}

double CompressorGraphs::hitReduction() const {
    const double in = threshold() + kHitDb;
    return in - compressorCurveDb(in, threshold(), values_[pCompRatio], values_[pCompKnee]);
}

std::wstring CompressorGraphs::valueText(int j) const {
    switch (j) {
    case 0:
        return number(threshold(), 1, L" LU", true);
    case 1:
        return number(values_[pCompRatio], 1, L":1");
    case 2:
        return number(values_[pCompKnee], 1, L" dB");
    default:
        return values_[pCompMakeup] < 0.0 ? std::wstring(L"auto") : number(makeup(), 1, L" dB");
    }
}

// The panels, their titles, the legend's texts, the attack and release graphs.
void CompressorGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    static const wchar_t* const kTitle[kPanels] = {L"Level and curve", L"Attack", L"Release", L"Side-chain high-pass"};
    const bool on = values_[pCompOn] >= 0.5;
    const double reduce = hitReduction();
    const int th = textH_;
    std::vector<Label> all;
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(col(kBack));
        drawPanels(g, panel_, kPanels);
        for (int i = 0; i < kPanels; ++i) {
            const RECT& p = panel_[i];
            all.push_back({static_cast<int>(p.left) + px(6), static_cast<int>(p.top) + px(3),
                           i == 0 && !on ? std::wstring(L"Level and curve (off)") : kTitle[i], kDim, TA_LEFT});
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0 ? legend_.headerBottom + px(5) : p.top + th + px(7); // the first: under its legend
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        // ---- 1. the level and the curve: the legend in the panel's header (the graph itself is drawn with the
        // audio, in paintLive: the level lies behind the curve) ----
        plot_[0].x0 = plot_[0].y0 = kLo; // level in to the right, level out - and the level of the last seconds - up
        plot_[0].x1 = plot_[0].y1 = kHi;
        for (int j = 0; j < legend_.count; ++j) {
            const RECT& item = legend_.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, valueText(j), kOrange, TA_RIGHT});
        }
        // ---- 2 and 3. the attack and the release ----
        drawAttack(g, plot_[1], mAttack, kHitDb, reduce, values_[pCompAttack], kAttackSpanMs, 50.0, L"150 ms", all);
        drawRelease(g, plot_[2], mRelease, reduce, values_[pCompRelease], kReleaseFromMs, kReleaseToMs, 250.0, 1000.0,
                    L"1 s", all);
        // ---- 4. the side chain: its scale (drawn with the audio, in paintLive) ----
        plot_[3].logX = true;
        plot_[3].x0 = kSpecLoHz;
        plot_[3].x1 = kSpecHiHz;
    }
    labels(dc, all);
}

// The curve of the level-and-curve graph is still, yet it lies over the level, which moves: it goes into the
// strip's layer, drawn here when a value, the size or the DPI changes and laid over the level at every refresh
// (paintLive). Drawn with the level at every refresh instead, its long dashed and dotted anti-aliased lines took a
// quarter of a processor core.
void CompressorGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    curveNames_.clear();
    const double thr = threshold(), mk = makeup();
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    // The curve, over the level and on its vertical scale - so the four values set here are level lines across
    // both: the threshold (music above it is compressed), the knee's band around it, the ratio through the curve's
    // loud end (where the loudest music comes out), the make-up through its quiet end (which it lifts, and the
    // whole curve with it).
    const Plot& q = plot_[0];
    const double knee = values_[pCompKnee], ratioIn = kHi - kRatioMarkLu, makeupIn = kLo + kMakeupMarkLu;
    auto row = [](double y) { return std::floor(y) + 0.5; }; // a level line on a pixel row: sharp
    const double yThr = row(q.Y(thr)), yRatio = row(q.Y(out(ratioIn))), yMakeup = row(q.Y(out(makeupIn)));
    const double yKneeT = row(q.Y(thr + knee / 2)), yKneeB = row(q.Y(thr - knee / 2));
    const double xThr = q.X(thr), xRatio = q.X(ratioIn), xMakeup = q.X(makeupIn), xKnee = q.l + px(7);
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    Pen dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 3.0f * lw);
    dots.SetDashStyle(DashStyleDot);
    dash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    g.DrawLine(&dots, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0)))); // the target's level
    if (knee > 0.0) {
        SolidBrush band(col(kOrange, 40));
        g.FillRectangle(&band, static_cast<REAL>(q.l), static_cast<REAL>(yKneeT), static_cast<REAL>(q.r - q.l),
                        static_cast<REAL>(yKneeB - yKneeT));
        Pen kneeEdge(col(kOrange, 150), lw);
        kneeEdge.SetDashStyle(DashStyleDot);
        g.DrawLine(&kneeEdge, pt(q.l, yKneeT), pt(q.r, yKneeT));
        g.DrawLine(&kneeEdge, pt(q.l, yKneeB), pt(q.r, yKneeB));
    }
    g.DrawLine(&dots, pt(q.X(kLo), q.Y(kLo + mk)), pt(q.X(kHi - mk), q.Y(kHi))); // no compression
    g.DrawLine(&dash, pt(q.l, yThr), pt(q.r, yThr));
    g.DrawLine(&dash, pt(q.l, yRatio), pt(q.r, yRatio));
    g.DrawLine(&dash, pt(q.l, yMakeup), pt(q.r, yMakeup));
    std::vector<PointF> line;
    for (int i = 0; i <= 96; ++i) {
        const double in = kLo + (kHi - kLo) * i / 96.0;
        line.push_back(pt(q.X(in), q.Y(out(in))));
    }
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    g.ResetClip();
    // The marks: the threshold's where its line crosses the diagonal - the level at which the curve bends -, the
    // ratio's and the make-up's on the curve's ends, the knee's on the band's upper edge at the left (on the
    // threshold's line at a knee of 0, where it can still be taken).
    drawMark(g, mThreshold, xThr, yThr);
    drawMark(g, mRatio, xRatio, yRatio);
    drawMark(g, mMakeup, xMakeup, yMakeup);
    drawMark(g, mKnee, xKnee, yKneeT);
    // Each name stands at its line beside its mark: the threshold's and the ratio's left of their marks, the
    // make-up's and the knee's right of theirs (client, 2026-11-13: "ratio text left of the mark, threshold text
    // left of the mark, make-up text right of the mark"). On the line, where the curve is not: left of a mark the
    // curve lies under the line, right of the make-up's mark it rises over it - that name hangs under its line. A
    // name that would leave the plot at the top goes under its line too, and so does the threshold's where the
    // ratio's name is in its way. Each name has its value behind it (client, 2026-11-13: "put the set values behind
    // the text"); the knee's, which stands only half a knee over the threshold's, goes over that one where the two
    // would meet.
    std::wstring name[4]; // the legend's order: threshold, ratio, knee, make-up
    int nameW[4];
    for (int j = 0; j < 4; ++j) {
        name[j] = std::wstring(legend_.name[j]) + L" " + valueText(j);
        nameW[j] = textWidth(name[j]);
    }
    const double above = th + px(2), under = px(2), side = px(8);
    const double yRatioName = yRatio - above < q.t ? yRatio + under : yRatio - above;
    double yThrName = yThr - above;
    if (xRatio - side - nameW[1] < xThr - side && std::fabs(yRatioName - yThrName) < th) yThrName = yThr + under;
    double yKneeName = yKneeT - above < q.t ? yKneeT + under : yKneeT - above;
    const RECT thrRect{static_cast<LONG>(xThr - side - nameW[0]), static_cast<LONG>(yThrName), static_cast<LONG>(xThr - side),
                       static_cast<LONG>(yThrName + th)};
    const RECT kneeRect{static_cast<LONG>(xKnee + side), static_cast<LONG>(yKneeName),
                        static_cast<LONG>(xKnee + side + nameW[2]), static_cast<LONG>(yKneeName + th)};
    RECT both;
    if (IntersectRect(&both, &thrRect, &kneeRect)) {
        yKneeName = thrRect.top - th;
        if (yKneeName < q.t) yKneeName = thrRect.bottom;
    }
    curveNames_.push_back({static_cast<int>(xThr - side), static_cast<int>(yThrName), name[0], kOrange, TA_RIGHT});
    curveNames_.push_back({static_cast<int>(xRatio - side), static_cast<int>(yRatioName), name[1], kOrange, TA_RIGHT});
    curveNames_.push_back({static_cast<int>(xMakeup + side), static_cast<int>(yMakeup + under), name[3], kOrange, TA_LEFT});
    curveNames_.push_back({static_cast<int>(xKnee + side), static_cast<int>(yKneeName), name[2], kOrange, TA_LEFT});
}

// The parts that move with the audio, on a copy of the still picture: the level and the reduction of the last 4 s
// with the curve over them, and the side chain's spectrum with its filter.
void CompressorGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    const bool have = an && playing && an->compCount > 0;
    const double mk = makeup(), corner = values_[pCompSideHpf], level0 = aim();
    const int hair = std::max(1, px(1)), th = textH_;
    const COLORREF inputArea = blend(kInput, kPanel, 0.28), heardArea = blend(kTeal, inputArea, 0.32);

    // ---- 1. the level and the reduction of the last 4 s: the background of the curve, which is laid over it
    // below. Dimmer than the curve; time runs over the plot's whole width, the newest at the right edge. ----
    const Plot& qc = plot_[0];
    double most = 0.0, nowIn = kLo - 1.0; // the largest reduction in view; the newest level (LU to the aim)
    {
        const int saved = clip(dc, qc);
        if (have) {
            std::vector<POINT> in, outLine, red;
            in.push_back(ipt(qc.l, qc.b));
            red.push_back(ipt(qc.l, qc.t));
            const double perDb = 0.4 * (qc.b - qc.t) / 12.0, step = (qc.r - qc.l) / (kTimePoints - 1.0);
            for (int i = 0; i < kTimePoints; ++i) {
                const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - kTimePoints + i);
                const double lu = std::max(kLo, c.in - level0), x = qc.l + i * step;
                in.push_back(ipt(x, qc.Y(lu)));
                outLine.push_back(ipt(x, qc.Y(c.in - level0 < kLo ? kLo : lu - c.reduction + mk)));
                red.push_back(ipt(x, qc.t + std::min(12.0, static_cast<double>(c.reduction)) * perDb));
                most = std::max(most, static_cast<double>(c.reduction));
            }
            in.push_back(ipt(qc.r, qc.b));
            red.push_back(ipt(qc.r, qc.t));
            polygon(dc, in, blend(kInput, kPanel, 0.2));
            polygon(dc, red, blend(kOrange, kPanel, 0.5));
            polyline(dc, outLine, blend(kTeal, kPanel, 0.55), std::max(1, px(1.5)));
            nowIn = an->compAt(DeckAnalysis::kComp - 1).in - level0;
        }
        RestoreDC(dc, saved);
    }
    // The curve's layer over the level, then a thin line from the newest end of the level - the right edge -
    // across to where that level is on the curve.
    blendLayer(dc, RECT{static_cast<LONG>(qc.l) - px(9), static_cast<LONG>(qc.t) - px(9), static_cast<LONG>(qc.r) + px(9),
                        static_cast<LONG>(qc.b) + px(9)});
    if (have && nowIn >= kLo) {
        const int saved = clip(dc, qc);
        polyline(dc, {ipt(qc.X(nowIn), qc.Y(nowIn)), ipt(qc.r, qc.Y(nowIn))}, blend(kText, kPanel, 0.5), 1, PS_DOT);
        RestoreDC(dc, saved);
    }

    // ---- 4. the side chain: the spectrum (GDI), then its filter and mark (GDI+, below) ----
    Plot& qs = plot_[3];
    auto filterY = [&](double db) { return qs.t + std::min(1.0, std::max(0.0, -db / kFilterRangeDb)) * (qs.b - qs.t); };
    const double xCorner = corner > 0.0 ? qs.X(corner) : qs.l;
    {
        const int saved = clip(dc, qs);
        if (an && playing && an->specIn.frames() > 0) {
            const auto& points = an->specIn.points();
            double top = -70.0;
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i);
                if (hz >= kSpecLoHz && hz <= kSpecHiHz) top = std::max(top, static_cast<double>(points[static_cast<size_t>(i)]));
            }
            qs.y1 = top + 3.0;
            qs.y0 = qs.y1 - kSpecRangeDb;
            std::vector<POINT> raw, heard, heardLine;
            raw.push_back(ipt(qs.l, qs.b));
            heard.push_back(ipt(qs.l, qs.b));
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i);
                if (hz < kSpecLoHz || hz > kSpecHiHz) continue;
                const double db = points[static_cast<size_t>(i)];
                raw.push_back(ipt(qs.X(hz), qs.Y(db)));
                heardLine.push_back(ipt(qs.X(hz), qs.Y(db + sideChainDb(hz, corner))));
                heard.push_back(heardLine.back());
            }
            raw.push_back(ipt(qs.r, qs.b));
            heard.push_back(ipt(qs.r, qs.b));
            polygon(dc, raw, inputArea);
            polygon(dc, heard, heardArea);
            if (heardLine.size() > 1) polyline(dc, heardLine, kTeal, hair);
        }
        for (double hz : {50.0, 100.0, 200.0, 500.0}) polyline(dc, {ipt(qs.X(hz), qs.t), ipt(qs.X(hz), qs.b)}, kGrid, 1, PS_DOT);
        if (corner > 0.0) polyline(dc, {ipt(xCorner, qs.t), ipt(xCorner, qs.b)}, kOrange, 1, PS_DASH);
        RestoreDC(dc, saved);
    }

    {
        using namespace Gdiplus;
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
        // the side chain's filter and its mark
        std::vector<PointF> filter;
        for (int i = 0; i <= 80; ++i) {
            const double hz = kSpecLoHz * std::pow(kSpecHiHz / kSpecLoHz, i / 80.0);
            filter.push_back(pt(qs.X(hz), filterY(sideChainDb(hz, corner))));
        }
        Pen filterPen(col(kOrange), 2.0f * lw);
        g.DrawLines(&filterPen, filter.data(), static_cast<INT>(filter.size()));
        drawMark(g, mSideHpf, xCorner, filterY(corner > 0.0 ? -3.0 : 0.0));
        // where the level is now, on the curve (which lies over the level: blended in above): the last half second
        // as a trail, the white dot, and the reduction of this moment
        if (have && nowIn >= kLo) {
            const Plot& q = qc;
            g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                            static_cast<REAL>(q.b - q.t + 2)));
            const int n = 50; // 0.5 s
            for (int k = 0; k < n; ++k) {
                const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - n + k);
                const double in = c.in - level0;
                if (in < kLo) continue;
                const double x = q.X(in), y = q.Y(in - c.reduction + mk);
                if (k == n - 1) {
                    if (c.reduction > 0.2) {
                        Pen now(col(kOrange), 3.0f * lw);
                        g.DrawLine(&now, pt(x, q.Y(in + mk)), pt(x, y));
                    }
                    SolidBrush white(col(RGB(250, 250, 250)));
                    const double r = px(35) / 10.0;
                    g.FillEllipse(&white, static_cast<REAL>(x - r), static_cast<REAL>(y - r), static_cast<REAL>(2 * r),
                                  static_cast<REAL>(2 * r));
                } else {
                    SolidBrush faint(col(RGB(250, 250, 250), 20 + 120 * k / n));
                    const double r = px(15) / 10.0;
                    g.FillEllipse(&faint, static_cast<REAL>(x - r), static_cast<REAL>(y - r), static_cast<REAL>(2 * r),
                                  static_cast<REAL>(2 * r));
                }
            }
            g.ResetClip();
        }
    }
    labels(dc, curveNames_);
    if (have) text(dc, panel_[0].right - px(6), panel_[0].top + px(3), number(-most, 1, L" dB"), kOrange, TA_RIGHT);
    else text(dc, (qc.l + qc.r) / 2, qc.b - th - px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
    text(dc, qs.X(100.0) + px(3), qs.b - th - px(2), L"100", kText); // over the spectrum's filled area: the light colour
    text(dc, qs.r - px(3), qs.b - th - px(2), L"1 k", kText, TA_RIGHT);
    text(dc, xCorner + px(8), filterY(corner > 0.0 ? -3.0 : 0.0) + px(6),
         corner > 0.0 ? number(corner, 0, L" Hz") : std::wstring(L"off"), kOrange);
}

// The level lines of the level-and-curve graph - the threshold's, the ratio's, the make-up's, and the two edges of
// the knee's band - are taken anywhere along them, not only by their marks: the nearest one within a few pixels (in
// that order where they lie together; a knee of 0 is taken by its mark).
int CompressorGraphs::lineAt(POINT p) const {
    const Plot& q = plot_[0];
    if (p.x < q.l - 2 || p.x > q.r + 2 || p.y < q.t - 2 || p.y > q.b + 2) return -1;
    const long reach = px(5);
    const double half = values_[pCompKnee] / 2.0;
    const long kneeT = std::lround(q.Y(threshold() + half)), kneeB = std::lround(q.Y(threshold() - half));
    const long d[4] = {std::labs(p.y - mark_[mThreshold].y), std::labs(p.y - mark_[mRatio].y),
                       std::labs(p.y - mark_[mMakeup].y),
                       half > 0.0 ? std::min(std::labs(p.y - kneeT), std::labs(p.y - kneeB)) : reach + 1};
    static const int kLine[4] = {mThreshold, mRatio, mMakeup, mKnee};
    int best = -1;
    long bestD = reach + 1;
    for (int i = 0; i < 4; ++i) {
        if (d[i] < bestD) {
            bestD = d[i];
            best = kLine[i];
        }
    }
    return best;
}

// Where the wheel steps a value: anywhere over the attack's, the release's or the side chain's graph, and over an
// item of the legend (over the level-and-curve graph itself it could be any of four).
int CompressorGraphs::wheelParam(POINT p) const {
    for (int k = 0; k < 3; ++k)
        if (PtInRect(&panel_[1 + k], p)) return kCornerParam[k];
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return legend_.param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    return -1;
}

// One wheel notch is one step of the value. The attack and the release go by the parameter's own step (1 ms, 10 ms),
// not by a slider position: on their logarithmic scales one position often is the same value, and at the long end a
// position skips values. The others are on linear scales, where a position is a step (and the make-up's first
// position is "auto").
double CompressorGraphs::stepped(int param, int steps) const {
    if (param != pCompAttack && param != pCompRelease) return GraphStrip::stepped(param, steps);
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
}

bool CompressorGraphs::dragValue(int mark, POINT p, double& v) const {
    switch (mark) {
    case mThreshold: // a level line: up / down
        v = plot_[0].fromY(p.y);
        return true;
    case mRatio: { // the ratio that brings the curve's right end to the mouse (the curve falls as the ratio rises)
        const double want = plot_[0].fromY(p.y), x = kHi - kRatioMarkLu;
        const ParamInfo& in = paramInfo(pCompRatio);
        PresetValues t = values_;
        double lo = in.min, hi = in.max;
        for (int i = 0; i < 24; ++i) {
            t[pCompRatio] = 0.5 * (lo + hi);
            const double mkT = t[pCompMakeup] >= 0.0 ? t[pCompMakeup] : -compressorCurveDb(0.0, t[pCompThreshold], t[pCompRatio], t[pCompKnee]);
            const double y = compressorCurveDb(x, t[pCompThreshold], t[pCompRatio], t[pCompKnee]) + mkT;
            if (y > want) lo = t[pCompRatio];
            else hi = t[pCompRatio];
        }
        v = 0.5 * (lo + hi);
        return true;
    }
    case mMakeup: {
        const double mk = plot_[0].fromY(p.y) - (kLo + kMakeupMarkLu);
        v = mk < -0.3 ? -1.0 : std::max(0.0, mk); // under 0: auto
        return true;
    }
    case mKnee: // the band is as wide on both sides of the threshold: either edge (and the mark) sets half of it
        v = 2.0 * std::fabs(plot_[0].fromY(p.y) - threshold());
        return true;
    case mAttack:
        v = plot_[1].fromX(p.x) - kHitAtMs;
        return true;
    case mRelease:
        v = plot_[2].fromX(p.x);
        return true;
    case mSideHpf: {
        const double hz = plot_[3].fromX(p.x);
        v = hz < 34.0 ? 0.0 : std::max(40.0, hz); // left of the lowest corner: off
        return true;
    }
    default:
        return false;
    }
}

} // namespace ap
