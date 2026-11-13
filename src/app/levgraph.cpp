// SPDX-License-Identifier: GPL-3.0-or-later
#include "levgraph.h"

#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

// the level-and-gain graph: dB to the target, the last 30 s (the 0.1 s points of the deck's history)
const double kLo = -24.0, kHi = 18.0;
const int kTimePoints = 300;
const double kSeconds = kTimePoints / 10.0;
const int kSlowPoints = 30;         // the 3 s level of what comes out
const double kSilenceMarkAt = 0.24; // "after silence": its mark stands at this part of the width, from the left
const double kSilenceHoldDb = -2.0; // where its mark stands at "hold": under the target's line
// the three small graphs: an example difference closing, on one logarithmic time axis
const double kTimeLo = 0.15, kTimeHi = 40.0;
const double kFastDb = 10.0; // the attack and the release are seconds per 10 dB
const double kSlowDb = 3.0;  // the speed: a difference that is not yet "clearly" too loud (deck.cpp: more than 3 dB)

const int kCornerParam[3] = {pLevAttack, pLevRelease, pLevSpeed};
const int kLegendParam[5] = {pLevRange, pLevWindow, pLevHold, pLevQuietWait, pLevSilence};

} // namespace

bool LevellerGraphs::owns(int param) const {
    for (int p : kCornerParam)
        if (p == param) return true;
    for (int p : kLegendParam)
        if (p == param) return true;
    return false;
}

int LevellerGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pLevRange,   pLevWindow, pLevHold,    pLevQuietWait,
                                         pLevSilence, pLevAttack, pLevRelease, pLevSpeed};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

// The level lines go up / down; the quiet wait, the attack, the release and the speed are times: left / right.
bool LevellerGraphs::markUpDown(int mark) const {
    return mark == mRange || mark == mWindow || mark == mHold || mark == mSilence;
}

void LevellerGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[5] = {L"Range", L"Window", L"Hold below", L"Quiet wait", L"After silence"};
    static const wchar_t* const kWidest[5] = {L"15.0 dB", L"3.0 dB", L"20 LU", L"10.0 s", L"15.0 dB"};
    legend_.count = 5;
    for (int j = 0; j < 5; ++j) {
        legend_.param[j] = kLegendParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"Level and gain (off)");
    numberW_ = textWidth(dc, L"quiet   +15.0 dB");
}

// As the Compressor tab: the graph with the audio across the top, the three small ones in a row under it; all four
// side by side in a wide strip (layoutFour). The reset icons of the attack, the release and the speed stand in
// their panels' title line; the legend of the first panel in its header, off the graph.
void LevellerGraphs::layout(int w, int h) {
    layoutFour(w, h, panel_);
    for (int k = 0; k < 3; ++k) icons_.push_back({cornerIcon(panel_[1 + k]), kCornerParam[k]});
    layoutLegend(legend_, panel_[0], titleW_, numberW_);
}

// The rectangles do not overlap (the tooltip control keeps the tool the mouse is in): the first panel is three
// around its legend (legendTips); a panel with an icon in its corner is two rectangles around the icon; then the
// legend's items and the eight icons.
void LevellerGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kLevel =
        L"The last 30 seconds of this deck, in dB to the target (the dotted line). Grey: the music as it would come "
        L"out without the leveller. Orange: the leveller's gain - above the target's line it turns the music up, "
        L"under it down. The line: the level that comes out, its average over 3 seconds - what the gain brings to "
        L"the target. The number is the gain now.\n\nFive values are set here. Range: the two outer lines - the gain "
        L"stays between them. Window: the band around the target - while the level stays in it the gain rests. Hold "
        L"below: music under this line is left alone (fades, pauses). Quiet wait: the upright line near the right "
        L"edge, this long before now - a quiet part that stays that long is levelled after all; drag it left or "
        L"right. After silence: the line the gain goes to after 2 s of silence, so the next song starts there; all "
        L"the way down, under the target's line = hold (the gain stays where it was).\n\nDrag a line anywhere along "
        L"it, or its mark; where two lines lie on each other, the one whose name is nearer is taken. The legend names "
        L"the values; the arrow behind a value sets it back.";
    static const wchar_t* const kCorner[3] = {
        L"Attack (0.2 .. 20 s): music that is clearly too loud - here 10 dB (dashed), a loud start after a quiet "
        L"intro or a break - and how it comes out (the line): the gain goes down 10 dB in this time. Short: the loud "
        L"part is down at once. Long: a gentler turn of the knob. Never slower than Speed.\n\nSet it here: drag the "
        L"upright line, or turn the mouse wheel over this graph for one step of 0.1 s. The arrow in the corner sets "
        L"it back.",
        L"Release (0.2 .. 20 s): music that is clearly too soft - here 10 dB (dashed), a soft intro, a quiet song "
        L"after a loud one, a new song after a pause - and how it comes out (the line): the gain goes up 10 dB in "
        L"this time. Short: on level sooner. Long: a gentler turn of the knob. Never slower than Speed.\n\nSet it "
        L"here: drag the upright line, or turn the mouse wheel over this graph for one step of 0.1 s. The arrow in "
        L"the corner sets it back.",
        L"Speed (0.1 .. 6 dB/s): how fast the gain moves while the music is only a little off - here 3 dB too loud "
        L"(dashed) and how it comes out (the line). Further left is faster. Inside the Window the gain only drifts, "
        L"at a tenth of this.\n\nSet it here: drag the upright line, or turn the mouse wheel over this graph for one "
        L"step of 0.1 dB/s. The arrow in the corner sets it back.",
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

// The level the chain brings the output to: the target, louder by the input gain and softer by the output gain.
double LevellerGraphs::reference() const { return values_[pTarget] + values_[pInputGain] + values_[pOutputGain]; }

double LevellerGraphs::silenceDb() const { return values_[pLevSilence] < 0.0 ? kSilenceHoldDb : values_[pLevSilence]; }

double LevellerGraphs::silenceMarkX() const { return plot_[0].l + kSilenceMarkAt * (plot_[0].r - plot_[0].l); }

// The panels, their titles, the legend's texts and the three small graphs. The first graph itself is drawn with the
// audio: its lines lie over the level (renderLayer, paintLive).
void LevellerGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    static const wchar_t* const kTitle[kPanels] = {L"Level and gain", L"Attack: 10 dB too loud", L"Release: 10 dB too soft",
                                                   L"Speed: 3 dB too loud"};
    const bool on = values_[pLevOn] >= 0.5;
    const double speed = values_[pLevSpeed], attack = values_[pLevAttack], release = values_[pLevRelease];
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
                           i == 0 && !on ? std::wstring(L"Level and gain (off)") : kTitle[i], kDim, TA_LEFT});
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0 ? legend_.headerBottom + px(5) : p.top + th + px(7); // the first: under its legend
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        // ---- 1. level and gain: its scales (seconds ago to the right, dB to the target up) and its legend ----
        plot_[0].x0 = -kSeconds;
        plot_[0].x1 = 0.0;
        plot_[0].y0 = kLo;
        plot_[0].y1 = kHi;
        const std::wstring value[5] = {number(values_[pLevRange], 1, L" dB"), number(values_[pLevWindow], 1, L" dB"),
                                       number(values_[pLevHold], 0, L" LU"), number(values_[pLevQuietWait], 1, L" s"),
                                       values_[pLevSilence] < 0.0 ? std::wstring(L"hold")
                                                                  : number(values_[pLevSilence], 1, L" dB")};
        for (int j = 0; j < legend_.count; ++j) {
            const RECT& item = legend_.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, value[j], kOrange, TA_RIGHT});
        }
        // ---- 2, 3 and 4. the attack, the release (never slower than the speed) and the speed ----
        drawRamp(g, plot_[1], mAttack, kFastDb, std::max(kFastDb / attack, speed), attack, number(attack, 1, L" s"), all);
        drawRamp(g, plot_[2], mRelease, -kFastDb, std::max(kFastDb / release, speed), release, number(release, 1, L" s"), all);
        drawRamp(g, plot_[3], mSpeed, kSlowDb, speed, kSlowDb / speed, number(speed, 1, L" dB/s"), all);
    }
    labels(dc, all);
}

// The leveller's gain moves at a steady number of dB per second: the level that comes out is a straight line down
// (or up) to the target - bent here by the logarithmic time axis, which gives 0.2 s and 20 s both their room.
void LevellerGraphs::drawRamp(Gdiplus::Graphics& g, Plot& q, int mark, double fromDb, double rate, double markS,
                              const std::wstring& value, std::vector<Label>& all) {
    using namespace Gdiplus;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    Pen grid(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw), inputDash(col(kInput), 1.5f * lw);
    dash.SetDashStyle(DashStyleDash);
    inputDash.SetDashStyle(DashStyleDash);
    curve.SetLineJoin(LineJoinRound);
    SolidBrush orangeFill(col(kOrange, 60));
    const bool loud = fromDb > 0.0;
    const double size = std::fabs(fromDb), reach = size / std::max(rate, 1e-6);
    q.logX = true;
    q.x0 = kTimeLo;
    q.x1 = kTimeHi;
    q.y0 = loud ? -0.2 * size : -1.2 * size;
    q.y1 = loud ? 1.2 * size : 0.2 * size;
    auto level = [&](double s) { return fromDb * std::max(0.0, 1.0 - s / reach); };
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    g.DrawLine(&grid, pt(q.l, q.Y(0)), pt(q.r, q.Y(0))); // the target
    std::vector<double> times;
    for (int i = 0; i <= 160; ++i) times.push_back(kTimeLo * std::pow(kTimeHi / kTimeLo, i / 160.0));
    if (reach > kTimeLo && reach < kTimeHi) times.push_back(reach); // the corner where it arrives
    std::sort(times.begin(), times.end());
    std::vector<PointF> line, area;
    area.push_back(pt(q.l, q.Y(fromDb)));
    for (double s : times) {
        line.push_back(pt(q.X(s), q.Y(level(s))));
        area.push_back(line.back());
    }
    area.push_back(pt(q.r, q.Y(fromDb)));
    g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size())); // what the gain takes off, or adds
    g.DrawLine(&inputDash, pt(q.l, q.Y(fromDb)), pt(q.r, q.Y(fromDb)));
    g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
    const double xm = q.X(markS);
    g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
    for (double s : {1.0, 5.0, 20.0}) g.DrawLine(&grid, pt(q.X(s), q.b - px(4)), pt(q.X(s), q.b));
    g.ResetClip();
    drawMark(g, mark, xm, q.Y(level(markS)));
    const bool right = xm > q.l + 0.6 * (q.r - q.l);
    const int th = textH_, yTicks = static_cast<int>(q.b) - th - px(4);
    all.push_back({static_cast<int>(xm + (right ? -px(5) : px(5))), static_cast<int>(q.t), value, kOrange,
                   static_cast<UINT>(right ? TA_RIGHT : TA_LEFT)});
    static const struct {
        double s;
        const wchar_t* text;
    } kTick[3] = {{1.0, L"1 s"}, {5.0, L"5 s"}, {20.0, L"20 s"}};
    for (const auto& tick : kTick) // not where the mark and its line stand
        if (std::fabs(q.X(tick.s) - xm) > px(22)) all.push_back({static_cast<int>(q.X(tick.s)), yTicks, tick.text, kDim, TA_CENTER});
    all.push_back({static_cast<int>(q.r) - px(2), static_cast<int>(loud ? q.Y(0) - th - px(2) : q.Y(0) + px(2)), L"target",
                   kDim, TA_RIGHT});
}

// The first graph's still parts - the target's line, the window's band, the range's two lines, the hold line, the
// line of "after silence", the quiet wait's upright line, their marks - lie over the level and the gain, which
// move: they go into the strip's layer. Their names are written after it (names_). Level lines and upright lines
// throughout (client, 2026-11-13, while this was being built: "use horizontal and vertical lines here too").
void LevellerGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    names_.clear();
    const Plot& q = plot_[0];
    const double range = values_[pLevRange], window = values_[pLevWindow], hold = values_[pLevHold];
    const bool holds = values_[pLevSilence] < 0.0; // "after silence": the gain stays where it was
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; }; // a level line on a pixel row: sharp
    const double yTarget = row(q.Y(0)), yUp = row(q.Y(range)), yDown = row(q.Y(-range)), yHold = row(q.Y(-hold));
    const double yWinT = row(q.Y(window)), yWinB = row(q.Y(-window)), ySilence = row(q.Y(silenceDb()));
    const double xLeft = q.l + px(7), xRange = q.r - px(9), xWait = q.X(-values_[pLevQuietWait]);
    const double xSilence = silenceMarkX();
    // The names, each at its line: the range's at the far right (over its upper line, and again under its lower
    // one), the window's and "hold below" at the far left beside their marks - "hold below" over its line, the
    // range's lower line often lies close under it -, "after silence" right of its mark over its line, the quiet
    // wait's at the bottom of its upright line, left of it. A line gives way to a name it would run through
    // (lineAround); the lower "Range" is left out where the quiet wait's stretch on the hold line is in its way.
    enum { nRange, nRangeLow, nWindow, nHold, nWait, nSilence, kNames };
    static const wchar_t* const kName[kNames] = {L"Range", L"Range", L"Window", L"Hold below", L"Quiet wait", L"After silence"};
    static const int kWidthOf[kNames] = {0, 0, 1, 2, 3, 4}; // the legend's name widths
    const int above = th + px(2), under = px(2), side = px(8);
    std::vector<RECT> name(kNames);
    auto place = [&](int j, double x, double y, bool leftAligned) {
        const int at = static_cast<int>(x), top = static_cast<int>(y), width = legend_.nameW[kWidthOf[j]];
        name[static_cast<size_t>(j)] = leftAligned ? RECT{at, top, at + width, top + th} : RECT{at - width, top, at, top + th};
    };
    place(nRange, xRange - side, yUp - above, false);
    place(nRangeLow, q.r - px(4), yDown + under, false);
    place(nWindow, xLeft + side, yWinT - above, true);
    place(nHold, xLeft + side, yHold - above, true);
    place(nWait, xWait - px(5), q.b - th - px(2), false);
    place(nSilence, xSilence + side, holds ? ySilence - th / 2.0 : ySilence - above, true);
    const bool onStretch = name[nRangeLow].top - px(2) <= yHold && yHold <= name[nRangeLow].bottom + px(2);
    if (range <= 0.0 || onStretch) name[nRangeLow] = RECT{0, 0, 0, 0};

    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    Pen dots(col(kGrid), lw), dash(col(kOrange), lw), solid(col(kOrange), 3.0f * lw), grid(col(kGrid), lw);
    dots.SetDashStyle(DashStyleDot);
    dash.SetDashStyle(DashStyleDash);
    SolidBrush held(col(kInput, 18));
    g.FillRectangle(&held, static_cast<REAL>(q.l), static_cast<REAL>(yHold), static_cast<REAL>(q.r - q.l),
                    static_cast<REAL>(q.b - yHold)); // under the hold line: left alone
    if (window > 0.0) {
        SolidBrush band(col(kOrange, 40));
        g.FillRectangle(&band, static_cast<REAL>(q.l), static_cast<REAL>(yWinT), static_cast<REAL>(q.r - q.l),
                        static_cast<REAL>(yWinB - yWinT));
        Pen edge(col(kOrange, 150), lw);
        edge.SetDashStyle(DashStyleDot);
        g.DrawLine(&edge, pt(q.l, yWinT), pt(q.r, yWinT));
        g.DrawLine(&edge, pt(q.l, yWinB), pt(q.r, yWinB));
    }
    g.DrawLine(&dots, pt(q.l, yTarget), pt(q.r, yTarget));
    for (double s = 5.0; s < kSeconds - 0.01; s += 5.0) g.DrawLine(&grid, pt(q.X(-s), q.b - px(4)), pt(q.X(-s), q.b));
    lineAround(g, dash, false, yUp, q.l, q.r, name);
    lineAround(g, dash, false, yDown, q.l, q.r, name);
    lineAround(g, dash, false, yHold, q.l, xWait, name);
    g.DrawLine(&solid, pt(xWait, yHold), pt(q.r, yHold)); // the quiet wait: this long under the hold line
    lineAround(g, dash, true, std::floor(xWait) + 0.5, q.t, q.b, name);
    if (!holds) lineAround(g, dash, false, ySilence, q.l, q.r, name);
    g.ResetClip();
    drawMark(g, mRange, xRange, yUp);
    drawMark(g, mWindow, xLeft, yWinT);
    drawMark(g, mHold, xLeft, yHold);
    drawMark(g, mQuietWait, xWait, yHold);
    drawMark(g, mSilence, xSilence, ySilence);
    for (int j = 0; j < kNames; ++j) {
        const RECT& r = name[static_cast<size_t>(j)];
        if (r.right > r.left) names_.push_back({static_cast<int>(r.left), static_cast<int>(r.top), kName[j], kOrange, TA_LEFT});
    }
    const int yTime = static_cast<int>(q.b) - th - px(2);
    names_.push_back({static_cast<int>(q.l) + px(3), yTime, L"30 s ago", kDim, TA_LEFT});
    names_.push_back({static_cast<int>(q.r) - px(3), yTime, L"now", kDim, TA_RIGHT});
    names_.push_back({static_cast<int>(xRange - side), static_cast<int>(yTarget + under), L"target", kDim, TA_RIGHT});
}

// The parts that move with the audio, on a copy of the still picture: the last 30 s of the music without the
// leveller's gain, that gain, and the 3 s level of what comes out - with the lines' layer over them.
void LevellerGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    const DeckMeters* m = meters();
    const bool have = an && m && playing;
    const Plot& q = plot_[0];
    const double ref = reference();
    const int th = textH_;
    {
        const int saved = clip(dc, q);
        if (have) {
            const int first = DeckAnalysis::kHistory - kTimePoints;
            auto power = [&](int index, bool& ok) { // the output's power at a point of the history
                const DeckAnalysis::Point& p = an->at(index);
                ok = p.live && p.outM > kFloorDb + 1.0;
                return ok ? std::pow(10.0, p.outM / 10.0) : 0.0;
            };
            double sum = 0.0; // the output of the last 3 s
            int count = 0;
            for (int k = first - kSlowPoints; k < first; ++k) {
                bool ok = false;
                const double pw = power(k, ok);
                if (ok) sum += pw, ++count;
            }
            std::vector<POINT> music, gainArea;
            std::vector<std::vector<POINT>> gainLine(1), outLine(1);
            const double step = (q.r - q.l) / (kTimePoints - 1.0), yTarget = q.Y(0);
            music.push_back(ipt(q.l, q.b));
            gainArea.push_back(ipt(q.l, yTarget));
            for (int i = 0; i < kTimePoints; ++i) {
                const DeckAnalysis::Point& p = an->at(first + i);
                bool ok = false, okOld = false;
                const double pw = power(first + i, ok), old = power(first + i - kSlowPoints, okOld);
                if (ok) sum += pw, ++count;
                if (okOld) sum -= old, --count;
                const double x = q.l + i * step;
                music.push_back(ipt(x, ok ? q.Y(p.outM - p.leveller - ref) : q.b));
                gainArea.push_back(ipt(x, p.live ? q.Y(p.leveller) : yTarget));
                if (p.live) gainLine.back().push_back(gainArea.back());
                else if (!gainLine.back().empty()) gainLine.emplace_back();
                if (ok && count > 0 && sum > 0.0) outLine.back().push_back(ipt(x, q.Y(10.0 * std::log10(sum / count) - ref)));
                else if (!outLine.back().empty()) outLine.emplace_back();
            }
            music.push_back(ipt(q.r, q.b));
            gainArea.push_back(ipt(q.r, yTarget));
            // The gain's area looks laid over the music: one colour over the panel, another over the music's grey
            // (plain polygons, the second inside the music's outline).
            const COLORREF grey = blend(kInput, kPanel, 0.22);
            polygon(dc, music, grey);
            polygon(dc, gainArea, blend(kOrange, kPanel, 0.34));
            if (HRGN inside = CreatePolygonRgn(music.data(), static_cast<int>(music.size()), ALTERNATE)) {
                const int inner = SaveDC(dc);
                ExtSelectClipRgn(dc, inside, RGN_AND);
                polygon(dc, gainArea, blend(kOrange, grey, 0.34));
                RestoreDC(dc, inner);
                DeleteObject(inside);
            }
            for (const auto& part : gainLine)
                if (part.size() > 1) polyline(dc, part, kOrange, std::max(1, px(1.5)));
            for (const auto& part : outLine)
                if (part.size() > 1) polyline(dc, part, kTeal, std::max(2, px(2)));
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(9), static_cast<LONG>(q.t) - px(9), static_cast<LONG>(q.r) + px(9),
                        static_cast<LONG>(q.b) + px(9)});
    labels(dc, names_);
    if (have) {
        // the gain now, and "quiet" while the music is under the hold line
        const std::wstring now = number(m->levellerDb, 1, L" dB", true);
        const double x = panel_[0].right - px(6), y = panel_[0].top + px(3);
        text(dc, x, y, now, kOrange, TA_RIGHT);
        if (m->flags & mfQuiet) text(dc, x - textWidth(dc, now.c_str()) - px(8), y, L"quiet", kDim, TA_RIGHT);
    } else {
        text(dc, (q.l + q.r) / 2, q.b - th - px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
    }
}

// The lines are taken anywhere along them, not only by their marks. In the three small graphs: the upright line of
// the attack, the release or the speed. In the first graph the nearest one within a few pixels - the quiet wait's
// upright line, "after silence", the hold line, the window's two edges, the range's two lines (in that order where
// they lie together). By default "after silence" lies on the range's upper line: there the one whose mark and name
// are nearer is taken - "after silence" left of the middle between the two marks, the range right of it.
int LevellerGraphs::lineAt(POINT p) const {
    const long reach = px(5), none = reach + 1;
    static const int kUpright[3] = {mAttack, mRelease, mSpeed};
    for (int k = 0; k < 3; ++k) {
        const Plot& s = plot_[1 + k];
        if (p.x < s.l - 2 || p.x > s.r + 2 || p.y < s.t - 2 || p.y > s.b + 2) continue;
        return std::labs(p.x - mark_[static_cast<size_t>(kUpright[k])].x) <= reach ? kUpright[k] : -1;
    }
    const Plot& q = plot_[0];
    if (p.x < q.l - 2 || p.x > q.r + 2 || p.y < q.t - 2 || p.y > q.b + 2) return -1;
    const double range = values_[pLevRange], window = values_[pLevWindow];
    auto off = [&](double db) { return std::labs(p.y - std::lround(q.Y(db))); };
    const long silence = values_[pLevSilence] >= 0.0 ? off(values_[pLevSilence]) : none;
    long d[5] = {std::labs(p.x - std::lround(q.X(-values_[pLevQuietWait]))), silence, off(-values_[pLevHold]),
                 window > 0.0 ? std::min(off(window), off(-window)) : none, std::min(off(range), off(-range))};
    if (silence <= reach && std::labs(silence - off(range)) <= 1) {
        if (p.x > (silenceMarkX() + q.r - px(9)) / 2) d[1] = none;
        else d[4] = off(-range);
    }
    static const int kLine[5] = {mQuietWait, mSilence, mHold, mWindow, mRange};
    int best = -1;
    long bestD = none;
    for (int i = 0; i < 5; ++i) {
        if (d[i] < bestD) {
            bestD = d[i];
            best = kLine[i];
        }
    }
    return best;
}

// Where the wheel steps a value: anywhere over the attack's, the release's or the speed's graph, and over an item
// of the legend (over the first graph itself it could be any of five).
int LevellerGraphs::wheelParam(POINT p) const {
    for (int k = 0; k < 3; ++k)
        if (PtInRect(&panel_[1 + k], p)) return kCornerParam[k];
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return legend_.param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    return -1;
}

// One wheel notch is one step of the value. The times go by the parameter's own step (0.1 s, 0.5 s), not by a
// slider position: on their logarithmic scales one position often is the same value. The others are on linear
// scales, where a position is a step (and the first position of "after silence" is "hold").
double LevellerGraphs::stepped(int param, int steps) const {
    if (param != pLevAttack && param != pLevRelease && param != pLevQuietWait) return GraphStrip::stepped(param, steps);
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
}

bool LevellerGraphs::dragValue(int mark, POINT p, double& v) const {
    const Plot& q = plot_[0];
    switch (mark) {
    case mRange: // either of its two lines: as far from the target's line
    case mWindow:
        v = std::fabs(q.fromY(p.y));
        return true;
    case mHold:
        v = -q.fromY(p.y);
        return true;
    case mSilence: { // under the target's line: hold
        const double db = q.fromY(p.y);
        v = db < -1.0 ? -1.0 : std::max(0.0, db);
        return true;
    }
    case mQuietWait: // seconds back from the right edge
        v = -q.fromX(p.x);
        return true;
    case mAttack:
        v = plot_[1].fromX(p.x);
        return true;
    case mRelease:
        v = plot_[2].fromX(p.x);
        return true;
    case mSpeed: // the time the example difference takes
        v = kSlowDb / plot_[3].fromX(p.x);
        return true;
    default:
        return false;
    }
}

} // namespace ap
