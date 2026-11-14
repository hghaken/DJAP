// SPDX-License-Identifier: GPL-3.0-or-later
#include "autograph.h"

#include "colour.h"
#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

const double kCorrDb = 10.0;           // the corrections' scale: +-10 dB
const double kWantLo = -12.0, kWantHi = 12.0, kAmountAt = 10.0; // the amount's graph; its mark 10 dB from the reference
const double kTimeTo = 90.0;           // the time's graph: 90 s
const double kLoHz = 20.0, kHiHz = 20000.0, kFs = 48000.0;
const Param kGain[4] = {pToneBass, pToneLowMid, pTonePresence, pToneTreble};
const wchar_t* const kName[4] = {L"bass", L"low-mid", L"presence", L"treble"};
const COLORREF kNode[4] = {RGB(120, 175, 255), RGB(205, 150, 255), RGB(255, 190, 110), RGB(130, 230, 160)};
const int kLegendParam[3] = {pAutoAmount, pAutoMax, pAutoTime};

} // namespace

int AutoToneGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pAutoMax, pAutoAmount, pAutoTime};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

void AutoToneGraphs::measure(HDC dc) {
    static const wchar_t* const kNames[3] = {L"Amount", L"Maximum", L"Time"};
    static const wchar_t* const kWidest[3] = {L"100 %", L"9.0 dB", L"60 s"};
    legend_.count = 3;
    for (int j = 0; j < 3; ++j) {
        legend_.param[j] = kLegendParam[j];
        legend_.name[j] = kNames[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"Corrections, last 60 s (off)");
}

void AutoToneGraphs::layout(int w, int h) {
    layoutFour(w, h, panel_);
    layoutLegend(legend_, panel_[0], titleW_, 0);
}

void AutoToneGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kHistory =
        L"The auto tone's corrections of the last 60 seconds, a line per tone section (bass, low-mid, presence, "
        L"treble - the colours of the Tone tab's points): how much it lifts (up) or lowers (down) each section to "
        L"bring the song's balance towards the reference. It holds them while the music is quiet or fading, and "
        L"measures fast in a song's first seconds.\n\nMaximum: the dashed lines - no section is corrected more (the "
        L"treble half as much: the top of an old recording is often its hiss). Drag a line or its mark up or down. "
        L"The legend names the three values; the arrow behind a value sets it back.";
    static const wchar_t* const kAmount =
        L"Amount (0 .. 100 %): the correction against how far the song is from the reference in a section (to the "
        L"right: the song has less there than the reference). At 100 % the whole difference is corrected, at 50 % "
        L"half of it; the maximum cuts it off (the dotted part goes on without it; the treble's maximum is the green "
        L"dotted lines, half as far). The dots are the four sections now.\n\nDrag the mark up or down, or turn the "
        L"mouse wheel over this graph for one step of 5 %.";
    static const wchar_t* const kTime =
        L"Time (2 .. 60 s): how fast the correction follows when the song's balance changes - after this time it "
        L"has come most of the way (63 %). Short follows each part of a song; long keeps one correction for the "
        L"whole song.\n\nDrag the mark left or right, or turn the mouse wheel over this graph for one step of 1 s.";
    static const wchar_t* const kNow =
        L"The auto tone's correction of this moment as an EQ curve: what it adds to the Tone tab's curve. The dotted "
        L"lines are the maximum.";
    legendTips(legend_, panel_[0], kHistory, out);
    out.push_back({panel_[1], kAmount, -1, -1});
    out.push_back({panel_[2], kTime, -1, -1});
    out.push_back({panel_[3], kNow, -1, -1});
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

std::wstring AutoToneGraphs::valueText(int j) const {
    switch (j) {
    case 0:
        return number(values_[pAutoAmount], 0, L" %");
    case 1:
        return number(values_[pAutoMax], 1, L" dB");
    default:
        return number(values_[pAutoTime], 0, L" s");
    }
}

void AutoToneGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    static const wchar_t* const kTitle[kPanels] = {L"Corrections, last 60 s", L"Amount", L"Time", L"Correction now"};
    const bool on = values_[pAutoAmount] > 0.0;
    const double amount = values_[pAutoAmount] / 100.0, max = values_[pAutoMax], time = values_[pAutoTime];
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
                           i == 0 && !on ? std::wstring(L"Corrections, last 60 s (off)") : kTitle[i], kDim, TA_LEFT});
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0 ? legend_.headerBottom + px(5) : p.top + th + px(7);
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        plot_[0].x0 = 0.0;
        plot_[0].x1 = DeckAnalysis::kHistory - 1.0;
        plot_[0].y0 = -kCorrDb;
        plot_[0].y1 = kCorrDb;
        for (int j = 0; j < legend_.count; ++j) {
            const RECT& item = legend_.item[j];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[j], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, valueText(j), kOrange, TA_RIGHT});
        }
        // the sections' names in their colours, under the corrections
        {
            int x = static_cast<int>(plot_[0].l) + px(2);
            for (int s = 0; s < 4; ++s) {
                all.push_back({x, static_cast<int>(plot_[0].b) - th - px(2), kName[s], kNode[s], TA_LEFT});
                x += textWidth(kName[s]) + px(10);
            }
        }
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
        Pen grid(col(kGrid), lw), dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw);
        dots.SetDashStyle(DashStyleDot);
        dash.SetDashStyle(DashStyleDash);
        curve.SetLineJoin(LineJoinRound);
        auto row = [](double y) { return std::floor(y) + 0.5; };
        auto clipTo = [&](const Plot& q) {
            g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                            static_cast<REAL>(q.b - q.t + 2)));
        };
        // ---- 2. the amount ----
        {
            Plot& q = plot_[1];
            q.x0 = kWantLo;
            q.x1 = kWantHi;
            q.y0 = -kCorrDb;
            q.y1 = kCorrDb;
            clipTo(q);
            g.DrawLine(&grid, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
            g.DrawLine(&grid, pt(row(q.X(0)), q.t), pt(row(q.X(0)), q.b));
            g.DrawLine(&dots, pt(q.l, row(q.Y(max))), pt(q.r, row(q.Y(max))));
            g.DrawLine(&dots, pt(q.l, row(q.Y(-max))), pt(q.r, row(q.Y(-max))));
            g.DrawLine(&dots, pt(q.X(kWantLo), q.Y(kWantLo * amount)), pt(q.X(kWantHi), q.Y(kWantHi * amount))); // no maximum
            if (max > 0.0) { // the treble's maximum: half
                Pen half(col(kNode[3], 150), lw);
                half.SetDashStyle(DashStyleDot);
                g.DrawLine(&half, pt(q.l, row(q.Y(max / 2))), pt(q.r, row(q.Y(max / 2))));
                g.DrawLine(&half, pt(q.l, row(q.Y(-max / 2))), pt(q.r, row(q.Y(-max / 2))));
            }
            std::vector<PointF> line;
            for (int i = 0; i <= 96; ++i) {
                const double want = kWantLo + (kWantHi - kWantLo) * i / 96.0;
                line.push_back(pt(q.X(want), q.Y(std::min(max, std::max(-max, want * amount)))));
            }
            g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
            g.ResetClip();
            drawMark(g, mAmount, q.X(kAmountAt), q.Y(kAmountAt * amount));
            const double ym = q.Y(kAmountAt * amount);
            all.push_back({static_cast<int>(q.X(kAmountAt)) - px(9), static_cast<int>(ym - th - px(2) < q.t ? ym + px(4) : ym - th - px(4)),
                           L"Amount " + valueText(0), kOrange, TA_RIGHT});
            all.push_back({static_cast<int>(q.r), static_cast<int>(q.b) - th - px(2), L"less than the reference", kDim, TA_RIGHT});
        }
        // ---- 3. the time ----
        {
            Plot& q = plot_[2];
            q.x0 = 0.0;
            q.x1 = kTimeTo;
            q.y0 = 0.0;
            q.y1 = 1.1;
            clipTo(q);
            g.DrawLine(&grid, pt(q.l, row(q.Y(1.0))), pt(q.r, row(q.Y(1.0))));
            for (double s = 10.0; s < kTimeTo; s += 10.0) g.DrawLine(&grid, pt(row(q.X(s)), q.b - px(4)), pt(row(q.X(s)), q.b));
            std::vector<PointF> line, area;
            area.push_back(pt(q.X(0), q.Y(0)));
            for (int i = 0; i <= 120; ++i) {
                const double t = kTimeTo * i / 120.0;
                line.push_back(pt(q.X(t), q.Y(1.0 - std::exp(-t / time))));
                area.push_back(line.back());
            }
            area.push_back(pt(q.X(kTimeTo), q.Y(0)));
            SolidBrush fill(col(kTeal, 40));
            g.FillPolygon(&fill, area.data(), static_cast<INT>(area.size()));
            g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
            const double xm = q.X(time);
            g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
            g.ResetClip();
            drawMark(g, mTime, xm, q.Y(1.0 - std::exp(-1.0)));
            const bool right = xm > q.l + 0.6 * (q.r - q.l);
            all.push_back({static_cast<int>(xm + (right ? -px(5) : px(5))), static_cast<int>(q.t), valueText(2), kOrange,
                           static_cast<UINT>(right ? TA_RIGHT : TA_LEFT)});
            all.push_back({static_cast<int>(q.r), static_cast<int>(q.b) - th - px(4), L"90 s", kDim, TA_RIGHT});
        }
        // ---- 4. the correction now: its scale and the maximum ----
        {
            Plot& q = plot_[3];
            q.logX = true;
            q.x0 = kLoHz;
            q.x1 = kHiHz;
            q.y0 = -kCorrDb;
            q.y1 = kCorrDb;
            g.DrawLine(&grid, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
            g.DrawLine(&dots, pt(q.l, row(q.Y(max))), pt(q.r, row(q.Y(max))));
            g.DrawLine(&dots, pt(q.l, row(q.Y(-max))), pt(q.r, row(q.Y(-max))));
            for (double hz : {100.0, 1000.0, 10000.0}) g.DrawLine(&dots, pt(row(q.X(hz)), q.t), pt(row(q.X(hz)), q.b));
            all.push_back({static_cast<int>(q.X(100.0)) + px(2), static_cast<int>(q.b) - th - px(2), L"100", kDim, TA_LEFT});
            all.push_back({static_cast<int>(q.X(1000.0)) + px(2), static_cast<int>(q.b) - th - px(2), L"1 k", kDim, TA_LEFT});
            all.push_back({static_cast<int>(q.X(10000.0)) + px(2), static_cast<int>(q.b) - th - px(2), L"10 k", kDim, TA_LEFT});
        }
    }
    labels(dc, all);
}

// The maximum's two lines over the corrections (and the treble's half), with the mark and the name.
void AutoToneGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    layerNames_.clear();
    const Plot& q = plot_[0];
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    const double max = values_[pAutoMax];
    Pen grid(col(kGrid), lw), dash(col(kOrange), lw), half(col(kNode[3], 150), lw);
    dash.SetDashStyle(DashStyleDash);
    half.SetDashStyle(DashStyleDot);
    g.DrawLine(&grid, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
    g.DrawLine(&dash, pt(q.l, row(q.Y(max))), pt(q.r, row(q.Y(max))));
    g.DrawLine(&dash, pt(q.l, row(q.Y(-max))), pt(q.r, row(q.Y(-max))));
    if (max > 0.0) {
        g.DrawLine(&half, pt(q.l, row(q.Y(max / 2))), pt(q.r, row(q.Y(max / 2))));
        g.DrawLine(&half, pt(q.l, row(q.Y(-max / 2))), pt(q.r, row(q.Y(-max / 2))));
    }
    const double x = q.r - px(14), y = row(q.Y(max));
    drawMark(g, mMax, x, y);
    layerNames_.push_back({static_cast<int>(x) - px(9), static_cast<int>(y - th - px(2) < q.t ? y + px(2) : y - th - px(2)),
                           L"Maximum " + valueText(1), kOrange, TA_RIGHT});
}

void AutoToneGraphs::paintLive(HDC dc) {
    const DeckAnalysis* an = analysis();
    const DeckMeters* m = meters();
    const bool playing = live();
    const int th = textH_;
    // ---- 1. the corrections of the last 60 s ----
    const Plot& q = plot_[0];
    if (an) {
        const int saved = clip(dc, q);
        for (int s = 0; s < 4; ++s) {
            std::vector<POINT> line;
            for (int k = 0; k < DeckAnalysis::kHistory; ++k) {
                const DeckAnalysis::Point& p = an->at(k);
                if (!p.live) {
                    if (line.size() > 1) polyline(dc, line, kNode[s], std::max(1, px(1.5)));
                    line.clear();
                    continue;
                }
                line.push_back(ipt(q.X(k), q.Y(p.autoTone[s])));
            }
            if (line.size() > 1) polyline(dc, line, kNode[s], std::max(1, px(1.5)));
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(9), static_cast<LONG>(q.t) - px(9) - th, static_cast<LONG>(q.r) + px(9),
                        static_cast<LONG>(q.b) + px(9)});
    labels(dc, layerNames_);
    if (!m || !playing) {
        text(dc, (q.l + q.r) / 2, q.t + px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
        return;
    }
    // ---- 2. the four sections where they are now ----
    {
        using namespace Gdiplus;
        const Plot& qa = plot_[1];
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetClip(RectF(static_cast<REAL>(qa.l - 1), static_cast<REAL>(qa.t - 1), static_cast<REAL>(qa.r - qa.l + 2),
                        static_cast<REAL>(qa.b - qa.t + 2)));
        const double r = px(40) / 10.0;
        for (int s = 0; s < 4; ++s) {
            SolidBrush dot(col(kNode[s]));
            const double x = qa.X(m->autoToneWant[s]), y = qa.Y(m->autoToneDb[s]);
            g.FillEllipse(&dot, static_cast<REAL>(x - r), static_cast<REAL>(y - r), static_cast<REAL>(2 * r),
                          static_cast<REAL>(2 * r));
        }
        g.ResetClip();
    }
    // ---- 4. the correction now as an EQ curve ----
    {
        const Plot& qn = plot_[3];
        PresetValues v = values_;
        for (Param p : kGain) v[p] = 0.0;
        v[pLoudness] = 0.0;
        double autoDb[4];
        for (int s = 0; s < 4; ++s) autoDb[s] = m->autoToneDb[s];
        const int saved = clip(dc, qn);
        std::vector<POINT> line;
        const int n = 72;
        for (int i = 0; i <= n; ++i) {
            const double hz = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(i) / n);
            line.push_back(ipt(qn.X(hz), qn.Y(colourCurveDb(v, autoDb, kFs, hz))));
        }
        polyline(dc, line, kTeal, std::max(2, px(2)));
        RestoreDC(dc, saved);
    }
}

// The maximum's lines (above and under the middle) are taken anywhere along them.
int AutoToneGraphs::lineAt(POINT p) const {
    const Plot& q = plot_[0];
    if (p.x < q.l - 2 || p.x > q.r + 2 || p.y < q.t - 2 || p.y > q.b + 2) return -1;
    const long reach = px(5), up = std::lround(q.Y(values_[pAutoMax])), down = std::lround(q.Y(-values_[pAutoMax]));
    return std::labs(p.y - up) <= reach || std::labs(p.y - down) <= reach ? mMax : -1;
}

int AutoToneGraphs::wheelParam(POINT p) const {
    if (PtInRect(&panel_[1], p)) return pAutoAmount;
    if (PtInRect(&panel_[2], p)) return pAutoTime;
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return legend_.param[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    return -1;
}

// The time by its own step (1 s), as the compressor's times.
double AutoToneGraphs::stepped(int param, int steps) const {
    if (param != pAutoTime) return GraphStrip::stepped(param, steps);
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
}

bool AutoToneGraphs::dragValue(int mark, POINT p, double& v) const {
    switch (mark) {
    case mMax:
        v = std::fabs(plot_[0].fromY(p.y));
        return true;
    case mAmount:
        v = 100.0 * plot_[1].fromY(p.y) / kAmountAt;
        return true;
    case mTime:
        v = plot_[2].fromX(p.x);
        return true;
    default:
        return false;
    }
}

} // namespace ap
