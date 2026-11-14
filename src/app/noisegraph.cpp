// SPDX-License-Identifier: GPL-3.0-or-later
#include "noisegraph.h"

#include "graphgdi.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>

namespace ap {

using namespace graph;

namespace {

const double kLoHz = 20.0, kHiHz = 20000.0;    // the bands' frequency scale
const double kLevelLo = -90.0, kLevelHi = 6.0; // LU to the aim: the bands graph's level scale, the curve's level axis
const double kSpecRangeDb = 54.0;              // the spectrum behind the bands
// the curve's gain axis: room over the 0 line for the names at the top, and down to the ratio's mark at 10:1
const double kGainTop = 6.0, kGainBottom = -48.0;
const double kRatioMarkLu = 5.0; // the ratio's mark: this far under the threshold, on the slope
const double kOpenLoMs = 0.2, kOpenHiMs = 100.0; // the attack's time axis (logarithmic)
// the hold-and-release graph's time axis: logarithmic in (ms + this), so that a hold of 0 has its place too
const double kCloseOffsetMs = 20.0, kCloseHiMs = 5000.0;

const wchar_t* const kHint = L"thick line: the band's level now";
const wchar_t* const kOff = L"the noise reduction is off: switch it on to see the bands' levels";
const int kCurveParam[3] = {pNoiseThreshold, pNoiseDepth, pNoiseRatio};
const int kTimeParam[2] = {pNoiseHold, pNoiseRelease};

std::wstring hzText(double hz) {
    if (hz < 1000.0) return number(hz, 0, L" Hz");
    const double k = hz / 1000.0;
    return number(k, std::fabs(k - std::round(k)) < 0.05 ? 0 : 1, L" k");
}

} // namespace

bool NoiseGraphs::owns(int param) const { return param >= pNoiseBands && param <= pNoiseSplit3; }

int NoiseGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pNoiseSplit1,    pNoiseSplit2, pNoiseSplit3, pNoiseThreshold, pNoiseThreshold,
                                         pNoiseDepth,     pNoiseRatio,  pNoiseAttack, pNoiseHold,      pNoiseRelease};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

// The threshold in the bands graph, the depth and the ratio go up / down; the splits, the threshold on the curve and
// the three times left / right.
bool NoiseGraphs::markUpDown(int mark) const { return mark == mThreshold || mark == mDepth || mark == mRatio; }

int NoiseGraphs::bands() const { return values_[pNoiseBands] >= 2.5 ? kNoiseBands : 1; }

double NoiseGraphs::split(int k) const { return values_[static_cast<size_t>(pNoiseSplit1 + k)]; }

double NoiseGraphs::edgeHz(int b) const { return b <= 0 ? kLoHz : b >= bands() ? kHiHz : split(b - 1); }

double NoiseGraphs::aim() const { return workingLevel(values_); }

double NoiseGraphs::gainAt(double level) const {
    return -noiseReductionDb(values_[pNoiseThreshold] - level, values_[pNoiseRatio] - 1.0, values_[pNoiseDepth]);
}

std::wstring NoiseGraphs::curveValue(int j) const {
    switch (j) {
    case 0:
        return number(values_[pNoiseThreshold], 0, L" LU");
    case 1:
        return number(values_[pNoiseDepth], 0, L" dB");
    default:
        return L"1:" + number(values_[pNoiseRatio], 1, L"");
    }
}

std::wstring NoiseGraphs::timeValue(int j) const {
    return number(values_[static_cast<size_t>(j == 0 ? pNoiseHold : pNoiseRelease)], 0, L" ms");
}

double NoiseGraphs::holdX(double ms) const { return plot_[3].X(ms + kCloseOffsetMs); }

void NoiseGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[3] = {L"Threshold", L"Depth", L"Ratio"};
    static const wchar_t* const kWidest[3] = {L"-80 LU", L"40 dB", L"1:10.0"};
    legend_.count = 3;
    for (int j = 0; j < 3; ++j) {
        legend_.param[j] = kCurveParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    static const wchar_t* const kTimeNames[2] = {L"Hold", L"Release"};
    static const wchar_t* const kTimeWidest[2] = {L"500 ms", L"2000 ms"};
    times_.count = 2;
    for (int j = 0; j < 2; ++j) {
        times_.param[j] = kTimeParam[j];
        times_.name[j] = kTimeNames[j];
        times_.widest[j] = kTimeWidest[j];
    }
    measureLegend(dc, legend_);
    measureLegend(dc, times_);
    bandsTitleW_ = textWidth(dc, L"Bands");
    timesTitleW_ = textWidth(dc, L"Hold and release");
    digitW_ = textWidth(dc, L"4");
    hintW_ = textWidth(dc, kHint);
    offW_ = textWidth(dc, kOff);
}

// The bands across the top, the curve, the attack and the hold and release under them (layoutFour). In the bands
// panel's header: the number of bands behind the title as a toggle of two segments, 1 | 4 (client, 2026-11-14: "make
// this a toggle 1 / 4 bands, no plus and minus" - the chosen one orange), and its reset icon, then the legend of the
// threshold, the depth and the ratio (on that line where it fits, else in columns under it) - the small curve panel
// keeps its height for the curve. The hold and the release have a legend in their panel's header; the attack an
// icon in its corner.
void NoiseGraphs::layout(int w, int h) {
    layoutFour(w, h, panel_);
    const RECT& p = panel_[0];
    const int rowH = px(18), top = static_cast<int>(p.top) + px(3) - (rowH - textH_) / 2;
    const int oneX = static_cast<int>(p.left) + px(6) + bandsTitleW_ + px(8), segW = std::max(px(20), digitW_ + px(10));
    const int fourX = oneX + segW - 1, iconX = fourX + segW + px(4); // the two segments share their middle edge
    bandsItem_ = {p.left, top, oneX, top + rowH};
    steppers_.push_back({RECT{oneX, top + px(1), oneX + segW, top + rowH - px(1)}, pNoiseBands, -1, L"1", true});
    steppers_.push_back({RECT{fourX, top + px(1), fourX + segW, top + rowH - px(1)}, pNoiseBands, 1, L"4", true});
    icons_.push_back({RECT{iconX, top, iconX + px(22), top + rowH}, pNoiseBands});
    bandsHeaderBottom_ = std::max(static_cast<int>(p.top) + textH_ + px(5), top + rowH);
    icons_.push_back({cornerIcon(panel_[2]), pNoiseAttack});
    layoutLegend(legend_, p, iconX + px(22) - (static_cast<int>(p.left) + px(6)), hintW_);
    layoutLegend(times_, panel_[3], timesTitleW_, 0);
}

// The rectangles do not overlap (the tooltip control keeps the tool the mouse is in). The reset icons that stand
// beside a split's value in the bands graph have no tooltip of their own: they lie in the graph.
void NoiseGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kBands =
        L"The noise reduction's bands over the music's spectrum (grey). The dashed upright lines are the splits "
        L"between the bands: drag one left or right. The dashed level line is the threshold, in LU to the target (the "
        L"dotted line): a band whose level falls under it is turned down - drag it up or down. The thick line in each "
        L"band is its level now; under the threshold, orange shows how far the band is turned down at this moment. "
        L"The number of bands - 4, or 1 for the whole sound - is the 1 | 4 toggle in the top line: click the other "
        L"one (the chosen one is orange).\n\nThe mouse "
        L"wheel steps the split the pointer is on, else the threshold. A small arrow beside a split that was changed "
        L"sets it back.";
    static const wchar_t* const kCurve =
        L"What the stage does to one band: the gain it gets (up) against its level (to the right, in LU to the "
        L"target). Above the threshold nothing happens. Under it the band goes down: every dB less in gives Ratio dB "
        L"less out (the slope), by Depth at most (the floor). The white dots are the bands now, each with its number."
        L"\n\nDrag the upright line (Threshold) left or right and the two level lines up or down: Depth's is the "
        L"floor, Ratio's goes through the slope 5 LU under the threshold - lower is a steeper slope. The legend over "
        L"the bands graph names the values; the arrow behind a value sets it back.";
    static const wchar_t* const kOpen =
        L"Attack (0.5 .. 50 ms): a band that was turned down by Depth, when the music comes back (dashed) - how fast "
        L"it opens (the line). Short keeps the first beat after a pause; too short can tick on deep bass. The time "
        L"axis is logarithmic.\n\nSet it here: drag the upright line, or turn the mouse wheel over this graph for one "
        L"step of 0.5 ms. The arrow in the corner sets it back.";
    static const wchar_t* const kClose =
        L"Hold and release: the music stops at the left edge, and the band's gain (the line). Hold (0 .. 500 ms): the "
        L"band stays open this long, so it does not chatter between notes. Release (20 .. 2000 ms): then it goes "
        L"down, by Depth at most - long lets a note's tail fade naturally, short takes the noise away sooner. The "
        L"time axis is logarithmic.\n\nDrag an upright line left or right, or turn the mouse wheel over this graph "
        L"for one step of 10 ms of the line nearer the pointer. The arrows in the legend set them back.";
    // the bands panel: its header row (the number of bands, the rest of the row - or, with the legend in it, what is
    // left and right of the legend), then everything under the header; four rectangles either way
    const RECT& p = panel_[0];
    const RECT& bandsIcon = icons_[0].rect;
    const RECT& lb = legend_.bounds;
    if (legend_.inLine) {
        const int bottom = std::max(bandsHeaderBottom_, legend_.headerBottom);
        out.push_back({RECT{p.left, p.top, bandsIcon.left, bottom}, nullptr, pNoiseBands, -1});
        out.push_back({RECT{bandsIcon.right, p.top, lb.left, bottom}, kBands, -1, -1});
        out.push_back({RECT{lb.right, p.top, p.right, bottom}, kBands, -1, -1});
        out.push_back({RECT{p.left, bottom, p.right, p.bottom}, kBands, -1, -1});
    } else {
        const int row = std::min(bandsHeaderBottom_, static_cast<int>(lb.top));
        out.push_back({RECT{p.left, p.top, bandsIcon.left, row}, nullptr, pNoiseBands, -1});
        out.push_back({RECT{bandsIcon.right, p.top, p.right, row}, kBands, -1, -1});
        out.push_back({RECT{lb.right, lb.top, p.right, lb.bottom}, kBands, -1, -1});
        out.push_back({RECT{p.left, lb.bottom, p.right, p.bottom}, kBands, -1, -1});
    }
    out.push_back({panel_[1], kCurve, -1, -1});
    {
        const RECT& q = panel_[2];
        const RECT& icon = icons_[1].rect;
        out.push_back({RECT{q.left, q.top, icon.left, q.bottom}, kOpen, -1, -1});
        out.push_back({RECT{icon.left, icon.bottom, q.right, q.bottom}, kOpen, -1, -1});
    }
    legendTips(times_, panel_[3], kClose, out);
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (int j = 0; j < times_.count; ++j) out.push_back({times_.item[j], nullptr, times_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

// The panels, their titles, the legends' texts, the curve, the attack's and the hold-and-release graphs. The bands
// graph itself is drawn with the audio: its lines lie over the spectrum (renderLayer, paintLive).
void NoiseGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    static const wchar_t* const kTitle[kPanels] = {L"Bands", L"Curve per band", L"Attack: the music comes back",
                                                   L"Hold and release"};
    const bool on = values_[pNoiseOn] >= 0.5;
    const int th = textH_;
    std::vector<Label> all;
    {
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(col(kBack));
        drawPanels(g, panel_, kPanels);
        for (int i = 0; i < kPanels; ++i) {
            const RECT& p = panel_[i];
            all.push_back({static_cast<int>(p.left) + px(6), static_cast<int>(p.top) + px(3), kTitle[i], kDim, TA_LEFT});
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0   ? std::max(bandsHeaderBottom_, legend_.headerBottom) + px(5)
                  : i == 3 ? times_.headerBottom + px(5)
                           : p.top + th + px(7);
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        // ---- 1. the bands: the number in the header (the graph: renderLayer, paintLive) ----
        plot_[0].logX = true;
        plot_[0].x0 = kLoHz;
        plot_[0].x1 = kHiHz;
        plot_[0].y0 = kLevelLo;
        plot_[0].y1 = kLevelHi;
        {
            const RECT& p = panel_[0]; // the number of bands is the toggle's chosen segment (drawControls)
            const int leftOfHint = legend_.inLine ? static_cast<int>(legend_.bounds.right) : static_cast<int>(icons_[0].rect.right);
            if (p.right - px(6) - (on ? hintW_ : offW_) > leftOfHint + px(12))
                all.push_back({static_cast<int>(p.right) - px(6), static_cast<int>(p.top) + px(3), on ? kHint : kOff, kDim, TA_RIGHT});
        }
        // ---- 2. the curve per band, 3. the attack, 4. the hold and the release ----
        drawCurve(g, all);
        drawOpening(g, all);
        drawClosing(g, all);
    }
    auto legendTexts = [&](const Legend& lg, bool curve) {
        for (int j = 0; j < lg.count; ++j) {
            const RECT& item = lg.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, lg.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, curve ? curveValue(j) : timeValue(j), kOrange, TA_RIGHT});
        }
    };
    legendTexts(legend_, true);
    legendTexts(times_, false);
    labels(dc, all);
}

// The gain a band gets against its level - the stage's rule (noiseReductionDb). Its three values are lines: the
// threshold upright (where the band starts to go down), the depth a level line (the floor), the ratio a level line
// through its mark on the slope 5 LU under the threshold (where a steeper slope is lower; dotted on under the floor
// when the depth cuts the slope off before it). Names with their values at the lines, off each other: the
// threshold's at the top, over the 0 line where the bands' dots are; the depth's over its line at the left; the
// ratio's under its line at the right (its line often lies just under the 0 line).
void NoiseGraphs::drawCurve(Gdiplus::Graphics& g, std::vector<Label>& all) {
    using namespace Gdiplus;
    Plot& q = plot_[1];
    q.x0 = kLevelLo;
    q.x1 = kLevelHi;
    q.y0 = kGainBottom;
    q.y1 = kGainTop;
    const double thr = values_[pNoiseThreshold], depth = values_[pNoiseDepth], slope = values_[pNoiseRatio] - 1.0;
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    const double xThr = row(q.X(thr)), yDepth = row(q.Y(-depth)), yRatio = row(q.Y(-slope * kRatioMarkLu));
    const double xRatio = q.X(thr - kRatioMarkLu), xDepth = q.l + px(7), y0 = row(q.Y(0.0));

    enum { nThr, nDepth, nRatio, kNames };
    std::wstring text[kNames];
    int textW[kNames];
    for (int j = 0; j < kNames; ++j) {
        text[j] = std::wstring(legend_.name[j]) + L" " + curveValue(j);
        textW[j] = textWidth(text[j]);
    }
    const int above = th + px(2), under = px(2);
    std::vector<RECT> name(kNames);
    auto put = [&](int j, double x, double y, bool leftAligned) {
        const int at = static_cast<int>(x), top = static_cast<int>(y);
        name[static_cast<size_t>(j)] = leftAligned ? RECT{at, top, at + textW[j], top + th} : RECT{at - textW[j], top, at, top + th};
    };
    const bool thrLeft = xThr - px(5) - textW[nThr] >= q.l + px(4);
    put(nThr, thrLeft ? xThr - px(5) : xThr + px(5), q.t + px(1), !thrLeft);
    // the depth's over its line, else under it; the ratio's under its line, else over it - the first place that is
    // in the plot, meets no name placed before and is not run through by another level line
    auto fine = [&](int j, const std::vector<double>& lines) {
        const RECT& r = name[static_cast<size_t>(j)];
        if (r.top < q.t || r.bottom > q.b) return false;
        RECT both;
        for (int k = 0; k < j; ++k)
            if (IntersectRect(&both, &r, &name[static_cast<size_t>(k)])) return false;
        for (double y : lines)
            if (y > r.top - 1 && y < r.bottom + 1) return false;
        return true;
    };
    put(nDepth, xDepth + px(8), yDepth - above, true);
    if (!fine(nDepth, {yRatio, y0})) {
        put(nDepth, xDepth + px(8), yDepth + under, true);
        if (!fine(nDepth, {yRatio, y0})) put(nDepth, xDepth + px(8), yDepth - above < q.t ? yDepth + under : yDepth - above, true);
    }
    put(nRatio, q.r - px(4), yRatio + under, false);
    if (!fine(nRatio, {yDepth, y0})) {
        put(nRatio, q.r - px(4), yRatio - above, false);
        if (!fine(nRatio, {yDepth, y0})) put(nRatio, q.r - px(4), yRatio + under + th > q.b ? yRatio - above : yRatio + under, false);
    }

    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    Pen dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.5f * lw), on(col(kOrange, 150), lw);
    dots.SetDashStyle(DashStyleDot);
    dash.SetDashStyle(DashStyleDash);
    on.SetDashStyle(DashStyleDot);
    curve.SetLineJoin(LineJoinRound);
    SolidBrush zone(col(kInput, 22));
    g.FillRectangle(&zone, static_cast<REAL>(q.l), static_cast<REAL>(q.t), static_cast<REAL>(xThr - q.l),
                    static_cast<REAL>(q.b - q.t)); // under the threshold: turned down
    g.DrawLine(&dots, pt(q.l, y0), pt(q.r, y0)); // the gain 0: left alone
    for (double lu = -80.0; lu <= 0.0; lu += 20.0) g.DrawLine(&dots, pt(q.X(lu), q.b - px(4)), pt(q.X(lu), q.b));
    lineAround(g, dash, true, xThr, q.t, q.b, name);
    lineAround(g, dash, false, yDepth, q.l, q.r, name);
    lineAround(g, dash, false, yRatio, q.l, q.r, name);
    if (slope > 0.0 && slope * kRatioMarkLu > depth) // the slope on under the floor, to the ratio's mark
        g.DrawLine(&on, pt(q.X(thr - depth / slope), q.Y(-depth)), pt(xRatio, yRatio));
    std::vector<PointF> line;
    for (double x = kLevelLo; x <= kLevelHi + 0.01; x += 0.25) line.push_back(pt(q.X(x), q.Y(gainAt(x))));
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    g.ResetClip();
    drawMark(g, mCurveThreshold, xThr, q.Y(gainAt(thr)));
    drawMark(g, mDepth, xDepth, yDepth);
    drawMark(g, mRatio, xRatio, yRatio);
    for (int j = 0; j < kNames; ++j)
        all.push_back({static_cast<int>(name[static_cast<size_t>(j)].left), static_cast<int>(name[static_cast<size_t>(j)].top),
                       text[j], kOrange, TA_LEFT});
    const int yTicks = static_cast<int>(q.b) - th - px(5);
    for (double lu = -80.0; lu <= 0.0; lu += 20.0) {
        const double x = q.X(lu);
        if (std::fabs(x - xThr) < px(24)) continue; // not where the threshold's line stands
        all.push_back({static_cast<int>(x), yTicks, lu == 0.0 ? std::wstring(L"0 LU") : number(lu, 0, L""), kDim,
                       static_cast<UINT>(lu == 0.0 ? TA_RIGHT : TA_CENTER)});
    }
}

// The attack: a band turned down by the depth opens when the music comes back (dashed, at 0 dB) - the gain closes
// on 0 in the attack time (a one-pole, as the stage). Logarithmic time from 0.2 to 100 ms; the upright line at the
// attack time is the value.
void NoiseGraphs::drawOpening(Gdiplus::Graphics& g, std::vector<Label>& all) {
    using namespace Gdiplus;
    Plot& q = plot_[2];
    const double depth = values_[pNoiseDepth], attack = values_[pNoiseAttack], size = std::max(depth, 6.0);
    q.logX = true;
    q.x0 = kOpenLoMs;
    q.x1 = kOpenHiMs;
    q.y0 = -1.5 * size; // room under the floor for the times
    q.y1 = 0.25 * size;
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto gain = [&](double ms) { return -depth * std::exp(-ms / attack); };
    Pen grid(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw), inputDash(col(kInput), 1.5f * lw);
    dash.SetDashStyle(DashStyleDash);
    inputDash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    SolidBrush orangeFill(col(kOrange, 60));
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    std::vector<PointF> line, area;
    area.push_back(pt(q.l, q.Y(0.0)));
    for (int i = 0; i <= 160; ++i) {
        const double ms = kOpenLoMs * std::pow(kOpenHiMs / kOpenLoMs, i / 160.0);
        line.push_back(pt(q.X(ms), q.Y(gain(ms))));
        area.push_back(line.back());
    }
    area.push_back(pt(q.r, q.Y(0.0)));
    g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size())); // what is still turned down
    g.DrawLine(&grid, pt(q.l, q.Y(-depth)), pt(q.r, q.Y(-depth)));
    g.DrawLine(&inputDash, pt(q.l, q.Y(0.0)), pt(q.r, q.Y(0.0)));
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    const double xm = q.X(attack);
    g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
    for (double ms : {1.0, 10.0}) g.DrawLine(&grid, pt(q.X(ms), q.b - px(4)), pt(q.X(ms), q.b));
    g.ResetClip();
    drawMark(g, mAttack, xm, q.Y(gain(attack)));
    const bool right = xm > q.l + 0.6 * (q.r - q.l);
    all.push_back({static_cast<int>(xm + (right ? -px(5) : px(5))), static_cast<int>(q.t), number(attack, 1, L" ms"), kOrange,
                   static_cast<UINT>(right ? TA_RIGHT : TA_LEFT)});
    const int yTicks = static_cast<int>(q.b) - th - px(4);
    static const struct {
        double ms;
        const wchar_t* text;
        UINT align;
    } kTick[3] = {{1.0, L"1 ms", TA_CENTER}, {10.0, L"10 ms", TA_CENTER}, {kOpenHiMs, L"100 ms", TA_RIGHT}};
    for (const auto& tick : kTick)
        if (std::fabs(q.X(tick.ms) - xm) > px(26)) all.push_back({static_cast<int>(q.X(tick.ms)), yTicks, tick.text, kDim, tick.align});
}

// The hold and the release: the music stops at the left edge; the band stays open for the hold, then goes down by
// the depth at the release time (a one-pole). Logarithmic time in (ms + 20) up to 5 s, so a hold of 0 sits at the
// edge; the upright lines are the hold's end and one release time after it.
void NoiseGraphs::drawClosing(Gdiplus::Graphics& g, std::vector<Label>& all) {
    using namespace Gdiplus;
    Plot& q = plot_[3];
    const double depth = values_[pNoiseDepth], hold = values_[pNoiseHold], release = values_[pNoiseRelease];
    const double size = std::max(depth, 6.0);
    q.logX = true;
    q.x0 = kCloseOffsetMs;
    q.x1 = kCloseHiMs + kCloseOffsetMs;
    q.y0 = -1.5 * size; // room under the floor for the times
    q.y1 = 0.25 * size;
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto gain = [&](double ms) { return ms <= hold ? 0.0 : -depth * (1.0 - std::exp(-(ms - hold) / release)); };
    Pen grid(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw);
    dash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    SolidBrush orangeFill(col(kOrange, 60));
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    std::vector<double> times;
    for (int i = 0; i <= 200; ++i) times.push_back(kCloseOffsetMs * std::pow((kCloseHiMs + kCloseOffsetMs) / kCloseOffsetMs, i / 200.0) - kCloseOffsetMs);
    times.push_back(hold); // the corner where it starts to go down
    std::sort(times.begin(), times.end());
    std::vector<PointF> line, area;
    area.push_back(pt(q.l, q.Y(0.0)));
    for (double ms : times) {
        line.push_back(pt(holdX(ms), q.Y(gain(ms))));
        area.push_back(line.back());
    }
    area.push_back(pt(q.r, q.Y(0.0)));
    g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size())); // what is taken off
    g.DrawLine(&grid, pt(q.l, q.Y(0.0)), pt(q.r, q.Y(0.0)));
    g.DrawLine(&grid, pt(q.l, q.Y(-depth)), pt(q.r, q.Y(-depth)));
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    const double xHold = holdX(hold), xRelease = holdX(hold + release);
    g.DrawLine(&dash, pt(xHold, q.t), pt(xHold, q.b));
    g.DrawLine(&dash, pt(xRelease, q.t), pt(xRelease, q.b));
    for (double ms : {100.0, 1000.0}) g.DrawLine(&grid, pt(holdX(ms), q.b - px(4)), pt(holdX(ms), q.b));
    g.ResetClip();
    drawMark(g, mHold, xHold, q.Y(0.0));
    drawMark(g, mRelease, xRelease, q.Y(gain(hold + release)));
    // the names with their values at the top of their lines: right of a line, left of it in the right part; the
    // release's a row lower where the two would meet
    RECT name[2];
    std::wstring text[2];
    for (int j = 0; j < 2; ++j) {
        text[j] = std::wstring(times_.name[j]) + L" " + timeValue(j);
        const double x = j == 0 ? xHold : xRelease;
        const int width = textWidth(text[j]), top = static_cast<int>(q.t);
        const bool left = x > q.l + 0.6 * (q.r - q.l);
        const int at = static_cast<int>(left ? x - px(5) - width : x + px(5));
        name[j] = RECT{at, top, at + width, top + th};
    }
    RECT both;
    if (IntersectRect(&both, &name[0], &name[1])) OffsetRect(&name[1], 0, th + px(2));
    for (int j = 0; j < 2; ++j) all.push_back({static_cast<int>(name[j].left), static_cast<int>(name[j].top), text[j], kOrange, TA_LEFT});
    const int yTicks = static_cast<int>(q.b) - th - px(4);
    static const struct {
        double ms;
        const wchar_t* text;
        UINT align;
    } kTick[3] = {{100.0, L"0.1 s", TA_CENTER}, {1000.0, L"1 s", TA_CENTER}, {kCloseHiMs, L"5 s", TA_RIGHT}};
    for (const auto& tick : kTick) {
        const double x = holdX(tick.ms);
        if (std::fabs(x - xHold) > px(26) && std::fabs(x - xRelease) > px(26))
            all.push_back({static_cast<int>(x), yTicks, tick.text, kDim, tick.align});
    }
}

// The bands graph's still parts - the target's line, the threshold's line with the shade under it, the splits, their
// marks - lie over the spectrum and the bands' levels, which move: they go into the strip's layer. Their texts are
// written after it (bandNames_), and a split that differs from its reset value gets its reset icon beside it
// (moreIcons_).
void NoiseGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    bandNames_.clear();
    for (int i = mSplit1; i <= mSplit3; ++i) mark_[static_cast<size_t>(i)] = POINT{-10000, -10000}; // with 1 band: none
    const Plot& q = plot_[0];
    const int n = bands(), th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    auto edge = [&](int b) { return q.X(edgeHz(b)); };
    const double yThr = row(q.Y(values_[pNoiseThreshold])), y0 = row(q.Y(0.0)), xMark = q.l + px(7);
    // the threshold's name with its value over its line beside its mark at the left (under it where the splits'
    // texts are at the top)
    const std::wstring thrText = std::wstring(legend_.name[0]) + L" " + curveValue(0);
    const int thrW = textWidth(thrText), splitRow = static_cast<int>(q.t) + px(1) + th + px(2);
    const int thrTop = static_cast<int>(yThr - th - px(2) < splitRow ? yThr + px(2) : yThr - th - px(2));
    const int thrLeft = static_cast<int>(xMark) + px(8);
    const std::vector<RECT> names = {RECT{thrLeft, thrTop, thrLeft + thrW, thrTop + th}};
    Pen dots(col(kGrid), lw), dash(col(kOrange), lw);
    dots.SetDashStyle(DashStyleDot);
    dash.SetDashStyle(DashStyleDash);
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    SolidBrush under(col(kInput, 18));
    g.FillRectangle(&under, static_cast<REAL>(q.l), static_cast<REAL>(yThr), static_cast<REAL>(q.r - q.l),
                    static_cast<REAL>(q.b - yThr)); // under the threshold: turned down
    g.DrawLine(&dots, pt(q.l, y0), pt(q.r, y0));  // the target
    for (int k = 0; k + 1 < n; ++k) lineAround(g, dash, true, row(edge(k + 1)), q.t, q.b, names);
    lineAround(g, dash, false, yThr, q.l, q.r, names);
    g.ResetClip();
    drawMark(g, mThreshold, xMark, yThr);
    bandNames_.push_back({thrLeft, thrTop, thrText, kOrange, TA_LEFT});
    bandNames_.push_back({static_cast<int>(q.r) - px(3), static_cast<int>(y0) + px(2), L"target", kDim, TA_RIGHT});
    const int iconW = px(18);
    // the splits: a mark at the top of the line, the frequency beside it - right of the line, or left of it when the
    // band there is the wider one
    for (int k = 0; k + 1 < n; ++k) {
        const int param = pNoiseSplit1 + k;
        const double x = edge(k + 1);
        drawMark(g, mSplit1 + k, x, q.t + px(7));
        const std::wstring s = hzText(split(k));
        const int width = textWidth(s), need = width + (lit(param) ? iconW + px(2) : 0);
        const double roomRight = edge(k + 2) - x - px(14), roomLeft = x - edge(k) - px(14);
        if (need > std::max(roomRight, roomLeft)) continue;
        const bool right = roomRight >= need;
        const int tx = static_cast<int>(right ? x + px(8) : x - px(8) - need);
        bandNames_.push_back({tx, static_cast<int>(q.t) + px(1), s, kOrange, TA_LEFT});
        if (lit(param)) {
            const int ix = tx + width + px(2);
            moreIcons_.push_back({RECT{ix, static_cast<LONG>(q.t) - px(1), ix + iconW, static_cast<LONG>(q.t) - px(1) + px(18)}, param});
        }
    }
    for (int b = 0; b < n; ++b)
        bandNames_.push_back({static_cast<int>((edge(b) + edge(b + 1)) / 2), static_cast<int>(q.b) - th - px(2),
                              std::to_wstring(b + 1), kDim, TA_CENTER});
}

// The parts that move with the audio: the spectrum behind the bands, each band's level at this moment and what the
// stage takes off it, and a dot per band on the curve.
void NoiseGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    const DeckMeters* m = meters();
    const bool on = values_[pNoiseOn] >= 0.5;
    const Plot& q = plot_[0];
    const int n = bands(), th = textH_;
    const double ref = aim();
    auto edge = [&](int b) { return q.X(edgeHz(b)); };
    // the stage runs on this deck (not while it is off or the deck is bypassed): every band in use has a level - in
    // digital silence the floor, at the bottom of the scale
    const bool running = playing && on && m && (m->active & (1u << sNoise)) != 0;
    auto has = [&](int b) { return running && b < n; };
    auto level = [&](int b) { return std::max(kLevelLo, m->noiseIn[b] - ref); }; // LU to the aim
    auto reduction = [&](int b) { return std::max(0.0, std::min(40.0, m->noiseDb[b])); };
    std::vector<Label> numbers;
    {
        const int saved = clip(dc, q);
        if (an && playing && an->specIn.frames() > 0) { // the music's spectrum, on a scale of its own
            const auto& points = an->specIn.points();
            double top = -70.0;
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) top = std::max(top, static_cast<double>(points[static_cast<size_t>(i)]));
            const double bottom = top + 3.0 - kSpecRangeDb;
            std::vector<POINT> area;
            area.push_back(ipt(q.l, q.b));
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i);
                if (hz < kLoHz || hz > kHiHz) continue;
                const double f = std::min(1.0, std::max(0.0, (points[static_cast<size_t>(i)] - bottom) / kSpecRangeDb));
                area.push_back(ipt(q.X(hz), q.b - f * (q.b - q.t)));
            }
            area.push_back(ipt(q.r, q.b));
            polygon(dc, area, blend(kInput, kPanel, 0.2));
        }
        for (int b = 0; b < n; ++b) { // the band's level now and, under it, what the stage takes off (orange)
            if (!has(b)) continue;
            const double x0 = edge(b) + px(3), x1 = edge(b + 1) - px(3), y = q.Y(level(b)), red = reduction(b);
            if (red >= 0.05) {
                const double y2 = q.Y(level(b) - red);
                polygon(dc, {ipt(x0, y), ipt(x1, y), ipt(x1, y2), ipt(x0, y2)}, blend(kOrange, kPanel, 0.36));
                polyline(dc, {ipt(x0, y2), ipt(x1, y2)}, kOrange, std::max(1, px(1.5)));
                const std::wstring s = number(-red, 1, L" dB");
                if (red >= 0.5 && x1 - x0 > textWidth(dc, s.c_str()) + px(8) && y2 + px(3) + th < q.b - th - px(4))
                    numbers.push_back({static_cast<int>((x0 + x1) / 2), static_cast<int>(y2) + px(3), s, kOrange, TA_CENTER});
            }
            polyline(dc, {ipt(x0, y), ipt(x1, y)}, kTeal, std::max(2, px(3)));
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(9), static_cast<LONG>(q.t) - px(9), static_cast<LONG>(q.r) + px(9),
                        static_cast<LONG>(q.b) + px(9)});
    labels(dc, bandNames_);
    labels(dc, numbers);
    if (!playing)
        text(dc, (q.l + q.r) / 2, q.b - 2 * th - px(8), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);

    // the curve: where each band is now - its level, and the gain the stage gives it at this moment (which lags the
    // level by the attack, the hold and the release)
    bool any = false;
    for (int b = 0; b < n; ++b) any = any || has(b);
    if (any) {
        using namespace Gdiplus;
        const Plot& c = plot_[1];
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush white(col(RGB(250, 250, 250)));
        std::vector<Label> dots;
        for (int b = 0; b < n; ++b) {
            if (!has(b)) continue;
            const double cx = c.X(std::min(kLevelHi, std::max(kLevelLo, level(b)))), cy = c.Y(-reduction(b));
            const double r = px(34) / 10.0;
            g.FillEllipse(&white, static_cast<REAL>(cx - r), static_cast<REAL>(cy - r), static_cast<REAL>(2 * r), static_cast<REAL>(2 * r));
            if (n > 1) dots.push_back({static_cast<int>(cx), static_cast<int>(cy) + px(5), std::to_wstring(b + 1), kText, TA_CENTER});
        }
        labels(dc, dots);
    }
}

// The lines are taken anywhere along them, not only by their marks: in the bands graph a split (the nearest within a
// few pixels) and else the threshold's line; in the curve the threshold's upright line and the depth's and the ratio's
// level lines (where those two lie on each other, the one whose mark is nearer); in the small graphs their upright
// lines.
int NoiseGraphs::lineAt(POINT p) const {
    const long reach = px(5), none = reach + 1;
    auto inside = [&](const Plot& q) { return p.x >= q.l - 2 && p.x <= q.r + 2 && p.y >= q.t - 2 && p.y <= q.b + 2; };
    const Plot& q = plot_[0];
    if (inside(q)) {
        const int n = bands();
        int best = -1;
        long bestD = none;
        for (int k = 0; k + 1 < n; ++k) {
            const long d = std::labs(p.x - std::lround(q.X(split(k))));
            if (d < bestD) {
                bestD = d;
                best = mSplit1 + k;
            }
        }
        if (best >= 0) return best;
        return std::labs(p.y - std::lround(q.Y(values_[pNoiseThreshold]))) <= reach ? mThreshold : -1;
    }
    const Plot& c = plot_[1];
    if (inside(c)) {
        long d[3] = {std::labs(p.x - std::lround(c.X(values_[pNoiseThreshold]))),
                     std::labs(p.y - std::lround(c.Y(-values_[pNoiseDepth]))),
                     std::labs(p.y - std::lround(c.Y(-(values_[pNoiseRatio] - 1.0) * kRatioMarkLu)))};
        if (d[1] <= reach && std::labs(d[1] - d[2]) <= 1) { // the depth's and the ratio's lines on each other
            if (std::labs(p.x - mark_[mDepth].x) <= std::labs(p.x - mark_[mRatio].x)) d[2] = none;
            else d[1] = none;
        }
        static const int kLine[3] = {mCurveThreshold, mDepth, mRatio};
        int best = -1;
        long bestD = none;
        for (int i = 0; i < 3; ++i) {
            if (d[i] < bestD) {
                bestD = d[i];
                best = kLine[i];
            }
        }
        return best;
    }
    if (inside(plot_[2])) return std::labs(p.x - mark_[mAttack].x) <= reach ? mAttack : -1;
    if (inside(plot_[3])) {
        const long dh = std::labs(p.x - mark_[mHold].x), dr = std::labs(p.x - mark_[mRelease].x);
        if (std::min(dh, dr) > reach) return -1;
        return dh < dr ? mHold : mRelease;
    }
    return -1;
}

bool NoiseGraphs::dragValue(int mark, POINT p, double& v) const {
    if (mark >= mSplit1 && mark <= mSplit3) { // a split: the owner's rule keeps the splits in order and apart
        v = plot_[0].fromX(p.x);
        return true;
    }
    switch (mark) {
    case mThreshold:
        v = plot_[0].fromY(p.y);
        return true;
    case mCurveThreshold:
        v = plot_[1].fromX(p.x);
        return true;
    case mDepth:
        v = -plot_[1].fromY(p.y);
        return true;
    case mRatio: // the slope that brings the curve to the mouse kRatioMarkLu under the threshold
        v = 1.0 + std::max(0.0, -plot_[1].fromY(p.y)) / kRatioMarkLu;
        return true;
    case mAttack:
        v = plot_[2].fromX(p.x);
        return true;
    case mHold:
        v = plot_[3].fromX(p.x) - kCloseOffsetMs;
        return true;
    case mRelease: // one release time after the hold's end
        v = plot_[3].fromX(p.x) - kCloseOffsetMs - values_[pNoiseHold];
        return true;
    default:
        return false;
    }
}

// Where the wheel steps a value: over an item of a legend, over the number of bands, over the attack's graph, over
// the hold-and-release graph the line nearer the pointer, and in the bands graph the split the pointer is on or else
// the threshold.
int NoiseGraphs::wheelParam(POINT p) const {
    for (const Legend* lg : {&legend_, &times_})
        for (int j = 0; j < lg->count; ++j)
            if (PtInRect(&lg->item[j], p)) return lg->param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    if (PtInRect(&bandsItem_, p)) return pNoiseBands;
    for (const Stepper& s : steppers_)
        if (PtInRect(&s.rect, p)) return s.param;
    if (PtInRect(&panel_[2], p)) return pNoiseAttack;
    if (PtInRect(&panel_[3], p))
        return std::labs(p.x - mark_[mHold].x) < std::labs(p.x - mark_[mRelease].x) ? pNoiseHold : pNoiseRelease;
    const Plot& q = plot_[0];
    if (p.x < q.l || p.x > q.r || p.y < q.t || p.y > q.b) return -1;
    const int n = bands();
    for (int k = 0; k + 1 < n; ++k)
        if (std::labs(p.x - std::lround(q.X(split(k)))) <= px(8)) return pNoiseSplit1 + k;
    return pNoiseThreshold;
}

// One wheel notch is one step of the value. The three times go by the parameter's own step (0.5 ms, 10 ms) and a
// split to the next value its slider has: on these logarithmic scales one slider position often is the same value.
// The others are on linear scales, where a position is a step.
double NoiseGraphs::stepped(int param, int steps) const {
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    if (param == pNoiseAttack || param == pNoiseHold || param == pNoiseRelease)
        return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
    if (param >= pNoiseSplit1 && param <= pNoiseSplit3) {
        const int last = paramPositions(param), dir = steps < 0 ? -1 : 1;
        int pos = paramPositionOf(param, now);
        double v = now;
        for (int s = 0; s < std::abs(steps); ++s) {
            double next = v;
            while (next == v && pos + dir >= 0 && pos + dir <= last) next = paramValueAt(param, pos += dir);
            v = next;
        }
        return v;
    }
    return GraphStrip::stepped(param, steps);
}

} // namespace ap
