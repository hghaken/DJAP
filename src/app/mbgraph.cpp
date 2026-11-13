// SPDX-License-Identifier: GPL-3.0-or-later
#include "mbgraph.h"

#include "graphgdi.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>

namespace ap {

using namespace graph;

namespace {

const double kLoHz = 20.0, kHiHz = 20000.0;     // the bands' frequency scale
const double kGainLo = -18.0, kGainHi = 18.0;   // dB: a band's gain - fixed (-12 .. +12) plus what the stage does
const double kSpecRangeDb = 54.0;               // the spectrum behind the bands
const double kCurveLo = -26.0, kCurveHi = 14.0; // the curve: dB to the band's own recent level, in and out
const double kMaxLiftMarkDb = 2.0;              // the largest lift's mark this far from the curve graph's left end
const double kAmountMarkDb = 1.0;               // the amount's mark this far under its right end
// the attack and release graphs: a hit this far over "down above"
const double kHitDb = 10.0, kAttackSpanMs = 330.0, kReleaseFromMs = -200.0, kReleaseToMs = 2200.0;

const wchar_t* const kHint = L"thick line: the band's gain now";
const int kLegendParam[4] = {pDensAmount, pDensDownAbove, pDensLiftBelow, pDensMaxLift};

std::wstring hzText(double hz) {
    if (hz < 1000.0) return number(hz, 0, L" Hz");
    const double k = hz / 1000.0;
    return number(k, std::fabs(k - std::round(k)) < 0.05 ? 0 : 1, L" k");
}

} // namespace

bool MbGraphs::owns(int param) const { return param >= pDensBands && param <= pDensGain7; }

int MbGraphs::markParam(int mark) const {
    if (mark < 0 || mark >= kMarks) return -1;
    if (mark < mGain1) return pDensSplit1 + mark;
    if (mark < mDown) return pDensGain1 + (mark - mGain1);
    static const int kParamOf[kMarks - mDown] = {pDensDownAbove, pDensLiftBelow, pDensMaxLift,
                                                 pDensAmount,    pDensAttack,    pDensRelease};
    return kParamOf[mark - mDown];
}

// A band's gain, the largest lift and the amount go up / down; the splits, the zone's two lines, the attack and
// the release left / right.
bool MbGraphs::markUpDown(int mark) const {
    return (mark >= mGain1 && mark < mDown) || mark == mMaxLift || mark == mAmount;
}

int MbGraphs::bands() const {
    return std::min(kMaxBands, std::max(1, static_cast<int>(std::lround(values_[pDensBands]))));
}

double MbGraphs::split(int k) const { return values_[static_cast<size_t>(pDensSplit1 + k)]; }

double MbGraphs::edgeHz(int b) const { return b <= 0 ? kLoHz : b >= bands() ? kHiHz : split(b - 1); }

double MbGraphs::ratio() const { return 1.0 + 3.0 * values_[pDensAmount] / 100.0; }

// The stage's rule (DeckProcessor's density bank): a band more than "down above" over its own recent level is
// turned down, one more than "lift below" under it is lifted - by the ratio's share of what is beyond, the lift at
// most "maximum lift".
double MbGraphs::gainAt(double x) const {
    const double k = 1.0 - 1.0 / ratio(), down = values_[pDensDownAbove], lift = values_[pDensLiftBelow];
    if (x > down) return -std::min(24.0, (x - down) * k);
    if (x < -lift) return std::min(values_[pDensMaxLift], (-lift - x) * k);
    return 0.0;
}

double MbGraphs::hitReduction() const { return kHitDb * (1.0 - 1.0 / ratio()); }

std::wstring MbGraphs::valueText(int j) const {
    switch (j) {
    case 0:
        return number(values_[pDensAmount], 0, L" %");
    case 1:
        return number(values_[pDensDownAbove], 1, L" dB");
    case 2:
        return number(values_[pDensLiftBelow], 1, L" dB");
    default:
        return number(values_[pDensMaxLift], 1, L" dB");
    }
}

void MbGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[4] = {L"Amount", L"Down above", L"Lift below", L"Maximum lift"};
    static const wchar_t* const kWidest[4] = {L"100 %", L"12.0 dB", L"20.0 dB", L"12.0 dB"};
    legend_.count = 4;
    for (int j = 0; j < 4; ++j) {
        legend_.param[j] = kLegendParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    bandsTitleW_ = textWidth(dc, L"Bands");
    curveTitleW_ = textWidth(dc, L"Curve per band");
    digitW_ = textWidth(dc, L"7");
    hintW_ = textWidth(dc, kHint);
}

// The bands across the top, the curve, the attack and the release under them (layoutFour). In the bands panel's
// header: the number of bands behind the title, its - and + and its reset icon. The curve's four values have a
// legend in its header; the attack and the release an icon in their corner.
void MbGraphs::layout(int w, int h) {
    layoutFour(w, h, panel_);
    const RECT& p = panel_[0];
    const int rowH = px(18), top = static_cast<int>(p.top) + px(3) - (rowH - textH_) / 2;
    const int valueX = static_cast<int>(p.left) + px(6) + bandsTitleW_ + px(8);
    const int minusX = valueX + digitW_ + px(8), plusX = minusX + px(18) + px(4), iconX = plusX + px(18) + px(4);
    bandsItem_ = {p.left, top, minusX, top + rowH};
    steppers_.push_back({RECT{minusX, top + px(1), minusX + px(18), top + rowH - px(1)}, pDensBands, -1, L"\x2212"});
    steppers_.push_back({RECT{plusX, top + px(1), plusX + px(18), top + rowH - px(1)}, pDensBands, 1, L"+"});
    icons_.push_back({RECT{iconX, top, iconX + px(22), top + rowH}, pDensBands});
    bandsHeaderBottom_ = std::max(static_cast<int>(p.top) + textH_ + px(5), top + rowH);
    icons_.push_back({cornerIcon(panel_[2]), pDensAttack});
    icons_.push_back({cornerIcon(panel_[3]), pDensRelease});
    // the curve's legend: in the header over the square graph, or in a column beside it - whichever leaves the
    // graph larger
    const RECT& c = panel_[1];
    const size_t before = icons_.size();
    layoutLegend(legend_, c, curveTitleW_, 0);
    const int width = static_cast<int>(c.right - c.left) - 2 * px(8), bottom = static_cast<int>(c.bottom) - px(8);
    const int under = std::min(width, bottom - (legend_.headerBottom + px(5)));
    const int beside = std::min(width - legendColumnWidth(legend_) - px(8), bottom - (static_cast<int>(c.top) + textH_ + px(7)));
    if (beside > under) {
        icons_.resize(before);
        layoutLegendColumn(legend_, c);
    }
}

// The rectangles do not overlap (the tooltip control keeps the tool the mouse is in). The reset icons that stand
// beside a split's or a band gain's value in the bands graph have no tooltip of their own: they lie in the graph.
void MbGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kBands =
        L"The bands of the MB Compressor over the music's spectrum (grey). The dashed upright lines are the splits "
        L"between the bands: drag one left or right. The orange line in each band is its fixed gain: drag it up or "
        L"down. The thick line is the band's gain at this moment - its fixed gain plus what the stage does: under the "
        L"orange line the band is turned down, above it lifted. The number of bands is set with - and + in the top "
        L"line.\n\nThe mouse wheel steps the gain of the band under the pointer, or the split the pointer is on. A "
        L"small arrow beside a value that was changed sets it back.";
    static const wchar_t* const kCurve =
        L"What the stage does to one band: the band's level against its own recent level (to the right) and what "
        L"comes out (up). In the grey zone in the middle nothing happens. A band that rises further than Down above "
        L"is turned down; one that falls further than Lift below is lifted, at most by Maximum lift. Amount is how "
        L"strongly: the slope outside the zone, 1:1 at 0 % to 4:1 at 100 %. The white dots are the bands now.\n\n"
        L"Drag the two upright lines left or right and the two level lines up or down: Amount's lies where the "
        L"curve ends on the right (lower is stronger), Maximum lift's where it begins on the left (higher lifts "
        L"more). The legend names the values; the arrow behind a value sets it back.";
    static const wchar_t* const kCorner[2] = {
        L"Attack (1 .. 200 ms): how fast a band is turned down - a hit 10 dB over Down above in its first 300 ms. "
        L"Dashed: the hit as it comes in; the line: what comes out. Shorter catches the hit sooner, longer lets it "
        L"through.\n\nSet it here: drag the mark, or turn the mouse wheel over this graph for one step of 1 ms. The "
        L"arrow in the corner sets it back.",
        L"Release (20 .. 2000 ms): how fast a band comes back after that hit. Short is lively but can sound restless; "
        L"long is smooth.\n\nSet it here: drag the mark, or turn the mouse wheel over this graph for one step of "
        L"10 ms. The arrow in the corner sets it back.",
    };
    const RECT& p = panel_[0];
    const RECT& bandsIcon = icons_[0].rect;
    out.push_back({RECT{p.left, bandsHeaderBottom_, p.right, p.bottom}, kBands, -1, -1});
    out.push_back({RECT{p.left, p.top, bandsIcon.left, bandsHeaderBottom_}, nullptr, pDensBands, -1});
    out.push_back({RECT{bandsIcon.right, p.top, p.right, bandsHeaderBottom_}, kBands, -1, -1});
    legendTips(legend_, panel_[1], kCurve, out);
    for (int k = 0; k < 2; ++k) {
        const RECT& q = panel_[2 + k];
        const RECT& icon = icons_[static_cast<size_t>(1 + k)].rect;
        out.push_back({RECT{q.left, q.top, icon.left, q.bottom}, kCorner[k], -1, -1});
        out.push_back({RECT{icon.left, icon.bottom, q.right, q.bottom}, kCorner[k], -1, -1});
    }
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

// The panels, their titles, the legend's texts, the curve per band, the attack and release graphs. The bands graph
// itself is drawn with the audio: its lines lie over the spectrum (renderLayer, paintLive).
void MbGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    static const wchar_t* const kTitle[kPanels] = {L"Bands", L"Curve per band", L"Attack", L"Release"};
    const bool on = values_[pDensOn] >= 0.5;
    const double down = values_[pDensDownAbove], lift = values_[pDensLiftBelow], maxLift = values_[pDensMaxLift];
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
            q.t = i == 0 ? bandsHeaderBottom_ + px(5) : i == 1 && !legend_.beside ? legend_.headerBottom + px(5) : p.top + th + px(7);
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        if (legend_.beside) plot_[1].l = legend_.bounds.right + px(8); // the curve: right of its legend
        // ---- 1. the bands: the number in the header (the graph: renderLayer, paintLive) ----
        plot_[0].logX = true;
        plot_[0].x0 = kLoHz;
        plot_[0].x1 = kHiHz;
        plot_[0].y0 = kGainLo;
        plot_[0].y1 = kGainHi;
        {
            const RECT& p = panel_[0];
            const int y = static_cast<int>(bandsItem_.top + (bandsItem_.bottom - bandsItem_.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(p.left) + px(6) + bandsTitleW_ + px(8), y, std::to_wstring(bands()), kOrange, TA_LEFT});
            if (p.right - px(6) - hintW_ > icons_[0].rect.right + px(12))
                all.push_back({static_cast<int>(p.right) - px(6), static_cast<int>(p.top) + px(3),
                               on ? kHint : L"the MB Compressor is off", kDim, TA_RIGHT});
        }
        // ---- 2. the curve per band: level in against level out, both to the band's own recent level ----
        {
            Plot& q = plot_[1];
            q.x0 = q.y0 = kCurveLo;
            q.x1 = q.y1 = kCurveHi;
            const double side = std::max(1.0, std::min(q.r - q.l, q.b - q.t)); // one scale both ways: square, centred
            q.l += (q.r - q.l - side) / 2;
            q.r = q.l + side;
            q.t += (q.b - q.t - side) / 2;
            q.b = q.t + side;
            const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
            Pen dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.5f * lw);
            dots.SetDashStyle(DashStyleDot);
            dash.SetDashStyle(DashStyleDash);
            curve.SetLineJoin(LineJoinRound);
            SolidBrush zone(col(kInput, 26));
            const double xDown = q.X(down), xLift = q.X(-lift);
            const double capIn = kCurveLo + kMaxLiftMarkDb, amountIn = kCurveHi - kAmountMarkDb;
            const double yDown = q.Y(down), yLift = q.Y(-lift), yCap = q.Y(capIn + maxLift);
            const double xAmount = q.X(amountIn), yAmount = q.Y(amountIn + gainAt(amountIn));
            // The names (client, 2026-11-13: the amount a level line with its name at the far left; the largest lift
            // a level line with its name at the far right; "down above" left of its line - first at the bottom,
            // then, after seeing it under "lift below": "left of the line but all the way up"), off the curve and
            // the bands' dots and off each other: "down above" at the top and "lift below" at the bottom, each left
            // of its own line (right of it where the graph ends, and "lift below" also where it would reach the
            // largest lift's mark); the amount's under its line - which lies in the top of the graph, at the height
            // where the curve ends - and under "down above" where the two would meet; the largest lift's over its
            // line - which lies low, at the height where the curve begins - and over "lift below" where the two
            // would meet. Each name has its value behind it (client, 2026-11-13: "put the set values behind the
            // text") - in a small graph, where the two do not fit beside the line, the name alone (the legend beside
            // the graph has the value).
            enum { nAmount, nDown, nLift, nCap, kNames }; // the legend's order
            std::wstring text[kNames];
            int textW[kNames];
            auto fits = [&](int j, int w) {
                switch (j) {
                case nDown:
                    return xDown - px(5) - w >= q.l - px(4) || xDown + px(5) + w <= q.r + px(4);
                case nLift:
                    return xLift - px(5) - w >= q.X(capIn) + px(8) || xLift + px(5) + w <= q.r + px(4);
                default:
                    return w <= q.r - q.l - px(8);
                }
            };
            for (int j = 0; j < kNames; ++j) {
                text[j] = std::wstring(legend_.name[j]) + L" " + valueText(j);
                textW[j] = textWidth(text[j]);
                if (!fits(j, textW[j])) {
                    text[j] = legend_.name[j];
                    textW[j] = legend_.nameW[j];
                }
            }
            const int bottomRow = static_cast<int>(q.b) - th - px(2);
            const bool downRight = xDown - px(5) - textW[nDown] < q.l - px(4);
            const bool liftRight = xLift - px(5) - textW[nLift] < q.X(capIn) + px(8);
            std::vector<RECT> name(kNames);
            auto place = [&](int j, double x, int y, bool leftAligned) {
                const int at = static_cast<int>(x);
                name[j] = leftAligned ? RECT{at, y, at + textW[j], y + th} : RECT{at - textW[j], y, at, y + th};
            };
            place(nAmount, q.l + px(4), static_cast<int>(yAmount) + px(3), true);
            place(nDown, xDown + (downRight ? px(5) : -px(5)), static_cast<int>(q.t) + px(1), downRight);
            place(nLift, xLift + (liftRight ? px(5) : -px(5)), bottomRow, liftRight);
            place(nCap, q.r - px(4), static_cast<int>(yCap) - th - px(3), false);
            RECT both;
            if (IntersectRect(&both, &name[nAmount], &name[nDown])) OffsetRect(&name[nAmount], 0, name[nDown].bottom - name[nAmount].top);
            if (IntersectRect(&both, &name[nCap], &name[nLift])) OffsetRect(&name[nCap], 0, name[nLift].top - name[nCap].bottom);
            // an orange line gives way to a name it would run through (lineAround)
            auto dashed = [&](bool upright, double at) {
                lineAround(g, dash, upright, at, upright ? q.t : q.l, upright ? q.b : q.r, name);
            };
            g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                            static_cast<REAL>(q.b - q.t + 2)));
            g.FillRectangle(&zone, static_cast<REAL>(xLift), static_cast<REAL>(q.t), static_cast<REAL>(xDown - xLift),
                            static_cast<REAL>(q.b - q.t)); // nothing happens here
            g.DrawLine(&dots, pt(q.l, q.b), pt(q.r, q.t)); // 1:1
            dashed(true, xDown);
            dashed(true, xLift);
            dashed(false, std::floor(yAmount) + 0.5);
            dashed(false, std::floor(yCap) + 0.5);
            std::vector<PointF> line;
            for (double x = kCurveLo; x <= kCurveHi + 0.01; x += 0.25) line.push_back(pt(q.X(x), q.Y(x + gainAt(x))));
            g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
            g.ResetClip();
            drawMark(g, mDown, xDown, yDown);
            drawMark(g, mLift, xLift, yLift);
            drawMark(g, mMaxLift, q.X(capIn), yCap);
            drawMark(g, mAmount, xAmount, yAmount);
            for (int j = 0; j < kNames; ++j)
                all.push_back({static_cast<int>(name[j].left), static_cast<int>(name[j].top), text[j], kOrange, TA_LEFT});
        }
        for (int j = 0; j < legend_.count; ++j) {
            const RECT& item = legend_.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, valueText(j), kOrange, TA_RIGHT});
        }
        // ---- 3 and 4. the attack and the release ----
        const double reduce = hitReduction();
        drawAttack(g, plot_[2], mAttack, kHitDb, reduce, values_[pDensAttack], kAttackSpanMs, 100.0, L"300 ms", all);
        drawRelease(g, plot_[3], mRelease, reduce, values_[pDensRelease], kReleaseFromMs, kReleaseToMs, 500.0, 2000.0,
                    L"2 s", all);
    }
    labels(dc, all);
}

// The bands graph's still parts - the splits, each band's fixed gain, their marks - lie over the spectrum, which
// moves: they go into the strip's layer. Their texts are written after it (bandNames_), and a value that differs
// from its reset value gets its reset icon beside it (moreIcons_).
void MbGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    bandNames_.clear();
    for (int i = 0; i < mDown; ++i) mark_[static_cast<size_t>(i)] = POINT{-10000, -10000}; // those of bands not in use
    const Plot& q = plot_[0];
    const int n = bands(), th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    auto edge = [&](int b) { return q.X(edgeHz(b)); };
    Pen dots(col(kGrid), lw), dash(col(kOrange), lw), fixed(col(kOrange), 1.5f * lw);
    dots.SetDashStyle(DashStyleDot);
    dash.SetDashStyle(DashStyleDash);
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    g.DrawLine(&dots, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
    for (int k = 0; k + 1 < n; ++k) g.DrawLine(&dash, pt(row(edge(k + 1)), q.t), pt(row(edge(k + 1)), q.b));
    for (int b = 0; b < n; ++b) {
        const double y = row(q.Y(values_[static_cast<size_t>(pDensGain1 + b)]));
        g.DrawLine(&fixed, pt(edge(b) + px(3), y), pt(edge(b + 1) - px(3), y));
    }
    g.ResetClip();
    HDC dc = GetDC(nullptr);
    HGDIOBJ oldFont = dc ? SelectObject(dc, font_) : nullptr;
    auto width = [&](const std::wstring& s) { return dc ? textWidth(dc, s.c_str()) : static_cast<int>(s.size()) * px(7); };
    const int iconW = px(18);
    // the splits: a mark at the top of the line, the frequency beside it - right of the line, or left of it when
    // the band there is the wider one
    for (int k = 0; k + 1 < n; ++k) {
        const int param = pDensSplit1 + k;
        const double x = edge(k + 1);
        drawMark(g, mSplit1 + k, x, q.t + px(7));
        const std::wstring s = hzText(split(k));
        const int need = width(s) + (lit(param) ? iconW + px(2) : 0);
        const double roomRight = edge(k + 2) - x - px(14), roomLeft = x - edge(k) - px(14);
        if (need > std::max(roomRight, roomLeft)) continue;
        const bool right = roomRight >= need;
        const int tx = static_cast<int>(right ? x + px(8) : x - px(8) - need);
        bandNames_.push_back({tx, static_cast<int>(q.t) + px(1), s, kOrange, TA_LEFT});
        if (lit(param)) {
            const int ix = tx + width(s) + px(2);
            moreIcons_.push_back({RECT{ix, static_cast<LONG>(q.t) - px(1), ix + iconW, static_cast<LONG>(q.t) - px(1) + px(18)}, param});
        }
    }
    // the bands: the number at the bottom, a mark on the fixed gain's line and - when it is not 0, or differs from
    // its reset value - the gain over the mark (under it near the top, where the splits' texts are)
    for (int b = 0; b < n; ++b) {
        const int param = pDensGain1 + b;
        const double gain = values_[static_cast<size_t>(param)], x0 = edge(b), x1 = edge(b + 1), xc = (x0 + x1) / 2;
        const double y = q.Y(gain);
        drawMark(g, mGain1 + b, xc, y);
        bandNames_.push_back({static_cast<int>(xc), static_cast<int>(q.b) - th - px(2), std::to_wstring(b + 1), kDim, TA_CENTER});
        if (gain == 0.0 && !lit(param)) continue;
        const std::wstring s = number(gain, 1, L" dB", true);
        const int tw = width(s), need = tw + (lit(param) ? iconW + px(2) : 0);
        if (need > x1 - x0 - px(8)) continue;
        const int ty = static_cast<int>(y - th - px(8) < q.t + th + px(6) ? y + px(8) : y - th - px(8));
        const int tx = static_cast<int>(xc - need / 2.0);
        bandNames_.push_back({tx, ty, s, kOrange, TA_LEFT});
        if (lit(param)) {
            const int ix = tx + tw + px(2), iy = ty - (px(18) - th) / 2;
            moreIcons_.push_back({RECT{ix, iy, ix + iconW, iy + px(18)}, param});
        }
    }
    if (dc) {
        SelectObject(dc, oldFont);
        ReleaseDC(nullptr, dc);
    }
}

// The parts that move with the audio: the spectrum behind the bands, what each band is turned down or lifted at
// this moment, and a dot per band on the curve.
void MbGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    const DeckMeters* m = meters();
    const Plot& q = plot_[0];
    const int n = bands(), th = textH_;
    auto edge = [&](int b) { return q.X(edgeHz(b)); };
    auto fixedGain = [&](int b) { return values_[static_cast<size_t>(pDensGain1 + b)]; };
    auto nowGain = [&](int b) { // what the stage does to band b now, dB (0 without audio)
        return playing && m && b < m->bands ? std::max(-24.0, std::min(12.0, m->densityDb[b])) : 0.0;
    };
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
        if (playing) { // from the fixed gain's line to the gain now: turned down (orange) or lifted
            for (int b = 0; b < n; ++b) {
                const double dyn = nowGain(b);
                if (std::fabs(dyn) < 0.05) continue;
                const double y0 = q.Y(fixedGain(b)), y1 = q.Y(fixedGain(b) + dyn), x0 = edge(b) + px(3), x1 = edge(b + 1) - px(3);
                polygon(dc, {ipt(x0, y0), ipt(x1, y0), ipt(x1, y1), ipt(x0, y1)},
                        dyn < 0.0 ? blend(kOrange, kPanel, 0.36) : blend(kTeal, kPanel, 0.32));
            }
        }
        if (playing) { // the gain now: the thick line (the layer's marks and thin lines come over it)
            for (int b = 0; b < n; ++b) {
                const double y = q.Y(fixedGain(b) + nowGain(b));
                polyline(dc, {ipt(edge(b) + px(3), y), ipt(edge(b + 1) - px(3), y)}, kTeal, std::max(2, px(3)));
            }
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(9), static_cast<LONG>(q.t) - px(9), static_cast<LONG>(q.r) + px(9),
                        static_cast<LONG>(q.b) + px(9)});
    labels(dc, bandNames_);
    if (!playing)
        text(dc, (q.l + q.r) / 2, q.b - 2 * th - px(8), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);

    // the curve: where each band is now. The stage reports a band's gain, not its level: the dot stands where on
    // the curve that gain belongs - a band that is left alone in the middle of the zone, small.
    if (playing && m) {
        using namespace Gdiplus;
        const Plot& c = plot_[1];
        const double k = 1.0 - 1.0 / ratio(), down = values_[pDensDownAbove], lift = values_[pDensLiftBelow];
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush white(col(RGB(250, 250, 250))), idle(col(kInput, 160));
        std::vector<Label> numbers;
        for (int b = 0; b < n; ++b) {
            const double dyn = nowGain(b);
            const bool busy = k > 1e-6 && std::fabs(dyn) >= 0.05;
            const double x = !busy ? 0.0 : std::min(kCurveHi, std::max(kCurveLo, dyn < 0.0 ? down - dyn / k : -lift - dyn / k));
            const double cx = c.X(x), cy = c.Y(x + (busy ? dyn : 0.0)), r = px(busy ? 34 : 22) / 10.0;
            g.FillEllipse(busy ? &white : &idle, static_cast<REAL>(cx - r), static_cast<REAL>(cy - r), static_cast<REAL>(2 * r),
                          static_cast<REAL>(2 * r));
            if (busy) numbers.push_back({static_cast<int>(cx), static_cast<int>(cy) + px(5), std::to_wstring(b + 1), kText, TA_CENTER});
        }
        labels(dc, numbers);
    }
}

// The lines are taken anywhere along them, not only by their marks: in the bands graph a split (the nearest within a
// few pixels) and else the fixed gain's line of the band under the point; in the curve the two upright lines and
// the two level lines.
int MbGraphs::lineAt(POINT p) const {
    const long reach = px(5);
    const Plot& q = plot_[0];
    const int n = bands();
    if (p.x >= q.l - 2 && p.x <= q.r + 2 && p.y >= q.t - 2 && p.y <= q.b + 2) {
        int best = -1;
        long bestD = reach + 1;
        for (int k = 0; k + 1 < n; ++k) {
            const long d = std::labs(p.x - std::lround(q.X(split(k))));
            if (d < bestD) {
                bestD = d;
                best = mSplit1 + k;
            }
        }
        if (best >= 0) return best;
        for (int b = 0; b < n; ++b) {
            if (p.x < q.X(edgeHz(b)) || p.x > q.X(edgeHz(b + 1))) continue;
            if (std::labs(p.y - std::lround(q.Y(values_[static_cast<size_t>(pDensGain1 + b)]))) <= reach) return mGain1 + b;
        }
        return -1;
    }
    const Plot& c = plot_[1];
    if (p.x >= c.l - 2 && p.x <= c.r + 2 && p.y >= c.t - 2 && p.y <= c.b + 2) {
        if (std::labs(p.x - std::lround(c.X(values_[pDensDownAbove]))) <= reach) return mDown;
        if (std::labs(p.x - std::lround(c.X(-values_[pDensLiftBelow]))) <= reach) return mLift;
        const double amountIn = kCurveHi - kAmountMarkDb;
        if (std::labs(p.y - std::lround(c.Y(kCurveLo + kMaxLiftMarkDb + values_[pDensMaxLift]))) <= reach) return mMaxLift;
        if (std::labs(p.y - std::lround(c.Y(amountIn + gainAt(amountIn)))) <= reach) return mAmount;
    }
    return -1;
}

bool MbGraphs::dragValue(int mark, POINT p, double& v) const {
    const Plot& c = plot_[1];
    if (mark >= 0 && mark < mGain1) { // a split: the owner's rule keeps the splits in order and apart
        v = plot_[0].fromX(p.x);
        return true;
    }
    if (mark >= mGain1 && mark < mDown) {
        v = plot_[0].fromY(p.y);
        return true;
    }
    switch (mark) {
    case mDown:
        v = c.fromX(p.x);
        return true;
    case mLift:
        v = -c.fromX(p.x);
        return true;
    case mMaxLift: // its level line lies this far over the 1:1 line at the mark: where the curve begins when the
                   // lift is at its largest there
        v = c.fromY(p.y) - (kCurveLo + kMaxLiftMarkDb);
        return true;
    case mAmount: { // the ratio that brings the curve's right end to the mouse (1:1 at 0 %, 4:1 at 100 %)
        const double in = kCurveHi - kAmountMarkDb, down = values_[pDensDownAbove], over = in - down;
        if (over < 0.5) return false; // "down above" at the graph's end: nothing to take hold of
        const double want = c.fromY(p.y) - down;
        const double r = want <= over / 4.0 ? 4.0 : std::max(1.0, over / want);
        v = (r - 1.0) / 3.0 * 100.0;
        return true;
    }
    case mAttack:
        v = plot_[2].fromX(p.x) - kHitAtMs;
        return true;
    case mRelease:
        v = plot_[3].fromX(p.x);
        return true;
    default:
        return false;
    }
}

// Where the wheel steps a value: over the attack's or the release's graph, over an item of the curve's legend, over
// the number of bands in the first panel's header, and in the bands graph the split the pointer is on or else the
// gain of the band under it.
int MbGraphs::wheelParam(POINT p) const {
    if (PtInRect(&panel_[2], p)) return pDensAttack;
    if (PtInRect(&panel_[3], p)) return pDensRelease;
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return legend_.param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    if (PtInRect(&bandsItem_, p)) return pDensBands;
    for (const Stepper& s : steppers_)
        if (PtInRect(&s.rect, p)) return s.param;
    const Plot& q = plot_[0];
    if (p.x < q.l || p.x > q.r || p.y < q.t || p.y > q.b) return -1;
    const int n = bands();
    for (int k = 0; k + 1 < n; ++k)
        if (std::labs(p.x - std::lround(q.X(split(k)))) <= px(8)) return pDensSplit1 + k;
    for (int b = 0; b < n; ++b)
        if (p.x >= q.X(edgeHz(b)) && p.x <= q.X(edgeHz(b + 1))) return pDensGain1 + b;
    return -1;
}

// One wheel notch is one step of the value. The attack and the release go by the parameter's own step (1 ms, 10 ms)
// and a split to the next value its slider has: on these logarithmic scales one slider position often is the same
// value. The others are on linear scales, where a position is a step.
double MbGraphs::stepped(int param, int steps) const {
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    if (param == pDensAttack || param == pDensRelease)
        return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
    if (param >= pDensSplit1 && param <= pDensSplit6) {
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
