// SPDX-License-Identifier: GPL-3.0-or-later
#include "widthgraph.h"

#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

const double kLoHz = 20.0, kHiHz = 20000.0, kTopPct = 200.0, kSpecRangeDb = 54.0;
const int kLegendParam[3] = {pWidth, pBassWidth, pWidthSplit};
const COLORREF kInLine = RGB(160, 166, 174);

} // namespace

int WidthGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pWidth, pBassWidth, pWidthSplit};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

void WidthGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[3] = {L"Width", L"Bass width", L"Split"};
    static const wchar_t* const kWidest[3] = {L"200 %", L"100 %", L"400 Hz"};
    legend_.count = 3;
    for (int j = 0; j < 3; ++j) {
        legend_.param[j] = kLegendParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"Width over frequency (off)");
}

void WidthGraphs::layout(int w, int h) {
    layoutFour(w, h, panel_);
    layoutLegend(legend_, panel_[0], titleW_, 0);
}

void WidthGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kWidth =
        L"The stereo width over the frequency, in % of the recording's own width (100 % = as recorded, 0 % = mono): "
        L"the bass width under the split, the width over it, the crossover between them. Behind it, faint, the "
        L"deck's spectrum - where its bass is.\n\nThree values are set here: the width (the line at the right), the "
        L"bass width (the line at the left; 0 % puts the bass in mono, firmer on a club system) and the split (the "
        L"upright line). Drag a line anywhere along it, or its mark; the mouse wheel over the left or right part "
        L"steps the bass width or the width. The legend names their values; the arrow behind a value sets it back.";
    static const wchar_t* const kPicture =
        L"The stereo picture of this deck (a goniometer): every dot is a moment of the sound - upright when both "
        L"channels are the same (mono), lying when they are opposite. Grey: the input, coloured: the output. A wider "
        L"setting spreads the coloured cloud sideways.";
    static const wchar_t* const kSide =
        L"Side / mid of the last 60 seconds, in % (grey: the input, coloured: the output): how much of the sound is "
        L"different between the channels. 0 % is mono; most music is between 20 and 60 %.";
    static const wchar_t* const kCorr =
        L"The correlation of the last 60 seconds (grey: the input, coloured: the output): +1 is mono, 0 unrelated "
        L"channels, under 0 they work against each other - played in mono (a phone, one speaker) such sound gets "
        L"weaker. Too much width pulls it down.";
    legendTips(legend_, panel_[0], kWidth, out);
    out.push_back({panel_[1], kPicture, -1, -1});
    out.push_back({panel_[2], kSide, -1, -1});
    out.push_back({panel_[3], kCorr, -1, -1});
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

// The deck's crossover is a Linkwitz-Riley pair in phase: the side gets bass width x low-pass + width x high-pass.
double WidthGraphs::widthAt(double hz) const {
    const double r = hz / std::max(1.0, values_[pWidthSplit]), r4 = r * r * r * r;
    return (values_[pBassWidth] + values_[pWidth] * r4) / (1.0 + r4);
}

std::wstring WidthGraphs::valueText(int j) const {
    switch (j) {
    case 0:
        return number(values_[pWidth], 0, L" %");
    case 1:
        return number(values_[pBassWidth], 0, L" %");
    default:
        return number(values_[pWidthSplit], 0, L" Hz");
    }
}

void WidthGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    static const wchar_t* const kTitle[kPanels] = {L"Width over frequency", L"Stereo picture", L"Side / mid, last 60 s",
                                                   L"Correlation, last 60 s"};
    const bool on = values_[pWidthOn] >= 0.5;
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
                           i == 0 && !on ? std::wstring(L"Width over frequency (off)") : kTitle[i], kDim, TA_LEFT});
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0 ? legend_.headerBottom + px(5) : p.top + th + px(7);
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        plot_[0].logX = true;
        plot_[0].x0 = kLoHz;
        plot_[0].x1 = kHiHz;
        plot_[0].y0 = 0.0;
        plot_[0].y1 = kTopPct;
        for (int j = 0; j < legend_.count; ++j) {
            const RECT& item = legend_.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, valueText(j), kOrange, TA_RIGHT});
        }
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
        Pen grid(col(kGrid), lw), dots(col(kGrid), lw);
        dots.SetDashStyle(DashStyleDot);
        auto row = [](double y) { return std::floor(y) + 0.5; };
        // ---- 2. the stereo picture: a square, its axes ----
        {
            Plot& q = plot_[1];
            const double side = std::min(q.r - q.l, q.b - q.t), cx = (q.l + q.r) / 2, cy = (q.t + q.b) / 2;
            q.l = cx - side / 2;
            q.r = cx + side / 2;
            q.t = cy - side / 2;
            q.b = cy + side / 2;
            g.DrawLine(&dots, pt(row(cx), q.t), pt(row(cx), q.b));
            g.DrawLine(&dots, pt(q.l, row(cy)), pt(q.r, row(cy)));
            g.DrawLine(&grid, pt(q.l, q.t), pt(q.r, q.b));
            g.DrawLine(&grid, pt(q.r, q.t), pt(q.l, q.b));
            all.push_back({static_cast<int>(cx) + px(3), static_cast<int>(q.t), L"mono", kDim, TA_LEFT});
            all.push_back({static_cast<int>(q.l), static_cast<int>(q.t), L"L", kDim, TA_LEFT});
            all.push_back({static_cast<int>(q.r), static_cast<int>(q.t), L"R", kDim, TA_RIGHT});
        }
        // ---- 3 and 4. the histories' scales ----
        plot_[2].x0 = plot_[3].x0 = 0.0;
        plot_[2].x1 = plot_[3].x1 = DeckAnalysis::kHistory - 1.0;
        plot_[2].y0 = 0.0;
        plot_[2].y1 = 100.0;
        plot_[3].y0 = -1.0;
        plot_[3].y1 = 1.0;
        for (double pct : {25.0, 50.0, 75.0}) g.DrawLine(&dots, pt(plot_[2].l, row(plot_[2].Y(pct))), pt(plot_[2].r, row(plot_[2].Y(pct))));
        g.DrawLine(&grid, pt(plot_[3].l, row(plot_[3].Y(0))), pt(plot_[3].r, row(plot_[3].Y(0))));
        g.DrawLine(&dots, pt(plot_[3].l, row(plot_[3].Y(0.5))), pt(plot_[3].r, row(plot_[3].Y(0.5))));
        all.push_back({static_cast<int>(plot_[2].l), static_cast<int>(plot_[2].Y(50.0)) - th - px(1), L"50 %", kDim, TA_LEFT});
        all.push_back({static_cast<int>(plot_[3].l), static_cast<int>(plot_[3].Y(0.0)) - th - px(1), L"0", kDim, TA_LEFT});
        all.push_back({static_cast<int>(plot_[3].l), static_cast<int>(plot_[3].t), L"+1 mono", kDim, TA_LEFT});
        for (int i = 2; i <= 3; ++i) {
            const Plot& q = plot_[i];
            all.push_back({static_cast<int>(q.r), static_cast<int>(q.b) - th - px(2), L"out", kTeal, TA_RIGHT});
            all.push_back({static_cast<int>(q.r) - textWidth(L"out") - px(8), static_cast<int>(q.b) - th - px(2), L"in",
                           kInLine, TA_RIGHT});
        }
    }
    labels(dc, all);
}

// The width over the frequency over the spectrum: the curve, the three lines, their marks and names.
void WidthGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    layerNames_.clear();
    const Plot& q = plot_[0];
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    const double split = values_[pWidthSplit], width = values_[pWidth], bass = values_[pBassWidth];
    const double xSplit = row(q.X(split)), yWidth = row(q.Y(width)), yBass = row(q.Y(bass));
    Pen dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 3.0f * lw);
    dots.SetDashStyle(DashStyleDot);
    dash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    g.DrawLine(&dots, pt(q.l, row(q.Y(100.0))), pt(q.r, row(q.Y(100.0)))); // as recorded
    for (double hz : {100.0, 1000.0, 10000.0}) g.DrawLine(&dots, pt(row(q.X(hz)), q.t), pt(row(q.X(hz)), q.b));
    g.DrawLine(&dash, pt(xSplit, q.t), pt(xSplit, q.b));
    g.DrawLine(&dash, pt(xSplit, yWidth), pt(q.r, yWidth));
    g.DrawLine(&dash, pt(q.l, yBass), pt(xSplit, yBass));
    std::vector<PointF> line;
    for (int i = 0; i <= 120; ++i) {
        const double hz = kLoHz * std::pow(kHiHz / kLoHz, i / 120.0);
        line.push_back(pt(q.X(hz), q.Y(widthAt(hz))));
    }
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    g.ResetClip();
    // the marks: the width's at the right, the bass width's at the left, the split's on the curve
    const double xWidth = q.r - px(14), xBass = q.l + px(14), ySplit = q.Y(widthAt(split));
    drawMark(g, mWidth, xWidth, yWidth);
    drawMark(g, mBass, xBass, yBass);
    drawMark(g, mSplit, xSplit, ySplit);
    auto nameY = [&](double y) { return static_cast<int>(y - th - px(2) < q.t ? y + px(2) : y - th - px(2)); };
    layerNames_.push_back({static_cast<int>(xWidth) - px(9), nameY(yWidth), L"Width " + valueText(0), kOrange, TA_RIGHT});
    layerNames_.push_back({static_cast<int>(xBass) + px(9), nameY(yBass), L"Bass width " + valueText(1), kOrange, TA_LEFT});
    layerNames_.push_back({static_cast<int>(xSplit) + px(9), static_cast<int>(q.b) - th - px(4), L"Split " + valueText(2),
                           kOrange, TA_LEFT});
    layerNames_.push_back({static_cast<int>(q.X(1000.0)) + px(3), static_cast<int>(row(q.Y(100.0))) - th - px(1),
                           L"as recorded", kDim, TA_LEFT});
}

void WidthGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    // ---- 1. the spectrum behind, faint ----
    const Plot& q = plot_[0];
    {
        const int saved = clip(dc, q);
        if (an && playing && an->specIn.frames() > 0) {
            const auto& points = an->specIn.points();
            double top = -70.0;
            for (float v : points) top = std::max(top, static_cast<double>(v));
            const double y1 = top + 3.0, y0 = y1 - kSpecRangeDb;
            std::vector<POINT> area;
            area.push_back(ipt(q.l, q.b));
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i), db = points[static_cast<size_t>(i)];
                area.push_back(ipt(q.X(hz), q.b - std::min(1.0, std::max(0.0, (db - y0) / (y1 - y0))) * (q.b - q.t)));
            }
            area.push_back(ipt(q.r, q.b));
            polygon(dc, area, blend(kInput, kPanel, 0.18));
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(9), static_cast<LONG>(q.t) - px(9), static_cast<LONG>(q.r) + px(9),
                        static_cast<LONG>(q.b) + px(9)});
    labels(dc, layerNames_);
    if (!an || !playing) {
        text(dc, (q.l + q.r) / 2, q.t + px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
        return;
    }
    // ---- 2. the stereo picture ----
    {
        const Plot& p = plot_[1];
        const double cx = (p.l + p.r) / 2, cy = (p.t + p.b) / 2, rad = (p.r - p.l) / 2, k = rad * 0.7071;
        const int dot = std::max(1, px(1.5));
        for (int which = 0; which < 2; ++which) {
            const StereoAnalyzer& st = which == 0 ? an->stereoIn : an->stereoOut;
            HBRUSH br = CreateSolidBrush(which == 0 ? blend(kInput, kPanel, 0.55) : kTeal);
            for (const auto& d : st.dots()) {
                const int gx = static_cast<int>(cx + (d[1] - d[0]) * k), gy = static_cast<int>(cy - (d[0] + d[1]) * k);
                if (std::abs(gx - cx) > rad || std::abs(gy - cy) > rad) continue;
                const RECT r{gx, gy, gx + dot, gy + dot};
                FillRect(dc, &r, br);
            }
            DeleteObject(br);
        }
    }
    // ---- 3 and 4. side / mid and the correlation of the last 60 s ----
    for (int i = 2; i <= 3; ++i) {
        const Plot& p = plot_[i];
        const int saved = clip(dc, p);
        std::vector<POINT> in, out;
        double nowIn = 0.0, nowOut = 0.0;
        for (int k = 0; k < DeckAnalysis::kHistory; ++k) {
            const DeckAnalysis::Point& pt0 = an->at(k);
            if (!pt0.live) {
                if (in.size() > 1) polyline(dc, in, kInLine, 1);
                if (out.size() > 1) polyline(dc, out, kTeal, std::max(1, px(1.5)));
                in.clear();
                out.clear();
                continue;
            }
            nowIn = i == 2 ? pt0.widthIn : pt0.corrIn;
            nowOut = i == 2 ? pt0.widthOut : pt0.corrOut;
            in.push_back(ipt(p.X(k), p.Y(nowIn)));
            out.push_back(ipt(p.X(k), p.Y(nowOut)));
        }
        if (in.size() > 1) polyline(dc, in, kInLine, 1);
        if (out.size() > 1) polyline(dc, out, kTeal, std::max(1, px(1.5)));
        RestoreDC(dc, saved);
        const std::wstring now = i == 2 ? number(nowOut, 0, L" %") : number(nowOut, 2, L"", true);
        text(dc, panel_[i].right - px(6), panel_[i].top + px(3), now, kTeal, TA_RIGHT);
    }
}

// The width's line (right of the split), the bass width's (left of it) and the split's upright line are taken
// anywhere along them.
int WidthGraphs::lineAt(POINT p) const {
    const Plot& q = plot_[0];
    if (p.x < q.l - 2 || p.x > q.r + 2 || p.y < q.t - 2 || p.y > q.b + 2) return -1;
    const long reach = px(5);
    if (std::labs(p.x - mark_[mSplit].x) <= reach) return mSplit;
    if (p.x > mark_[mSplit].x && std::labs(p.y - mark_[mWidth].y) <= reach) return mWidth;
    if (p.x < mark_[mSplit].x && std::labs(p.y - mark_[mBass].y) <= reach) return mBass;
    return -1;
}

int WidthGraphs::wheelParam(POINT p) const {
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return legend_.param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    const Plot& q = plot_[0];
    if (p.x >= q.l && p.x <= q.r && p.y >= q.t && p.y <= q.b) return p.x < mark_[mSplit].x ? pBassWidth : pWidth;
    return -1;
}

bool WidthGraphs::dragValue(int mark, POINT p, double& v) const {
    switch (mark) {
    case mWidth:
    case mBass:
        v = plot_[0].fromY(p.y);
        return true;
    case mSplit:
        v = plot_[0].fromX(p.x);
        return true;
    default:
        return false;
    }
}

} // namespace ap
