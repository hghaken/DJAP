// SPDX-License-Identifier: GPL-3.0-or-later
#include "tonegraph.h"

#include "colour.h"
#include "graphgdi.h"

#include <cmath>
#include <cstdlib>

namespace ap {

using namespace graph;

namespace {

const double kLoHz = 20.0, kHiHz = 20000.0, kRangeDb = 15.0, kSpecRangeDb = 60.0, kFs = 48000.0;
const Param kFreq[4] = {pToneBassFreq, pToneLowMidFreq, pTonePresenceFreq, pToneTrebleFreq};
const Param kGain[4] = {pToneBass, pToneLowMid, pTonePresence, pToneTreble};
const wchar_t* const kName[4] = {L"Bass", L"Low-mid", L"Presence", L"Treble"};
// the spectrum window's colours (spectrumdraw.cpp): each section's own, the contour grey, the curve yellow
const COLORREF kNode[4] = {RGB(120, 175, 255), RGB(205, 150, 255), RGB(255, 190, 110), RGB(130, 230, 160)};
const COLORREF kContour = RGB(150, 155, 162), kCurve = RGB(250, 220, 90);

std::wstring hzText(double hz) { return hz >= 1000.0 ? number(hz / 1000.0, 1, L" kHz") : number(hz, 0, L" Hz"); }

} // namespace

bool ToneGraphs::owns(int param) const {
    for (int s = 0; s < 4; ++s)
        if (param == kFreq[s] || param == kGain[s]) return true;
    return false;
}

int ToneGraphs::markParam(int mark) const { return mark >= 0 && mark < 4 ? kFreq[mark] : -1; }

int ToneGraphs::markParam2(int mark) const { return mark >= 0 && mark < 4 ? kGain[mark] : -1; }

int ToneGraphs::partner(int param) const {
    for (int s = 0; s < 4; ++s) {
        if (param == kGain[s]) return kFreq[s];
        if (param == kFreq[s]) return kGain[s];
    }
    return -1;
}

void ToneGraphs::measure(HDC dc) {
    static const wchar_t* const kWidest[4] = {L"+12.0 dB  250 Hz", L"+12.0 dB  1000 Hz", L"+12.0 dB  6.0 kHz",
                                              L"+12.0 dB  16.0 kHz"};
    legend_.count = 4;
    for (int j = 0; j < 4; ++j) {
        legend_.param[j] = kGain[j];
        legend_.name[j] = kName[j];
        legend_.widest[j] = kWidest[j];
    }
    measureLegend(dc, legend_);
    titleW_ = textWidth(dc, L"Tone");
}

// One panel: the legend of the four points in its header, the curve under it.
void ToneGraphs::layout(int w, int h) {
    const int pad = px(5);
    panel_ = {pad, pad, w - pad, h - pad};
    layoutLegend(legend_, panel_, titleW_, 0);
}

void ToneGraphs::fillTips(std::vector<Tip>& out) {
    static const wchar_t* const kCurveTip =
        L"The EQ's tone: four sections - the bass shelf, the low-mid bell, the presence bell and the treble shelf - "
        L"each a thin line in its colour, together the thick yellow line (with the contour when that is on, grey). "
        L"Behind: the deck's spectrum, grey going in, the line coming out. Dashed: the curve with the auto tone's "
        L"corrections of this moment.\n\nDrag a point: left or right moves its frequency, up or down its gain. The "
        L"mouse wheel over a point, or over its name in the legend, steps its gain by 0.5 dB. The arrow behind a "
        L"value sets the point back - its gain and its frequency.";
    legendTips(legend_, panel_, kCurveTip, out);
    for (int j = 0; j < legend_.count; ++j) out.push_back({legend_.item[j], nullptr, legend_.param[j], -1});
    for (const Icon& icon : icons_) out.push_back({icon.rect, nullptr, -1, icon.param});
}

std::wstring ToneGraphs::valueText(int j) const {
    return number(values_[kGain[j]], 1, L" dB", true) + L"  " + hzText(values_[kFreq[j]]);
}

void ToneGraphs::renderStill(HDC dc, int w, int h) {
    (void)w, (void)h;
    const int th = textH_;
    std::vector<Label> all;
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(col(kBack));
        drawPanels(g, &panel_, 1);
    }
    all.push_back({static_cast<int>(panel_.left) + px(6), static_cast<int>(panel_.top) + px(3), L"Tone", kDim, TA_LEFT});
    plot_.l = panel_.left + px(34);
    plot_.r = panel_.right - px(10);
    plot_.t = legend_.headerBottom + px(8);
    plot_.b = panel_.bottom - th - px(10);
    plot_.logX = true;
    plot_.x0 = kLoHz;
    plot_.x1 = kHiHz;
    plot_.y0 = -kRangeDb;
    plot_.y1 = kRangeDb;
    for (int j = 0; j < legend_.count; ++j) {
        const RECT& item = legend_.item[j];
        const int y = static_cast<int>(item.top + (item.bottom - item.top - th) / 2.0 + 0.5);
        all.push_back({static_cast<int>(item.left), y, legend_.name[j], kNode[j], TA_LEFT});
        all.push_back({static_cast<int>(item.right) - px(2), y, valueText(j), kOrange, TA_RIGHT});
    }
    // the scales: dB at the left, Hz under the plot
    for (double db : {-12.0, -6.0, 0.0, 6.0, 12.0})
        all.push_back({static_cast<int>(plot_.l) - px(5), static_cast<int>(plot_.Y(db)) - th / 2, number(db, 0, L"", db > 0),
                       kDim, TA_RIGHT});
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
        all.push_back({static_cast<int>(plot_.X(hz)), static_cast<int>(plot_.b) + px(3),
                       hz >= 1000.0 ? number(hz / 1000.0, 0, L" k") : number(hz, 0, L""), kDim, TA_CENTER});
    labels(dc, all);
}

// Over the spectrum: the grid, the sub bass's range, each section's line, the total, the points and their names.
void ToneGraphs::renderLayer(Gdiplus::Graphics& g, int w, int h) {
    (void)w, (void)h;
    using namespace Gdiplus;
    layerNames_.clear();
    const Plot& q = plot_;
    const int th = textH_;
    const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
    auto row = [](double y) { return std::floor(y) + 0.5; };
    Pen grid(col(kGrid), lw), dots(col(kGrid), lw);
    dots.SetDashStyle(DashStyleDot);
    for (double db : {-12.0, -6.0, 6.0, 12.0}) g.DrawLine(&dots, pt(q.l, row(q.Y(db))), pt(q.r, row(q.Y(db))));
    for (double hz : {100.0, 1000.0, 10000.0}) g.DrawLine(&dots, pt(row(q.X(hz)), q.t), pt(row(q.X(hz)), q.b));
    g.DrawLine(&grid, pt(q.l, row(q.Y(0))), pt(q.r, row(q.Y(0))));
    const double sub = values_[pSubAmount];
    if (sub > 0.0) { // an octave under the bass, as the spectrum window shows it
        const double xa = q.X(std::max(kLoHz, values_[pSubFreq] / 2.0)), xb = q.X(values_[pSubFreq]);
        SolidBrush fill(col(kNode[0], static_cast<int>(60 + 120 * sub / 100.0)));
        g.FillRectangle(&fill, static_cast<REAL>(xa), static_cast<REAL>(q.Y(0) - px(6)), static_cast<REAL>(xb - xa),
                        static_cast<REAL>(px(6)));
        layerNames_.push_back({static_cast<int>((xa + xb) / 2), static_cast<int>(q.Y(0)) - px(6) - th, L"sub", kNode[0], TA_CENTER});
    }
    const int n = std::max(96, static_cast<int>(q.r - q.l) / 3);
    std::vector<PointF> total, section[ColourConfig::kSections];
    for (int i = 0; i <= n; ++i) {
        const double hz = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(i) / n), x = q.X(hz);
        double sum = 0.0;
        for (int s = 0; s < ColourConfig::kSections; ++s) {
            const double one = colourSectionDb(values_, s, 0.0, kFs, hz);
            sum += one;
            section[s].push_back(pt(x, q.Y(one)));
        }
        total.push_back(pt(x, q.Y(sum)));
    }
    g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                    static_cast<REAL>(q.b - q.t + 2)));
    for (int s = 0; s < ColourConfig::kSections; ++s) {
        const double gain = s < 4 ? values_[kGain[s]] : values_[pLoudness];
        if (gain == 0.0) continue;
        Pen one(col(s < 4 ? kNode[s] : kContour, 200), 1.2f * lw);
        g.DrawLines(&one, section[s].data(), static_cast<INT>(section[s].size()));
    }
    Pen curve(col(kCurve), 3.0f * lw);
    curve.SetLineJoin(LineJoinRound);
    g.DrawLines(&curve, total.data(), static_cast<INT>(total.size()));
    g.ResetClip();
    // the points: in their own colours, a ring of the panel's colour around them
    const double mr = px(55) / 10.0;
    for (int s = 0; s < 4; ++s) {
        const double x = q.X(values_[kFreq[s]]), y = q.Y(values_[kGain[s]]);
        SolidBrush ring(col(kPanel)), fill(col(kNode[s]));
        g.FillEllipse(&ring, static_cast<REAL>(x - mr - 2 * lw), static_cast<REAL>(y - mr - 2 * lw),
                      static_cast<REAL>(2 * (mr + 2 * lw)), static_cast<REAL>(2 * (mr + 2 * lw)));
        g.FillEllipse(&fill, static_cast<REAL>(x - mr), static_cast<REAL>(y - mr), static_cast<REAL>(2 * mr),
                      static_cast<REAL>(2 * mr));
        mark_[static_cast<size_t>(s)] = {static_cast<LONG>(x + 0.5), static_cast<LONG>(y + 0.5)};
        const bool under = y - mr - th - px(3) < q.t;
        layerNames_.push_back({static_cast<int>(x), static_cast<int>(under ? y + mr + px(3) : y - mr - th - px(3)), kName[s],
                               kNode[s], TA_CENTER});
    }
}

// The spectrum behind (input area, output line), the layer over it, the curve with the auto tone now (dashed).
void ToneGraphs::paintLive(HDC dc) {
    const DeckAnalysis* an = analysis();
    const DeckMeters* m = meters();
    const bool playing = live();
    const Plot& q = plot_;
    {
        const int saved = clip(dc, q);
        if (an && playing && an->specIn.frames() > 0) {
            double top = -70.0;
            for (float v : an->specIn.points()) top = std::max(top, static_cast<double>(v));
            for (float v : an->specOut.points()) top = std::max(top, static_cast<double>(v));
            const double y1 = top + 3.0, y0 = y1 - kSpecRangeDb;
            auto sy = [&](double db) { return q.b - std::min(1.0, std::max(0.0, (db - y0) / (y1 - y0))) * (q.b - q.t); };
            std::vector<POINT> in, out;
            in.push_back(ipt(q.l, q.b));
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i) {
                const double hz = SpectrumAnalyzer::pointHz(i);
                in.push_back(ipt(q.X(hz), sy(an->specIn.points()[static_cast<size_t>(i)])));
                out.push_back(ipt(q.X(hz), sy(an->specOut.points()[static_cast<size_t>(i)])));
            }
            in.push_back(ipt(q.r, q.b));
            polygon(dc, in, blend(kInput, kPanel, 0.22));
            polyline(dc, out, blend(kTeal, kPanel, 0.6), 1);
        }
        RestoreDC(dc, saved);
    }
    blendLayer(dc, RECT{static_cast<LONG>(q.l) - px(12), static_cast<LONG>(q.t) - px(12) - textH_,
                        static_cast<LONG>(q.r) + px(12), static_cast<LONG>(q.b) + px(12)});
    bool moves = false;
    double autoDb[4] = {};
    for (int s = 0; s < 4 && m && playing; ++s) {
        autoDb[s] = m->autoToneDb[s];
        moves = moves || std::fabs(autoDb[s]) > 0.05;
    }
    if (moves) {
        const int saved = clip(dc, q);
        std::vector<POINT> line;
        const int n = 96;
        for (int i = 0; i <= n; ++i) {
            const double hz = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(i) / n);
            line.push_back(ipt(q.X(hz), q.Y(colourCurveDb(values_, autoDb, kFs, hz))));
        }
        polyline(dc, line, kCurve, 1, PS_DASH);
        RestoreDC(dc, saved);
    }
    labels(dc, layerNames_);
}

bool ToneGraphs::dragValue(int mark, POINT p, double& v) const {
    if (mark < 0 || mark >= 4) return false;
    v = plot_.fromX(p.x);
    return true;
}

bool ToneGraphs::dragValue2(int mark, POINT p, double& v) const {
    if (mark < 0 || mark >= 4) return false;
    v = plot_.fromY(p.y);
    return true;
}

// The wheel steps a point's gain: over the point, or over its name and value in the legend (or its icon).
int ToneGraphs::wheelParam(POINT p) const {
    for (int j = 0; j < legend_.count; ++j)
        if (PtInRect(&legend_.item[j], p)) return kGain[j];
    for (const Icon& icon : icons_)
        if (PtInRect(&icon.rect, p)) return icon.param;
    const long reach = px(12);
    for (int s = 0; s < 4; ++s) {
        const long dx = p.x - mark_[static_cast<size_t>(s)].x, dy = p.y - mark_[static_cast<size_t>(s)].y;
        if (dx * dx + dy * dy <= reach * reach) return kGain[s];
    }
    return -1;
}

} // namespace ap
