// SPDX-License-Identifier: GPL-3.0-or-later
#include "compgraph.h"

#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace Gdiplus { // gdiplus.h uses min / max unqualified
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

namespace ap {

namespace {

const wchar_t* const kClass = L"APCompressorGraphs";
bool g_registered = false;

// the meter panel's palette (meterview.cpp, analysiswnd.cpp)
const COLORREF kBack = RGB(28, 30, 33), kPanel = RGB(20, 22, 25), kEdge = RGB(55, 58, 64), kGrid = RGB(70, 74, 80),
               kText = RGB(214, 218, 224), kDim = RGB(120, 126, 134), kOrange = RGB(240, 130, 50),
               kTeal = RGB(80, 215, 195), kInput = RGB(150, 156, 164);

// the curve and the level graph: LU to the level the threshold is relative to (the threshold goes up to +15)
const double kLo = -30.0, kHi = 18.0;
const double kRatioMarkLu = 1.0;  // the ratio's mark sits this far under the right end
const double kMakeupMarkLu = 5.0; // the make-up's mark this far above the left end (under any knee)
// the attack and release graphs: a hit this far over the threshold
const double kHitDb = 10.0, kHitAtMs = 10.0, kAttackSpanMs = 160.0;
const double kReleaseFromMs = -100.0, kReleaseToMs = 1100.0, kGainLoDb = -10.0;
const int kTimePoints = 400; // the level graph: 4 s of 10 ms points
// the side chain's graph: up to 1 kHz (client, 2026-11-13: "to 1 k instead of 10 k" - the high-pass ends at 200 Hz,
// so the bass it is about gets the width)
const double kSpecLoHz = 30.0, kSpecHiHz = 1000.0, kSpecRangeDb = 54.0, kFilterRangeDb = 36.0;

Gdiplus::Color col(COLORREF c, int alpha = 255) {
    return Gdiplus::Color(static_cast<BYTE>(alpha), static_cast<BYTE>(c & 0xFF), static_cast<BYTE>((c >> 8) & 0xFF),
                          static_cast<BYTE>((c >> 16) & 0xFF));
}

Gdiplus::PointF pt(double x, double y) { return Gdiplus::PointF(static_cast<float>(x), static_cast<float>(y)); }

POINT ipt(double x, double y) { return POINT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))}; }

// `fg` at opacity `a` over `bg`: the colour of a blended area, worked out once (the live areas are plain polygons).
COLORREF blend(COLORREF fg, COLORREF bg, double a) {
    auto channel = [&](int shift) {
        const int f = static_cast<int>((fg >> shift) & 0xFF), b = static_cast<int>((bg >> shift) & 0xFF);
        return static_cast<COLORREF>(std::lround(b + (f - b) * a)) << shift;
    };
    return channel(0) | channel(8) | channel(16);
}

// The side chain's high-pass (2nd order, as design::highPass) in dB at `hz`; 0 when it is off.
double sideChainDb(double hz, double corner) {
    if (corner <= 0.0) return 0.0;
    const double r = hz / corner, r4 = r * r * r * r;
    return 10.0 * std::log10(r4 / (1.0 + r4) + 1e-12);
}

std::wstring number(double v, int decimals, const wchar_t* unit, bool sign = false) {
    wchar_t b[48];
    swprintf(b, 48, sign ? L"%+.*f%ls" : L"%.*f%ls", decimals, v, unit);
    return b;
}

} // namespace

double CompressorGraphs::Plot::X(double v) const {
    const double f = logX ? std::log(std::max(v, 1e-9) / x0) / std::log(x1 / x0) : (v - x0) / (x1 - x0);
    return l + std::min(1.0, std::max(0.0, f)) * (r - l);
}

double CompressorGraphs::Plot::Y(double v) const {
    const double f = (v - y0) / (y1 - y0);
    return b - std::min(1.0, std::max(0.0, f)) * (b - t);
}

double CompressorGraphs::Plot::fromX(double px) const {
    const double f = std::min(1.0, std::max(0.0, (px - l) / std::max(1.0, r - l)));
    return logX ? x0 * std::pow(x1 / x0, f) : x0 + f * (x1 - x0);
}

double CompressorGraphs::Plot::fromY(double py) const {
    const double f = std::min(1.0, std::max(0.0, (b - py) / std::max(1.0, b - t)));
    return y0 + f * (y1 - y0);
}

CompressorGraphs::~CompressorGraphs() {
    if (font_) DeleteObject(font_);
    if (symbol_) DeleteObject(symbol_);
    if (still_) DeleteObject(still_);
    if (frame_) DeleteObject(frame_);
    if (curve_) DeleteObject(curve_);
}

void CompressorGraphs::registerClass(HINSTANCE module) {
    if (g_registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    g_registered = RegisterClassExW(&wc) != 0;
}

void CompressorGraphs::unregisterClass(HINSTANCE module) {
    if (g_registered) UnregisterClassW(kClass, module);
    g_registered = false;
}

void CompressorGraphs::create(HWND parent, HINSTANCE module, UINT dpi) {
    registerClass(module);
    setDpi(dpi);
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD, 0, 0, 10, 10, parent, nullptr, module, this);
}

void CompressorGraphs::setDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    if (font_) DeleteObject(font_);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 85 / 100; // the analysis window's small font
    font_ = CreateFontIndirectW(&lf);
    if (symbol_) DeleteObject(symbol_);
    lf = ncm.lfMessageFont; // the arrow of the rows' reset buttons, smaller
    lf.lfHeight = lf.lfHeight * 7 / 5;
    wcscpy_s(lf.lfFaceName, L"Segoe UI Symbol");
    symbol_ = CreateFontIndirectW(&lf);
    // the legend's columns: the small font's height, its widest name and its widest value
    if (HDC dc = GetDC(nullptr)) {
        HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        auto width = [&](const wchar_t* s) {
            SIZE z{};
            GetTextExtentPoint32W(dc, s, lstrlenW(s), &z);
            return static_cast<int>(z.cx);
        };
        static const wchar_t* const kNames[kLegend] = {L"Threshold", L"Ratio", L"Knee", L"Make-up"};
        static const wchar_t* const kWidest[kLegend] = {L"+15.0 LU", L"8.0:1", L"12.0 dB", L"12.0 dB"};
        textH_ = tm.tmHeight;
        for (int j = 0; j < kLegend; ++j) {
            nameW_[j] = width(kNames[j]);
            valueW_[j] = width(kWidest[j]);
        }
        titleW_ = width(L"Level and curve (off)");
        numberW_ = width(L"-12.0 dB");
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    stillDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void CompressorGraphs::setValues(const PresetValues& values, const PresetValues& reset) {
    if (drag_ != mNone) return; // the mark being dragged is ahead of the settings by one message
    PresetValues v = values;
    normaliseValues(v);
    if (v == values_ && reset == resetValues_) return;
    values_ = v;
    resetValues_ = reset;
    stillDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void CompressorGraphs::setLive(const AnalysisSource* source, int deck) {
    source_ = source;
    deck_ = deck;
}

void CompressorGraphs::refresh() {
    if (hwnd_ && IsWindowVisible(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

int CompressorGraphs::resetParam(int k) {
    static const int kParam[kResets] = {pCompAttack, pCompRelease,  pCompSideHpf, // an icon in the graph's corner
                                        pCompThreshold, pCompRatio, pCompKnee, pCompMakeup}; // the curve's legend
    return k >= 0 && k < kResets ? kParam[k] : -1;
}

int CompressorGraphs::cornerPanel(int k) {
    static const int kCorner[kFirstLegend] = {1, 2, 3}; // attack, release, side chain
    return kCorner[k];
}

bool CompressorGraphs::resetLit(int k) const {
    const size_t p = static_cast<size_t>(resetParam(k));
    return std::fabs(values_[p] - resetValues_[p]) > 1e-9;
}

// The rectangles do not overlap (the tooltip control keeps the tool the mouse is in): a panel with an icon in its
// corner (attack, release, side chain) is two rectangles around the icon - what is left of it, what is under it;
// the level-and-curve panel is its graph under the header plus the header on either side of the legend (or above
// it and beside it, when the legend stands under the title); the legend is a rectangle per item plus that item's
// icon.
RECT CompressorGraphs::tipRect(int i) {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    layoutPanels(rc.right, rc.bottom);
    if (i < 0 || i >= kTips) return RECT{};
    if (i >= kFirstResetTip) return reset_[i - kFirstResetTip];
    if (i >= kFirstNameTip) return legendItem_[i - kFirstNameTip];
    const RECT& c = panel_[0];
    const bool inLine = legend_.top < c.top + textH_; // the legend on the title's line
    switch (i) {
    case 0:
        return RECT{c.left, headerBottom_, c.right, c.bottom};
    case 1:
        return inLine ? RECT{c.left, c.top, legend_.left, headerBottom_} : RECT{c.left, c.top, c.right, legend_.top};
    case 2:
        return inLine ? RECT{legend_.right, c.top, c.right, headerBottom_}
                      : RECT{legend_.right, legend_.top, c.right, headerBottom_};
    default: {
        const int k = (i - 3) / 2; // the icon in this panel's corner
        const RECT& p = panel_[cornerPanel(k)];
        if ((i - 3) % 2 == 0) return RECT{p.left, p.top, reset_[k].left, p.bottom}; // left of the icon
        return RECT{reset_[k].left, reset_[k].bottom, p.right, p.bottom};           // under it
    }
    }
}

const wchar_t* CompressorGraphs::tipText(int i) {
    static const int kPanelOf[kPanelTips] = {0, 0, 0, 1, 1, 2, 2, 3, 3};
    if (i < 0 || i >= kPanelTips) return nullptr; // a legend item or a reset icon: the owner's text
    i = kPanelOf[i];
    static const wchar_t* const kPanelTexts[kPanels] = {
        L"Behind: the last 4 seconds of this deck - the level at the compressor (grey: what its side chain hears), the "
        L"level it lets out (thin line) and, from the top, how much it turns down (orange; the number is the largest "
        L"reduction). In front: the compressor's curve on the same scale - the level going in (to the right) against "
        L"the level coming out (up), in LU to the target. Under the threshold it follows the dotted diagonal; above "
        L"it, it bends - flatter with a higher ratio, rounder with a wider knee. The white dot is the level now."
        L"\n\nFour values are set here, each a line across the graph: drag a line anywhere along it, or its mark, up "
        L"or down. Threshold: music above this line is compressed. Knee: the shaded band around it. Ratio: the level "
        L"the loudest music comes out at - lower is a higher ratio. Make-up: the quiet end of the curve, and the "
        L"whole curve with it; all the way down = auto. The legend names their values; the arrow behind a value sets "
        L"it back.",
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
    return kPanelTexts[i];
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

// Four panels. The level graph with the curve in it wants the width: it lies across the top, a little over half of
// the height, with the attack, the release and the side chain in a row under it, the three equally wide (client,
// 2026-11-13: "the side chain graph can be narrower", then "make the attack, release and side chain windows equally
// wide"). A strip too low for two rows, or so wide that the top panel would be a ribbon in which the curve goes
// flat, has all four side by side instead, the first by far the widest.
void CompressorGraphs::layoutPanels(int w, int h) {
    static const double kBeside[kPanels] = {2.2, 1.0, 1.0, 1.0}; // all four in one row
    static const double kUnder[kPanels] = {0.0, 1.0, 1.0, 1.0};  // the three under the wide one
    const int gap = px(5), pad = px(5);
    const int innerH = h - 2 * pad, topH = static_cast<int>((innerH - gap) * 0.58);
    const bool twoRows = innerH >= 2 * px(130) + gap && (w - 2 * pad) <= 4.6 * topH;
    auto row = [&](const double* share, int first, int count, int top, int bottom) {
        double sum = 0.0;
        for (int i = first; i < first + count; ++i) sum += share[i];
        const double usable = w - 2 * pad - (count - 1) * gap;
        double x = pad;
        for (int i = first; i < first + count; ++i) {
            const double pw = usable * share[i] / sum;
            panel_[i] = {static_cast<LONG>(x + 0.5), top, static_cast<LONG>(x + pw + 0.5), bottom};
            x += pw + gap;
        }
    };
    if (twoRows) {
        panel_[0] = {pad, pad, w - pad, pad + topH};
        row(kUnder, 1, kPanels - 1, pad + topH + gap, h - pad);
    } else {
        row(kBeside, 0, kPanels, pad, h - pad);
    }
    // the reset icons of the attack, the release and the side chain's high-pass: in their panels' title line, at
    // the right (above the plot)
    const int iconW = px(22), rowH = px(18);
    for (int k = 0; k < kFirstLegend; ++k) {
        const RECT& p = panel_[cornerPanel(k)];
        reset_[k] = {p.right - px(3) - iconW, p.top + px(1), p.right - px(3), p.top + px(1) + rowH};
    }
    // The legend of the level-and-curve panel - an item per value: its name, its value, its reset icon. In the
    // panel's header, off the graph: on the title's line between the title and the reduction's number where that
    // fits, else in columns under the title (as many as fit; two by two rather than three and one).
    const RECT& c = panel_[0];
    const int cTop = static_cast<int>(c.top), cLeft = static_cast<int>(c.left), between = px(14);
    int inLine = 0;
    for (int j = 0; j < kLegend; ++j) inLine += nameW_[j] + px(6) + valueW_[j] + px(2) + iconW + (j ? between : 0);
    const int from = cLeft + px(6) + titleW_ + px(16), until = static_cast<int>(c.right) - px(6) - numberW_ - px(12);
    if (from + inLine <= until) {
        const int top = cTop + px(3) - (rowH - textH_) / 2; // the texts on the title's base line
        int x = from;
        for (int j = 0; j < kLegend; ++j) {
            const int item = nameW_[j] + px(6) + valueW_[j] + px(2);
            legendItem_[j] = {x, top, x + item, top + rowH};
            reset_[kFirstLegend + j] = {x + item, top, x + item + iconW, top + rowH};
            x += item + iconW + between;
        }
        legend_ = {from, top, x - between, top + rowH};
        headerBottom_ = std::max(cTop + textH_ + px(5), top + rowH);
    } else {
        int nameW = 0, valueW = 0;
        for (int j = 0; j < kLegend; ++j) {
            nameW = std::max(nameW, nameW_[j]);
            valueW = std::max(valueW, valueW_[j]);
        }
        const int item = nameW + px(8) + valueW + px(2), cell = item + iconW;
        int perLine = std::min(kLegend, std::max(1, (static_cast<int>(c.right) - cLeft - 2 * px(6) + between) / (cell + between)));
        if (perLine == 3) perLine = 2;
        const int top = cTop + textH_ + px(5), lines = (kLegend + perLine - 1) / perLine;
        for (int j = 0; j < kLegend; ++j) {
            const int x = cLeft + px(6) + (j % perLine) * (cell + between), y = top + (j / perLine) * rowH;
            legendItem_[j] = {x, y, x + item, y + rowH};
            reset_[kFirstLegend + j] = {x + item, y, x + cell, y + rowH};
        }
        legend_ = {cLeft + px(6), top, cLeft + px(6) + perLine * (cell + between) - between, top + lines * rowH};
        headerBottom_ = top + lines * rowH;
    }
}

// What does not move with the audio - the panels, the curve, the attack and release graphs, their marks and texts -
// is drawn once into `still_` (again when a value, the size or the DPI changes); every refresh copies that and
// draws the live parts on it. Drawn whole at every refresh, the strip took a quarter of a processor core (GDI+
// anti-aliasing and blended fills over the full area, 20 times a second); this way about a tenth of that. The live
// areas are plain GDI polygons in colours blended beforehand.
void CompressorGraphs::renderStill(HDC dc, int w, int h, int th) {
    layoutPanels(w, h);
    std::vector<Label> labels;
    auto label = [&](double x, double y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT) {
        labels.push_back({static_cast<int>(x + 0.5), static_cast<int>(y + 0.5), s, c, align});
    };
    static const wchar_t* const kTitle[kPanels] = {L"Level and curve", L"Attack", L"Release", L"Side-chain high-pass"};
    const bool on = values_[pCompOn] >= 0.5;
    const double thr = threshold(), mk = makeup(), ratio = values_[pCompRatio], knee = values_[pCompKnee];
    const double attack = values_[pCompAttack], release = values_[pCompRelease];
    const double reduce = hitReduction();
    {
        using namespace Gdiplus;
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(col(kBack));
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0)); // a hairline
        Pen grid(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw),
            inputDash(col(kInput), 1.5f * lw), edge(col(kEdge), 1.0f);
        dash.SetDashStyle(DashStyleDash);
        inputDash.SetDashStyle(DashStyleDash);
        curve.SetLineJoin(LineJoinRound);
        SolidBrush panelBrush(col(kPanel)), orangeFill(col(kOrange, 60)), inputFill(col(kInput, 70));
        for (int i = 0; i < kPanels; ++i) {
            const RECT& p = panel_[i];
            g.FillRectangle(&panelBrush, static_cast<REAL>(p.left), static_cast<REAL>(p.top),
                            static_cast<REAL>(p.right - p.left), static_cast<REAL>(p.bottom - p.top));
            g.DrawRectangle(&edge, static_cast<REAL>(p.left) + 0.5f, static_cast<REAL>(p.top) + 0.5f,
                            static_cast<REAL>(p.right - p.left - 1), static_cast<REAL>(p.bottom - p.top - 1));
            label(p.left + px(6), p.top + px(3), i == 0 && !on ? std::wstring(L"Level and curve (off)") : kTitle[i],
                  kDim);
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = i == 0 ? headerBottom_ + px(5) : p.top + th + px(7); // the first: under its legend
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        const Region whole(Rect(0, 0, w, h));
        auto clipTo = [&](const Plot& q) {
            g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                            static_cast<REAL>(q.b - q.t + 2)));
        };

        // ---- 1. the level and the curve: the legend in the panel's header (the graph itself is drawn with the
        // audio, in paintLive: the level lies behind the curve) ----
        {
            Plot& q = plot_[0];
            q.x0 = q.y0 = kLo; // level in to the right, level out - and the level of the last seconds - up
            q.x1 = q.y1 = kHi;
            static const wchar_t* const kName[kLegend] = {L"Threshold", L"Ratio", L"Knee", L"Make-up"};
            const std::wstring value[kLegend] = {
                number(thr, 1, L" LU", true), number(ratio, 1, L":1"), number(knee, 1, L" dB"),
                values_[pCompMakeup] < 0.0 ? std::wstring(L"auto") : number(mk, 1, L" dB")};
            for (int j = 0; j < kLegend; ++j) {
                const RECT& item = legendItem_[j];
                const double y = item.top + (item.bottom - item.top - th) / 2.0;
                label(item.left, y, kName[j], kText);
                label(item.right - px(2), y, value[j], kOrange, TA_RIGHT);
            }
        }

        // ---- 2. the attack ----
        {
            Plot& q = plot_[1];
            q.x0 = 0.0;
            q.x1 = kAttackSpanMs;
            q.y0 = 0.0;
            q.y1 = kHitDb + 2.0;
            auto level = [&](double ms) { return ms < kHitAtMs ? 0.0 : kHitDb - reduce * (1.0 - std::exp(-(ms - kHitAtMs) / attack)); };
            clipTo(q);
            g.DrawLine(&grid, pt(q.l, q.Y(0)), pt(q.r, q.Y(0)));
            std::vector<PointF> line, area;
            line.push_back(pt(q.X(0), q.Y(0)));
            line.push_back(pt(q.X(kHitAtMs), q.Y(0)));
            area.push_back(pt(q.X(kHitAtMs), q.Y(kHitDb)));
            for (double ms = kHitAtMs; ms <= kAttackSpanMs + 0.01; ms += 1.0) {
                line.push_back(pt(q.X(ms), q.Y(level(ms))));
                area.push_back(line.back());
            }
            area.push_back(pt(q.X(kAttackSpanMs), q.Y(kHitDb)));
            g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size()));
            const PointF hit[4] = {pt(q.X(0), q.Y(0)), pt(q.X(kHitAtMs), q.Y(0)), pt(q.X(kHitAtMs), q.Y(kHitDb)),
                                   pt(q.X(kAttackSpanMs), q.Y(kHitDb))};
            g.DrawLines(&inputDash, hit, 4);
            g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
            const double xm = q.X(kHitAtMs + attack);
            g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
            for (double ms = 50.0; ms <= 150.0; ms += 50.0) g.DrawLine(&grid, pt(q.X(kHitAtMs + ms), q.b - px(4)), pt(q.X(kHitAtMs + ms), q.b));
            g.SetClip(&whole);
            drawMark(g, mAttack, xm, q.Y(level(kHitAtMs + attack)));
            const bool right = xm > q.l + 0.6 * (q.r - q.l);
            label(xm + (right ? -px(5) : px(5)), q.t, number(attack, 0, L" ms"), kOrange, right ? TA_RIGHT : TA_LEFT);
            label(q.r, q.b - th - px(4), L"150 ms", kDim, TA_RIGHT);
        }

        // ---- 3. the release ----
        {
            Plot& q = plot_[2];
            q.x0 = kReleaseFromMs;
            q.x1 = kReleaseToMs;
            q.y0 = kGainLoDb;
            q.y1 = 0.6;
            auto gain = [&](double ms) { return ms < 0.0 ? -reduce : -reduce * std::exp(-ms / release); };
            clipTo(q);
            g.FillRectangle(&inputFill, static_cast<REAL>(q.X(kReleaseFromMs)), static_cast<REAL>(q.t),
                            static_cast<REAL>(q.X(0) - q.X(kReleaseFromMs)), static_cast<REAL>(q.b - q.t));
            g.DrawLine(&grid, pt(q.l, q.Y(0)), pt(q.r, q.Y(0)));
            std::vector<PointF> line, area;
            area.push_back(pt(q.X(kReleaseFromMs), q.Y(0)));
            for (double ms = kReleaseFromMs; ms <= kReleaseToMs + 0.01; ms += 5.0) {
                line.push_back(pt(q.X(ms), q.Y(gain(ms))));
                area.push_back(line.back());
            }
            area.push_back(pt(q.X(kReleaseToMs), q.Y(0)));
            g.FillPolygon(&orangeFill, area.data(), static_cast<INT>(area.size()));
            g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
            const double xm = q.X(release);
            g.DrawLine(&dash, pt(xm, q.t), pt(xm, q.b));
            for (double ms = 250.0; ms <= 1000.0; ms += 250.0) g.DrawLine(&grid, pt(q.X(ms), q.b - px(4)), pt(q.X(ms), q.b));
            g.SetClip(&whole);
            drawMark(g, mRelease, xm, q.Y(gain(release)));
            const bool right = xm > q.l + 0.6 * (q.r - q.l);
            label(xm + (right ? -px(5) : px(5)), q.b - th - px(4), number(release, 0, L" ms"), kOrange,
                  right ? TA_RIGHT : TA_LEFT);
            label(q.X(0) - px(3), q.b - th - px(4), L"hit", kDim, TA_RIGHT);
            if (release < 780.0) label(q.X(1000.0) - px(3), q.b - th - px(4), L"1 s", kDim, TA_RIGHT);
        }

        // ---- 4. the side chain: its scale (drawn with the audio, in paintLive) ----
        plot_[3].logX = true;
        plot_[3].x0 = kSpecLoHz;
        plot_[3].x1 = kSpecHiHz;
    }
    SetBkMode(dc, TRANSPARENT);
    for (const Label& l : labels) {
        SetTextColor(dc, l.colour);
        SetTextAlign(dc, l.align | TA_TOP);
        TextOutW(dc, l.x, l.y, l.text.c_str(), static_cast<int>(l.text.size()));
    }
    // the reset icons: lit when the value differs from its reset value, as the rows' reset buttons
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    HGDIOBJ oldFont = SelectObject(dc, symbol_);
    for (int k = 0; k < kResets; ++k) {
        SetTextColor(dc, resetLit(k) ? kText : kGrid);
        RECT r = reset_[k];
        DrawTextW(dc, L"\x21BA", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
}

// A round mark that can be dragged, and where it is (markAt).
void CompressorGraphs::drawMark(Gdiplus::Graphics& g, int id, double x, double y) {
    using namespace Gdiplus;
    const double lw = std::max(1.0, dpi_ / 96.0), mr = px(45) / 10.0;
    SolidBrush ring(col(kPanel)), fill(col(kOrange));
    g.FillEllipse(&ring, static_cast<REAL>(x - mr - 2 * lw), static_cast<REAL>(y - mr - 2 * lw),
                  static_cast<REAL>(2 * (mr + 2 * lw)), static_cast<REAL>(2 * (mr + 2 * lw)));
    g.FillEllipse(&fill, static_cast<REAL>(x - mr), static_cast<REAL>(y - mr), static_cast<REAL>(2 * mr),
                  static_cast<REAL>(2 * mr));
    mark_[id] = {static_cast<LONG>(x + 0.5), static_cast<LONG>(y + 0.5)};
}

// The curve of the level-and-curve graph is still, yet it lies over the level, which moves: it has a layer of its
// own - a 32-bit bitmap with transparency, drawn here when a value, the size or the DPI changes and laid over the
// level at every refresh (paintLive). Drawn with the level at every refresh instead, its long dashed and dotted
// anti-aliased lines took a quarter of a processor core.
void CompressorGraphs::renderCurve(int w, int h, int th) {
    curveNames_.clear();
    if (!curve_ || !curveBits_) return;
    const double thr = threshold(), mk = makeup();
    {
        using namespace Gdiplus;
        Bitmap layer(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(curveBits_));
        Graphics g(&layer);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        const float lw = static_cast<float>(std::max(1.0, dpi_ / 96.0));
        // ---- the curve, over the level and on its vertical scale - so the four values set here are level lines
        // across both: the threshold (music above it is compressed), the knee's band around it, the ratio through
        // the curve's loud end (where the loudest music comes out), the make-up through its quiet end (which it
        // lifts, and the whole curve with it). ----
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
        // The marks: the threshold's where its line crosses the diagonal - the level at which the curve bends -,
        // the ratio's and the make-up's on the curve's ends, the knee's on the band's upper edge at the left (on
        // the threshold's line at a knee of 0, where it can still be taken).
        drawMark(g, mThreshold, xThr, yThr);
        drawMark(g, mRatio, xRatio, yRatio);
        drawMark(g, mMakeup, xMakeup, yMakeup);
        drawMark(g, mKnee, xKnee, yKneeT);
        // Each name stands at its line beside its mark: the threshold's and the ratio's left of their marks, the
        // make-up's and the knee's right of theirs (client, 2026-11-13: "ratio text left of the mark, threshold
        // text left of the mark, make-up text right of the mark"). On the line, where the curve is not: left of a
        // mark the curve lies under the line, right of the make-up's mark it rises over it - that name hangs under
        // its line. A name that would leave the plot at the top goes under its line too, and so does the
        // threshold's where the ratio's name is in its way.
        const double above = th + px(2), under = px(2), side = px(8);
        double yRatioName = yRatio - above < q.t ? yRatio + under : yRatio - above;
        double yThrName = yThr - above;
        if (xRatio - side - nameW_[1] < xThr - side && std::fabs(yRatioName - yThrName) < th) yThrName = yThr + under;
        curveNames_.push_back({static_cast<int>(xThr - side), static_cast<int>(yThrName), L"Threshold", kOrange, TA_RIGHT});
        curveNames_.push_back({static_cast<int>(xRatio - side), static_cast<int>(yRatioName), L"Ratio", kOrange, TA_RIGHT});
        curveNames_.push_back({static_cast<int>(xMakeup + side), static_cast<int>(yMakeup + under), L"Make-up", kOrange, TA_LEFT});
        curveNames_.push_back({static_cast<int>(xKnee + side), static_cast<int>(yKneeT - above < q.t ? yKneeT + under : yKneeT - above),
                         L"Knee", kOrange, TA_LEFT});
    }
}

// The parts that move with the audio, on a copy of the still picture: the level and the reduction of the last 4 s
// with the curve over them, and the side chain's spectrum with its filter.
void CompressorGraphs::paintLive(HDC dc, int th) {
    const bool live = source_ && source_->audioActive() && source_->meters(deck_).live;
    const DeckAnalysis* an = source_ ? &source_->analysis(deck_) : nullptr;
    const bool have = an && live && an->compCount > 0;
    const double mk = makeup(), corner = values_[pCompSideHpf], level0 = aim();
    const int hair = std::max(1, px(1));
    SetBkMode(dc, TRANSPARENT);
    auto polygon = [&](const std::vector<POINT>& pts, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b);
    };
    auto polyline = [&](const std::vector<POINT>& pts, COLORREF c, int width, int style = PS_SOLID) {
        HPEN pen = CreatePen(style, width, c);
        HGDIOBJ op = SelectObject(dc, pen);
        Polyline(dc, pts.data(), static_cast<int>(pts.size()));
        SelectObject(dc, op);
        DeleteObject(pen);
    };
    auto text = [&](double x, double y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT) {
        SetTextColor(dc, c);
        SetTextAlign(dc, align | TA_TOP);
        TextOutW(dc, static_cast<int>(x + 0.5), static_cast<int>(y + 0.5), s.c_str(), static_cast<int>(s.size()));
    };
    auto clip = [&](const Plot& q) {
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, static_cast<int>(q.l) - 1, static_cast<int>(q.t) - 1, static_cast<int>(q.r) + 2,
                          static_cast<int>(q.b) + 2);
        return saved;
    };
    const COLORREF inputArea = blend(kInput, kPanel, 0.28), heardArea = blend(kTeal, inputArea, 0.32);

    // ---- 1. the level and the reduction of the last 4 s: the background of the curve, which is drawn over it
    // below. Dimmer than the curve; time runs over the plot's whole width, the newest at the right edge. ----
    const Plot& qc = plot_[0];
    double most = 0.0, nowIn = kLo - 1.0; // the largest reduction in view; the newest level (LU to the aim)
    {
        const int saved = clip(qc);
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
            polygon(in, blend(kInput, kPanel, 0.2));
            polygon(red, blend(kOrange, kPanel, 0.5));
            polyline(outLine, blend(kTeal, kPanel, 0.55), std::max(1, px(1.5)));
            nowIn = an->compAt(DeckAnalysis::kComp - 1).in - level0;
        }
        RestoreDC(dc, saved);
    }
    // The curve's layer over the level (GdiAlphaBlend: gdi32's own), then a thin line from the newest end of the
    // level - the right edge - across to where that level is on the curve.
    if (curve_) {
        const int x = static_cast<int>(qc.l) - px(9), y = static_cast<int>(qc.t) - px(9);
        const int cw = static_cast<int>(qc.r - qc.l) + 2 * px(9), ch = static_cast<int>(qc.b - qc.t) + 2 * px(9);
        HDC layer = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(layer, curve_);
        const BLENDFUNCTION over{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        GdiAlphaBlend(dc, x, y, cw, ch, layer, x, y, cw, ch, over);
        SelectObject(layer, old);
        DeleteDC(layer);
    }
    if (have && nowIn >= kLo) {
        const int saved = clip(qc);
        polyline({ipt(qc.X(nowIn), qc.Y(nowIn)), ipt(qc.r, qc.Y(nowIn))}, blend(kText, kPanel, 0.5), 1, PS_DOT);
        RestoreDC(dc, saved);
    }

    // ---- 4. the side chain: the spectrum (GDI), then its filter and mark (GDI+, below) ----
    Plot& qs = plot_[3];
    auto filterY = [&](double db) { return qs.t + std::min(1.0, std::max(0.0, -db / kFilterRangeDb)) * (qs.b - qs.t); };
    const double xCorner = corner > 0.0 ? qs.X(corner) : qs.l;
    {
        const int saved = clip(qs);
        if (an && live && an->specIn.frames() > 0) {
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
            polygon(raw, inputArea);
            polygon(heard, heardArea);
            if (heardLine.size() > 1) polyline(heardLine, kTeal, hair);
        }
        for (double hz : {50.0, 100.0, 200.0, 500.0}) polyline({ipt(qs.X(hz), qs.t), ipt(qs.X(hz), qs.b)}, kGrid, 1, PS_DOT);
        if (corner > 0.0) polyline({ipt(xCorner, qs.t), ipt(xCorner, qs.b)}, kOrange, 1, PS_DASH);
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
    for (const Label& n : curveNames_) text(n.x, n.y, n.text, n.colour, n.align);
    if (have) text(panel_[0].right - px(6), panel_[0].top + px(3), number(-most, 1, L" dB"), kOrange, TA_RIGHT);
    else text((qc.l + qc.r) / 2, qc.b - th - px(4), source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
    text(qs.X(100.0) + px(3), qs.b - th - px(2), L"100", kText); // over the spectrum's filled area: the light colour
    text(qs.r - px(3), qs.b - th - px(2), L"1 k", kText, TA_RIGHT);
    text(xCorner + px(8), filterY(corner > 0.0 ? -3.0 : 0.0) + px(6),
         corner > 0.0 ? number(corner, 0, L" Hz") : std::wstring(L"off"), kOrange);
}

void CompressorGraphs::paint(HDC target) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    if (!still_ || !frame_ || w != bitmapW_ || h != bitmapH_) {
        if (still_) DeleteObject(still_);
        if (frame_) DeleteObject(frame_);
        if (curve_) DeleteObject(curve_);
        still_ = CreateCompatibleBitmap(target, w, h);
        frame_ = CreateCompatibleBitmap(target, w, h);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down: the first row first, as GDI+ wants it
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        curveBits_ = nullptr;
        curve_ = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &curveBits_, nullptr, 0);
        bitmapW_ = w;
        bitmapH_ = h;
        stillDirty_ = true;
    }
    HDC dc = CreateCompatibleDC(target), from = CreateCompatibleDC(target);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight;
    HGDIOBJ oldBmp = SelectObject(dc, still_);
    if (stillDirty_) {
        renderStill(dc, w, h, th);
        renderCurve(w, h, th);
        stillDirty_ = false;
    }
    SelectObject(dc, frame_);
    HGDIOBJ oldFrom = SelectObject(from, still_);
    BitBlt(dc, 0, 0, w, h, from, 0, 0, SRCCOPY);
    SelectObject(from, oldFrom);
    paintLive(dc, th);
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteDC(from);
    DeleteDC(dc);
}

int CompressorGraphs::markAt(POINT p) const {
    const int reach = px(11);
    int best = mNone;
    long bestD = static_cast<long>(reach) * reach + 1;
    for (int i = 0; i < kMarks; ++i) {
        const long dx = p.x - mark_[i].x, dy = p.y - mark_[i].y, d = dx * dx + dy * dy;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// The level lines of the level-and-curve graph - the threshold's, the ratio's, the make-up's, and the two edges of
// the knee's band - are taken anywhere along them, not only by their marks: the nearest one within a few pixels (in
// that order where they lie together; a knee of 0 is taken by its mark).
int CompressorGraphs::lineAt(POINT p) const {
    const Plot& q = plot_[0];
    if (p.x < q.l - 2 || p.x > q.r + 2 || p.y < q.t - 2 || p.y > q.b + 2) return mNone;
    const long reach = px(5);
    const double half = values_[pCompKnee] / 2.0;
    const long kneeT = std::lround(q.Y(threshold() + half)), kneeB = std::lround(q.Y(threshold() - half));
    const long d[4] = {std::labs(p.y - mark_[mThreshold].y), std::labs(p.y - mark_[mRatio].y),
                       std::labs(p.y - mark_[mMakeup].y),
                       half > 0.0 ? std::min(std::labs(p.y - kneeT), std::labs(p.y - kneeB)) : reach + 1};
    static const int kLine[4] = {mThreshold, mRatio, mMakeup, mKnee};
    int best = mNone;
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
    for (int k = 0; k < kFirstLegend; ++k)
        if (PtInRect(&panel_[cornerPanel(k)], p)) return resetParam(k);
    for (int j = 0; j < kLegend; ++j)
        if (PtInRect(&legendItem_[j], p) || PtInRect(&reset_[kFirstLegend + j], p)) return resetParam(kFirstLegend + j);
    return -1;
}

// One wheel notch is one step of the value. The attack and the release go by the parameter's own step (1 ms, 10 ms),
// not by a slider position: on their logarithmic scales one position often is the same value, and at the long end a
// position skips values. The others are on linear scales, where a position is a step (and the make-up's first
// position is "auto").
double CompressorGraphs::stepped(int param, int steps) const {
    const ParamInfo& in = paramInfo(param);
    const double now = values_[static_cast<size_t>(param)];
    if (param == pCompAttack || param == pCompRelease)
        return std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
    return paramValueAt(param, std::min(paramPositions(param), std::max(0, paramPositionOf(param, now) + steps)));
}

// The mark being dragged follows the mouse along its own direction; its value goes on one of the slider's positions.
void CompressorGraphs::drag(POINT p) {
    int param = -1;
    double v = 0.0;
    switch (drag_) {
    case mThreshold: // a level line: up / down
        param = pCompThreshold;
        v = plot_[0].fromY(p.y);
        break;
    case mRatio: { // the ratio that brings the curve's right end to the mouse (the curve falls as the ratio rises)
        param = pCompRatio;
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
        break;
    }
    case mMakeup: {
        param = pCompMakeup;
        const double mk = plot_[0].fromY(p.y) - (kLo + kMakeupMarkLu);
        v = mk < -0.3 ? -1.0 : std::min(paramInfo(pCompMakeup).max, std::max(0.0, mk)); // under 0: auto
        break;
    }
    case mKnee: // the band is as wide on both sides of the threshold: either edge (and the mark) sets half of it
        param = pCompKnee;
        v = 2.0 * std::fabs(plot_[0].fromY(p.y) - threshold());
        break;
    case mAttack:
        param = pCompAttack;
        v = plot_[1].fromX(p.x) - kHitAtMs;
        break;
    case mRelease:
        param = pCompRelease;
        v = plot_[2].fromX(p.x);
        break;
    case mSideHpf: {
        param = pCompSideHpf;
        const double hz = plot_[3].fromX(p.x);
        v = hz < 34.0 ? 0.0 : std::max(40.0, hz); // left of the lowest corner: off
        break;
    }
    default:
        return;
    }
    const ParamInfo& in = paramInfo(param);
    if (param != pCompMakeup && param != pCompSideHpf) v = std::max(in.min, v);
    v = std::min(in.max, v);
    const double snapped = paramValueAt(param, paramPositionOf(param, v));
    if (snapped != values_[static_cast<size_t>(param)]) change(param, snapped, false);
}

// A value set in the graph: the picture first, then the owner (heard at once; saved when `save`).
void CompressorGraphs::change(int param, double value, bool save) {
    values_[static_cast<size_t>(param)] = value;
    stillDirty_ = true;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (edit_) edit_(param, value, save);
}

int CompressorGraphs::resetAt(POINT p) const {
    for (int k = 0; k < kResets; ++k)
        if (PtInRect(&reset_[k], p) && resetLit(k)) return k;
    return -1;
}

LRESULT CALLBACK CompressorGraphs::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<CompressorGraphs*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<CompressorGraphs*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) { // the window goes (with its parent): the object lives on without it
        self->hwnd_ = nullptr;
        self->drag_ = mNone;
        self->press_ = -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->handle(msg, wp, lp);
}

LRESULT CompressorGraphs::handle(UINT msg, WPARAM wp, LPARAM lp) {
    static const int kParamOf[kMarks] = {pCompThreshold, pCompRatio, pCompMakeup, pCompAttack,
                                         pCompRelease,   pCompSideHpf, pCompKnee};
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        press_ = resetAt(p); // a reset icon acts when the button is let go on it, as a button
        if (press_ < 0) drag_ = markAt(p);
        if (press_ < 0 && drag_ == mNone) drag_ = lineAt(p);
        if (press_ >= 0 || drag_ != mNone) SetCapture(hwnd_);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag_ != mNone) drag(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_LBUTTONUP:
        if (press_ >= 0) {
            const int k = press_;
            press_ = -1;
            ReleaseCapture();
            const int param = resetParam(k);
            if (resetAt(POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))}) == k)
                change(param, resetValues_[static_cast<size_t>(param)], true);
        } else if (drag_ != mNone) {
            const int param = kParamOf[drag_];
            drag_ = mNone;
            ReleaseCapture();
            if (edit_) edit_(param, values_[static_cast<size_t>(param)], true); // saved now
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (drag_ != mNone) { // taken away in the middle of a drag: what is heard is saved
            const int param = kParamOf[drag_];
            drag_ = mNone;
            if (edit_) edit_(param, values_[static_cast<size_t>(param)], true);
        }
        press_ = -1;
        return 0;
    case WM_MOUSEWHEEL: { // one step of a value that has no slider: the fine control the slider's arrow keys were
        POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd_, &p);
        const int param = drag_ == mNone ? wheelParam(p) : -1;
        if (param < 0) break;
        wheel_ += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = wheel_ / WHEEL_DELTA;
        wheel_ -= steps * WHEEL_DELTA;
        const double v = stepped(param, steps);
        if (steps && v != values_[static_cast<size_t>(param)]) change(param, v, true);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd_, &p);
            if (press_ >= 0 || (drag_ == mNone && resetAt(p) >= 0)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            int m = drag_ != mNone ? drag_ : markAt(p);
            if (m == mNone) m = lineAt(p);
            if (m != mNone) {
                // the level lines go up / down, the marks of the attack, the release and the high-pass left / right
                SetCursor(LoadCursorW(nullptr, m == mAttack || m == mRelease || m == mSideHpf ? IDC_SIZEWE : IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace ap
