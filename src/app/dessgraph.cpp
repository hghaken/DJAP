// SPDX-License-Identifier: GPL-3.0-or-later
#include "dessgraph.h"

#include "dsp.h"
#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

// the high band's graph: LU to the level the threshold is relative to (the threshold goes from -30 to 0)
const double kLo = -42.0, kHi = 6.0;
// the reduction curve: dB over the threshold to the right, the reduction down
const double kOverLo = -6.0, kOverHi = 24.0, kRedMax = 13.0;
const int kTimePoints = 400; // 4 s of 10 ms points
// the frequency graph
const double kSpecLoHz = 1000.0, kSpecHiHz = 20000.0, kSpecRangeDb = 54.0, kFilterRangeDb = 15.0;
const double kFs = 48000.0; // the filters' responses are drawn at this rate (the shape hardly changes with it)

// how much the de-esser turns down at `over` dB over its threshold (deck.cpp: 4:1, at most `max`)
double reductionAt(double over, double max) { return std::min(max, std::max(0.0, 0.75 * over)); }

// the detector's high-pass (two 2nd-order sections at the frequency) and the shelf, in dB at `hz`
double detectorDb(double corner, double hz) { return 2.0 * design::magnitudeDb(design::highPass(kFs, corner), kFs, hz); }
double shelfDb(double corner, double reduce, double hz) {
    return reduce <= 0.0 ? 0.0 : design::magnitudeDb(design::highShelf(kFs, corner, -reduce), kFs, hz);
}

const int kLegendParam[3] = {pDessThreshold, pDessMax, pDessFreq};

} // namespace

bool DeEsserGraphs::owns(int param) const {
    for (int p : kLegendParam)
        if (p == param) return true;
    return false;
}

int DeEsserGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pDessThreshold, pDessMax, pDessFreq};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

void DeEsserGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[3] = {L"Threshold", L"Maximum", L"Frequency"};
    static const wchar_t* const kWidest[3] = {L"-30 LU", L"12.0 dB", L"10.0 kHz"};
    legend_.count = 3;
    for (int j = 0; j < 3; ++j) {
        legend_.param[j] = kLegendParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"High band and reduction (off)");
    numberW_ = textWidth(dc, L"-12.0 dB");
}

// The high band across the top with the reduction curve and the frequency under it, or all three side by side
// (layoutThree); the legend of the three values in the high band panel's header.
void DeEsserGraphs::layout(int w, int h) {
    layoutThree(w, h, panel_);
    layoutLegend(legend_, panel_[0], titleW_, numberW_);
}

// The high band panel is three rectangles around its legend, the other two one each; then the legend's items and
// their icons.
void DeEsserGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kLevel =
        L"Behind: the last 4 seconds of this deck - the level of its highs above the de-esser's frequency, as its "
        L"detector hears them (grey, in LU to the target), and, from the top, how much it turns the highs down "
        L"(orange; the number is the largest reduction). Sharp 's' and 'sh' sounds are the peaks of the grey."
        L"\n\nThreshold: the dashed line - highs above it are turned down. Drag the line or its mark up or down, or "
        L"turn the mouse wheel over its name in the legend for one step of 1 LU. The legend names the three values; "
        L"the arrow behind a value sets it back.";
    static const wchar_t* const kCurve =
        L"How much the highs are turned down against how far they are over the threshold: 1 dB for every 1.33 dB "
        L"over it (4:1), never more than the maximum. The white dot is now, the faint ones the last half second."
        L"\n\nMaximum: the dashed line - drag it or its mark up or down, or turn the mouse wheel over this graph for "
        L"one step of 0.5 dB.";
    static const wchar_t* const kFreq =
        L"Frequency (3 .. 10 kHz): the music's spectrum from 1 kHz (grey), the part of it the de-esser listens to "
        L"(coloured: what its high-pass leaves above the frequency) and the high shelf it turns the highs down with "
        L"- dashed at the maximum, solid now.\n\nDrag the mark left or right, or turn the mouse wheel over this graph "
        L"for one step of 100 Hz.";
    legendTips(legend_, panel_[0], kLevel, out);
    out.push_back({panel_[1], kCurve, -1, -1});
    out.push_back({panel_[2], kFreq, -1, -1});
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

double DeEsserGraphs::aim() const { return workingLevel(values_); }

std::wstring DeEsserGraphs::valueText(int j) const {
    switch (j) {
    case 0:
        return number(values_[pDessThreshold], 0, L" LU", true);
    case 1:
        return number(values_[pDessMax], 1, L" dB");
    default:
        return number(values_[pDessFreq] / 1000.0, 1, L" kHz");
    }
}

// The panels, their titles, the legend's texts, and the reduction curve with its maximum.
void DeEsserGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    static const wchar_t* const kTitle[kPanels] = {L"High band and reduction", L"Reduction", L"Frequency"};
    const bool on = values_[pDessOn] >= 0.5;
    const double max = values_[pDessMax];
    const int th = textH_;
    std::vector<Label> all;
    {
        using namespace Gdiplus;
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(col(kBack));
        drawPanels(g, panel_, kPanels);
        for (int i = 0; i < kPanels; ++i) {
            const RECT& p = panel_[i];
            all.push_back({static_cast<int>(p.left) + px(6), static_cast<int>(p.top) + px(3),
                           i == 0 && !on ? std::wstring(L"High band and reduction (off)") : kTitle[i], kDim, TA_LEFT});
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0 ? legend_.headerBottom + px(5) : p.top + th + px(7);
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        plot_[0].x0 = 0.0;
        plot_[0].x1 = 1.0;
        plot_[0].y0 = kLo;
        plot_[0].y1 = kHi;
        for (int j = 0; j < legend_.count; ++j) {
            const RECT& item = legend_.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, valueText(j), kOrange, TA_RIGHT});
        }
        // ---- 2. the reduction against the level over the threshold ----
        Plot& q = plot_[1];
        q.x0 = kOverLo;
        q.x1 = kOverHi;
        q.y0 = -kRedMax;
        q.y1 = 0.8;
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
        Pen grid(col(kGrid), lw), dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw);
        dots.SetDashStyle(DashStyleDot);
        dash.SetDashStyle(DashStyleDash);
        curve.SetLineJoin(LineJoinRound);
        auto row = [](double y) { return std::floor(y) + 0.5; };
        g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                        static_cast<REAL>(q.b - q.t + 2)));
        g.DrawLine(&grid, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
        g.DrawLine(&dots, pt(row(q.X(0)), q.t), pt(row(q.X(0)), q.b)); // the threshold
        for (double over = 6.0; over < kOverHi; over += 6.0)
            g.DrawLine(&grid, pt(row(q.X(over)), q.b - px(4)), pt(row(q.X(over)), q.b));
        const double yMax = row(q.Y(-max));
        g.DrawLine(&dash, pt(q.l, yMax), pt(q.r, yMax));
        SolidBrush fill(col(kOrange, 50));
        std::vector<PointF> line, area;
        area.push_back(pt(q.l, q.Y(0)));
        for (int i = 0; i <= 120; ++i) {
            const double over = kOverLo + (kOverHi - kOverLo) * i / 120.0;
            line.push_back(pt(q.X(over), q.Y(-reductionAt(over, max))));
            area.push_back(line.back());
        }
        area.push_back(pt(q.r, q.Y(0)));
        g.FillPolygon(&fill, area.data(), static_cast<INT>(area.size()));
        g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
        g.ResetClip();
        const double xMax = q.r - px(14);
        drawMark(g, mMax, xMax, yMax);
        const std::wstring name = L"Maximum " + valueText(1);
        const bool under = yMax - th - px(2) < q.t;
        all.push_back({static_cast<int>(xMax) - px(9), static_cast<int>(under ? yMax + px(2) : yMax - th - px(2)), name,
                       kOrange, TA_RIGHT});
        all.push_back({static_cast<int>(q.X(0)) + px(4), static_cast<int>(q.b) - th - px(4), L"threshold", kDim, TA_LEFT});
        all.push_back({static_cast<int>(q.r), static_cast<int>(q.b) - th - px(4), L"+24 dB over", kDim, TA_RIGHT});
        // ---- 3. the frequency: its scale (the spectrum is drawn with the audio, the filters in the layer) ----
        plot_[2].logX = true;
        plot_[2].x0 = kSpecLoHz;
        plot_[2].x1 = kSpecHiHz;
        plot_[2].y0 = -kFilterRangeDb;
        plot_[2].y1 = 0.0;
    }
    labels(dc, all);
}

// Still things over moving ones: the threshold's line over the high band, and the detector's and the shelf's curves
// over the spectrum.
void DeEsserGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    layerNames_.clear();
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    // ---- 1. the threshold ----
    {
        const Plot& q = plot_[0];
        const double y = row(q.Y(values_[pDessThreshold])), x = q.r - px(14);
        Pen dash(col(kOrange), lw), dots(col(kGrid), lw);
        dash.SetDashStyle(DashStyleDash);
        dots.SetDashStyle(DashStyleDot);
        g.DrawLine(&dots, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0)))); // the target's level
        g.DrawLine(&dash, pt(q.l, y), pt(q.r, y));
        drawMark(g, mThreshold, x, y);
        const bool under = y - th - px(2) < q.t;
        layerNames_.push_back({static_cast<int>(x) - px(9), static_cast<int>(under ? y + px(2) : y - th - px(2)),
                               L"Threshold " + valueText(0), kOrange, TA_RIGHT});
    }
    // ---- 3. the detector's high-pass and the shelf at the maximum ----
    {
        const Plot& q = plot_[2];
        const double corner = values_[pDessFreq], max = values_[pDessMax];
        auto filterY = [&](double db) { return q.t + std::min(1.0, std::max(0.0, -db / kFilterRangeDb)) * (q.b - q.t); };
        std::vector<PointF> det, shelf;
        for (int i = 0; i <= 80; ++i) {
            const double hz = kSpecLoHz * std::pow(kSpecHiHz / kSpecLoHz, i / 80.0);
            det.push_back(pt(q.X(hz), filterY(detectorDb(corner, hz))));
            shelf.push_back(pt(q.X(hz), filterY(shelfDb(corner, max, hz))));
        }
        g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                        static_cast<REAL>(q.b - q.t + 2)));
        Pen detPen(col(kTeal, 170), lw), shelfPen(col(kOrange), 1.5f * lw), upright(col(kOrange, 150), lw);
        shelfPen.SetDashStyle(DashStyleDash);
        upright.SetDashStyle(DashStyleDot);
        g.DrawLines(&detPen, det.data(), static_cast<INT>(det.size()));
        g.DrawLines(&shelfPen, shelf.data(), static_cast<INT>(shelf.size()));
        g.DrawLine(&upright, pt(row(q.X(corner)), q.t), pt(row(q.X(corner)), q.b));
        g.ResetClip();
        const double yMark = filterY(shelfDb(corner, max, corner));
        drawMark(g, mFrequency, q.X(corner), yMark);
        const bool right = q.X(corner) > q.l + 0.6 * (q.r - q.l);
        layerNames_.push_back({static_cast<int>(q.X(corner) + (right ? -px(9) : px(9))), static_cast<int>(yMark + px(6)),
                               valueText(2), kOrange, static_cast<UINT>(right ? TA_RIGHT : TA_LEFT)});
    }
}

// What moves with the audio: the high band and the reduction of the last 4 s, the working point on the reduction
// curve, the spectrum and the shelf now.
void DeEsserGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    const bool have = an && playing && an->compCount > 0;
    const double level0 = aim(), thr = values_[pDessThreshold], max = values_[pDessMax], corner = values_[pDessFreq];
    const int th = textH_;
    const COLORREF inputArea = blend(kInput, kPanel, 0.28), heardArea = blend(kTeal, inputArea, 0.32);

    // ---- 1. the high band and the reduction ----
    const Plot& q0 = plot_[0];
    double most = 0.0, nowRed = 0.0;
    {
        const int saved = clip(dc, q0);
        if (have) {
            std::vector<POINT> in, red;
            in.push_back(ipt(q0.l, q0.b));
            red.push_back(ipt(q0.l, q0.t));
            const double perDb = 0.4 * (q0.b - q0.t) / 12.0, step = (q0.r - q0.l) / (kTimePoints - 1.0);
            for (int i = 0; i < kTimePoints; ++i) {
                const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - kTimePoints + i);
                const double x = q0.l + i * step;
                in.push_back(ipt(x, q0.Y(std::max(kLo, c.dessIn - level0))));
                red.push_back(ipt(x, q0.t + std::min(12.0, static_cast<double>(c.dessReduction)) * perDb));
                most = std::max(most, static_cast<double>(c.dessReduction));
            }
            in.push_back(ipt(q0.r, q0.b));
            red.push_back(ipt(q0.r, q0.t));
            polygon(dc, in, blend(kInput, kPanel, 0.3));
            polygon(dc, red, blend(kOrange, kPanel, 0.5));
            const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - 1);
            nowRed = c.dessReduction;
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q0.l) - px(9), static_cast<LONG>(q0.t) - px(9), static_cast<LONG>(q0.r) + px(9),
                        static_cast<LONG>(q0.b) + px(9)});

    // ---- 3. the spectrum (GDI), the layer's filters over it, the shelf now ----
    Plot& qs = plot_[2];
    {
        const int saved = clip(dc, qs);
        if (an && playing && an->specIn.frames() > 0) {
            const auto& points = an->specIn.points();
            double top = -70.0;
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i);
                if (hz >= kSpecLoHz && hz <= kSpecHiHz) top = std::max(top, static_cast<double>(points[static_cast<size_t>(i)]));
            }
            const double y1 = top + 3.0, y0 = y1 - kSpecRangeDb;
            auto sy = [&](double db) { return qs.b - std::min(1.0, std::max(0.0, (db - y0) / (y1 - y0))) * (qs.b - qs.t); };
            std::vector<POINT> raw, heard;
            raw.push_back(ipt(qs.l, qs.b));
            heard.push_back(ipt(qs.l, qs.b));
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i);
                if (hz < kSpecLoHz || hz > kSpecHiHz) continue;
                const double db = points[static_cast<size_t>(i)];
                raw.push_back(ipt(qs.X(hz), sy(db)));
                heard.push_back(ipt(qs.X(hz), sy(db + detectorDb(corner, hz))));
            }
            raw.push_back(ipt(qs.r, qs.b));
            heard.push_back(ipt(qs.r, qs.b));
            polygon(dc, raw, inputArea);
            polygon(dc, heard, heardArea);
        }
        for (double hz : {2000.0, 5000.0, 10000.0}) polyline(dc, {ipt(qs.X(hz), qs.t), ipt(qs.X(hz), qs.b)}, kGrid, 1, PS_DOT);
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(qs.l) - px(9), static_cast<LONG>(qs.t) - px(9), static_cast<LONG>(qs.r) + px(9),
                        static_cast<LONG>(qs.b) + px(9)});
    if (have && nowRed > 0.05) {
        const int saved = clip(dc, qs);
        std::vector<POINT> now;
        for (int i = 0; i <= 60; ++i) {
            const double hz = kSpecLoHz * std::pow(kSpecHiHz / kSpecLoHz, i / 60.0);
            const double db = shelfDb(corner, nowRed, hz);
            now.push_back(ipt(qs.X(hz), qs.t + std::min(1.0, -db / kFilterRangeDb) * (qs.b - qs.t)));
        }
        polyline(dc, now, kOrange, std::max(1, px(2)));
        RestoreDC(dc, saved);
    }

    // ---- 2. the working point on the reduction curve: the last half second ----
    if (have) {
        using namespace Gdiplus;
        const Plot& q = plot_[1];
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                        static_cast<REAL>(q.b - q.t + 2)));
        const int n = 50;
        for (int k = 0; k < n; ++k) {
            const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - n + k);
            const double over = c.dessIn - level0 - thr;
            if (over < kOverLo) continue;
            const double x = q.X(over), y = q.Y(-reductionAt(over, max));
            const bool newest = k == n - 1;
            SolidBrush dot(col(RGB(250, 250, 250), newest ? 255 : 20 + 120 * k / n));
            const double r = (newest ? px(35) : px(15)) / 10.0;
            g.FillEllipse(&dot, static_cast<REAL>(x - r), static_cast<REAL>(y - r), static_cast<REAL>(2 * r),
                          static_cast<REAL>(2 * r));
        }
        g.ResetClip();
    }
    labels(dc, layerNames_);
    if (have) text(dc, panel_[0].right - px(6), panel_[0].top + px(3), number(most > 0.05 ? -most : 0.0, 1, L" dB"), kOrange, TA_RIGHT);
    else text(dc, (q0.l + q0.r) / 2, q0.b - th - px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
    text(dc, qs.X(2000.0) + px(3), qs.b - th - px(2), L"2 k", kText);
    text(dc, qs.X(10000.0) + px(3), qs.b - th - px(2), L"10 k", kText);
}

// The threshold's line is taken anywhere along it, and so is the maximum's in the reduction graph.
int DeEsserGraphs::lineAt(POINT p) const {
    const long reach = px(5);
    const Plot& q0 = plot_[0];
    if (p.x >= q0.l - 2 && p.x <= q0.r + 2 && p.y >= q0.t - 2 && p.y <= q0.b + 2 && std::labs(p.y - mark_[mThreshold].y) <= reach)
        return mThreshold;
    const Plot& q1 = plot_[1];
    if (p.x >= q1.l - 2 && p.x <= q1.r + 2 && p.y >= q1.t - 2 && p.y <= q1.b + 2 && std::labs(p.y - mark_[mMax].y) <= reach)
        return mMax;
    return -1;
}

// The wheel: over the reduction graph the maximum, over the frequency graph the frequency, over a legend item or
// icon its value.
int DeEsserGraphs::wheelParam(POINT p) const {
    if (PtInRect(&panel_[1], p)) return pDessMax;
    if (PtInRect(&panel_[2], p)) return pDessFreq;
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return legend_.param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    return -1;
}

bool DeEsserGraphs::dragValue(int mark, POINT p, double& v) const {
    switch (mark) {
    case mThreshold:
        v = plot_[0].fromY(p.y);
        return true;
    case mMax:
        v = -plot_[1].fromY(p.y);
        return true;
    case mFrequency:
        v = plot_[2].fromX(p.x);
        return true;
    default:
        return false;
    }
}

} // namespace ap
