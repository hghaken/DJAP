// SPDX-License-Identifier: GPL-3.0-or-later
#include "limgraph.h"

#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

// the peaks' graph: dBTP (the ceiling goes from -6 to 0)
const double kLo = -24.0, kHi = 4.0;
// in -> out: dBTP
const double kInLo = -12.0, kInHi = 6.0;
// the release graph: a peak turned down this much
const double kExampleDb = 6.0, kReleaseFromMs = -60.0, kReleaseToMs = 700.0;
const int kTimePoints = 400; // 4 s of 10 ms points

} // namespace

int LimiterGraphs::markParam(int mark) const {
    static const int kParamOf[kMarks] = {pLimCeiling, pLimRelease, pLimCeiling};
    return mark >= 0 && mark < kMarks ? kParamOf[mark] : -1;
}

void LimiterGraphs::measure(HDC dc) {
    legend_.count = 1;
    legend_.param[0] = pLimCeiling;
    legend_.name[0] = L"Ceiling";
    legend_.widest[0] = L"-6.0 dBTP";
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"Peaks in and out (off)");
    numberW_ = textWidth(dc, L"-12.0 dB");
}

// The peaks across the top with the release and in -> out under them, or all three side by side (layoutThree); the
// release's reset icon in its panel's title line, the ceiling's legend in the peaks panel's header.
void LimiterGraphs::layout(int w, int h) {
    layoutThree(w, h, panel_);
    icons_.push_back({cornerIcon(panel_[1]), pLimRelease});
    layoutLegend(legend_, panel_[0], titleW_, numberW_);
}

void LimiterGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kPeaks =
        L"Behind: the last 4 seconds of this deck - the true peaks going into the limiter (about: what came out plus "
        L"what it took), the peaks coming out (the line) and, orange, what the limiter cut off (the number is the "
        L"most it turned down). In front: the ceiling - the output never goes over it."
        L"\n\nCeiling (-6 .. 0 dBTP): drag the dashed line or its mark up or down, or turn the mouse wheel over its "
        L"name in the legend for one step of 0.1 dB. The arrow behind the value sets it back. The look-ahead - how "
        L"early the limiter sees a peak coming - is set in the Setup window.";
    static const wchar_t* const kRelease =
        L"Release (10 .. 500 ms): the gain after a peak the limiter turned down 6 dB - how fast it lets go. Short: "
        L"louder, but a fast series of peaks can make the sound rough; long: smoother, a little less loud."
        L"\n\nDrag the mark, or turn the mouse wheel over this graph for one step of 5 ms. The arrow in the corner "
        L"sets it back.";
    static const wchar_t* const kCurve =
        L"Peaks in -> out: under the ceiling the limiter does nothing; a peak above it comes out at the ceiling. The "
        L"white dot is the newest peak, the faint ones the last half second.\n\nThe mark where it bends is the "
        L"ceiling too: drag it up or down, or turn the mouse wheel over this graph.";
    legendTips(legend_, panel_[0], kPeaks, out);
    const RECT& p = panel_[1];
    const RECT& icon = icons_[0].rect;
    out.push_back({RECT{p.left, p.top, icon.left, p.bottom}, kRelease, -1, -1});
    out.push_back({RECT{icon.left, icon.bottom, p.right, p.bottom}, kRelease, -1, -1});
    out.push_back({panel_[2], kCurve, -1, -1});
    out.push_back({legend_.item[0], nullptr, pLimCeiling, -1});
    for (const Icon& i : icons_) out.push_back({i.rect, nullptr, -1, i.param});
}

void LimiterGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    static const wchar_t* const kTitle[kPanels] = {L"Peaks in and out", L"Release", L"Peaks in -> out"};
    const bool on = values_[pLimOn] >= 0.5;
    const double ceiling = values_[pLimCeiling];
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
                           i == 0 && !on ? std::wstring(L"Peaks in and out (off)") : kTitle[i], kDim, TA_LEFT});
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
        {
            const RECT& item = legend_.item[0];
            const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
            all.push_back({static_cast<int>(item.left), y, legend_.name[0], kText, TA_LEFT});
            all.push_back({static_cast<int>(item.right) - px(2), y, number(ceiling, 1, L" dBTP"), kOrange, TA_RIGHT});
        }
        // ---- 2. the release ----
        drawRelease(g, plot_[1], mRelease, kExampleDb, values_[pLimRelease], kReleaseFromMs, kReleaseToMs, 100.0, 500.0,
                    L"0.5 s", all);
        // ---- 3. in -> out ----
        Plot& q = plot_[2];
        q.x0 = q.y0 = kInLo;
        q.x1 = q.y1 = kInHi;
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
        Pen grid(col(kGrid), lw), dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw);
        dots.SetDashStyle(DashStyleDot);
        dash.SetDashStyle(DashStyleDash);
        curve.SetLineJoin(LineJoinRound);
        auto row = [](double y) { return std::floor(y) + 0.5; };
        g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                        static_cast<REAL>(q.b - q.t + 2)));
        g.DrawLine(&dots, pt(q.X(kInLo), q.Y(kInLo)), pt(q.X(kInHi), q.Y(kInHi))); // no limiting
        g.DrawLine(&grid, pt(row(q.X(0)), q.t), pt(row(q.X(0)), q.b));              // 0 dBTP
        g.DrawLine(&grid, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
        const double yC = row(q.Y(ceiling));
        g.DrawLine(&dash, pt(q.l, yC), pt(q.r, yC));
        SolidBrush cut(col(kOrange, 50));
        const PointF tri[3] = {pt(q.X(ceiling), q.Y(ceiling)), pt(q.X(kInHi), q.Y(kInHi)), pt(q.X(kInHi), yC)};
        g.FillPolygon(&cut, tri, 3);
        const PointF line[3] = {pt(q.X(kInLo), q.Y(kInLo)), pt(q.X(ceiling), yC), pt(q.X(kInHi), yC)};
        g.DrawLines(&curve, line, 3);
        g.ResetClip();
        drawMark(g, mCeilingCurve, q.X(ceiling), yC);
        all.push_back({static_cast<int>(q.X(0)) + px(3), static_cast<int>(q.b) - th - px(4), L"0 dBTP in", kDim, TA_LEFT});
    }
    labels(dc, all);
}

// The ceiling's line over the peaks: still, but it lies over what moves.
void LimiterGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    layerNames_.clear();
    const Plot& q = plot_[0];
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    const double y = std::floor(q.Y(values_[pLimCeiling])) + 0.5, x = q.r - px(14);
    Pen dash(col(kOrange), lw), dots(col(kGrid), lw);
    dash.SetDashStyle(DashStyleDash);
    dots.SetDashStyle(DashStyleDot);
    g.DrawLine(&dots, pt(q.l, std::floor(q.Y(0)) + 0.5), pt(q.r, std::floor(q.Y(0)) + 0.5)); // 0 dBTP
    g.DrawLine(&dash, pt(q.l, y), pt(q.r, y));
    drawMark(g, mCeiling, x, y);
    layerNames_.push_back({static_cast<int>(x) - px(9), static_cast<int>(y + px(2)),
                           L"Ceiling " + number(values_[pLimCeiling], 1, L" dBTP"), kOrange, TA_RIGHT});
}

void LimiterGraphs::paintLive(HDC dc) {
    const bool playing = live();
    const DeckAnalysis* an = analysis();
    const bool have = an && playing && an->compCount > 0;
    const int th = textH_;
    const Plot& q = plot_[0];
    double most = 0.0;
    {
        const int saved = clip(dc, q);
        if (have) {
            std::vector<POINT> in, out, outLine;
            in.push_back(ipt(q.l, q.b));
            out.push_back(ipt(q.l, q.b));
            const double step = (q.r - q.l) / (kTimePoints - 1.0);
            for (int i = 0; i < kTimePoints; ++i) {
                const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - kTimePoints + i);
                const double x = q.l + i * step, o = std::max(kLo, static_cast<double>(c.peak));
                in.push_back(ipt(x, q.Y(o + c.limiter)));
                out.push_back(ipt(x, q.Y(o)));
                outLine.push_back(out.back());
                most = std::max(most, static_cast<double>(c.limiter));
            }
            in.push_back(ipt(q.r, q.b));
            out.push_back(ipt(q.r, q.b));
            polygon(dc, in, blend(kOrange, kPanel, 0.55));  // what went in; what came out is laid over it: the cut stays
            polygon(dc, out, blend(kInput, kPanel, 0.28));
            polyline(dc, outLine, blend(kTeal, kPanel, 0.7), std::max(1, px(1.5)));
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(9), static_cast<LONG>(q.t) - px(9), static_cast<LONG>(q.r) + px(9),
                        static_cast<LONG>(q.b) + px(9)});
    // in -> out: the newest peaks on the curve
    if (have) {
        using namespace Gdiplus;
        const Plot& qc = plot_[2];
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetClip(RectF(static_cast<REAL>(qc.l - 1), static_cast<REAL>(qc.t - 1), static_cast<REAL>(qc.r - qc.l + 2),
                        static_cast<REAL>(qc.b - qc.t + 2)));
        const int n = 50;
        for (int k = 0; k < n; ++k) {
            const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - n + k);
            const double o = c.peak, in = o + c.limiter;
            if (in < kInLo) continue;
            const bool newest = k == n - 1;
            SolidBrush dot(col(RGB(250, 250, 250), newest ? 255 : 20 + 120 * k / n));
            const double r = (newest ? px(35) : px(15)) / 10.0, x = qc.X(in), y = qc.Y(o);
            g.FillEllipse(&dot, static_cast<REAL>(x - r), static_cast<REAL>(y - r), static_cast<REAL>(2 * r),
                          static_cast<REAL>(2 * r));
        }
        g.ResetClip();
    }
    labels(dc, layerNames_);
    if (have) text(dc, panel_[0].right - px(6), panel_[0].top + px(3), number(most > 0.05 ? -most : 0.0, 1, L" dB"), kOrange, TA_RIGHT);
    else text(dc, (q.l + q.r) / 2, q.b - th - px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
}

// The ceiling's line in the peaks graph and in in -> out is taken anywhere along it.
int LimiterGraphs::lineAt(POINT p) const {
    const long reach = px(5);
    const Plot& q0 = plot_[0];
    if (p.x >= q0.l - 2 && p.x <= q0.r + 2 && p.y >= q0.t - 2 && p.y <= q0.b + 2 && std::labs(p.y - mark_[mCeiling].y) <= reach)
        return mCeiling;
    const Plot& q2 = plot_[2];
    if (p.x >= q2.l - 2 && p.x <= q2.r + 2 && p.y >= q2.t - 2 && p.y <= q2.b + 2 &&
        std::labs(p.y - mark_[mCeilingCurve].y) <= reach)
        return mCeilingCurve;
    return -1;
}

int LimiterGraphs::wheelParam(POINT p) const {
    if (PtInRect(&panel_[1], p)) return pLimRelease;
    if (PtInRect(&panel_[2], p)) return pLimCeiling;
    if (PtInRect(&legend_.item[0], p)) return pLimCeiling;
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    return -1;
}

// The release by its own step (5 ms): its slider is logarithmic (see CompressorGraphs::stepped).
double LimiterGraphs::stepped(int param, int steps) const {
    if (param != pLimRelease) return GraphStrip::stepped(param, steps);
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
}

bool LimiterGraphs::dragValue(int mark, POINT p, double& v) const {
    switch (mark) {
    case mCeiling:
        v = plot_[0].fromY(p.y);
        return true;
    case mCeilingCurve:
        v = plot_[2].fromY(p.y);
        return true;
    case mRelease:
        v = plot_[1].fromX(p.x);
        return true;
    default:
        return false;
    }
}

} // namespace ap
