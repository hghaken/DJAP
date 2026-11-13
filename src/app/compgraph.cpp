// SPDX-License-Identifier: GPL-3.0-or-later
#include "compgraph.h"

#include <objidl.h>

#include <algorithm>
#include <cmath>
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
const double kSpecLoHz = 30.0, kSpecHiHz = 16000.0, kSpecRangeDb = 54.0, kFilterRangeDb = 36.0;

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

int CompressorGraphs::resetParam(int k) { return k == 0 ? pCompAttack : pCompRelease; }

bool CompressorGraphs::resetLit(int k) const {
    const size_t p = static_cast<size_t>(resetParam(k));
    return std::fabs(values_[p] - resetValues_[p]) > 1e-9;
}

// A panel with a reset icon (attack: panel 1, release: panel 2) is two rectangles around the icon's own: what is
// left of the icon, and what is under it.
RECT CompressorGraphs::tipRect(int i) {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    layoutPanels(rc.right, rc.bottom);
    if (i < 0 || i >= kTips) return RECT{};
    if (i >= kPanels + kResets) return reset_[i - kPanels - kResets];
    if (i >= kPanels) {
        const int k = i - kPanels;
        return RECT{reset_[k].left, reset_[k].bottom, panel_[1 + k].right, panel_[1 + k].bottom};
    }
    RECT r = panel_[i];
    if (i >= 1 && i < 1 + kResets) r.right = reset_[i - 1].left;
    return r;
}

const wchar_t* CompressorGraphs::tipText(int i) {
    if (i >= kPanels + kResets) return nullptr;       // a reset icon: the owner's text
    if (i >= kPanels) i = 1 + (i - kPanels);          // the rest of the attack / release panel
    static const wchar_t* const kPanelTips[kPanels] = {
        L"The compressor's curve: the level going in (to the right) against the level coming out (up), in LU to the "
        L"target. Under the threshold nothing happens (the dotted diagonal); above it the line bends - flatter with a "
        L"higher ratio, rounder with a wider knee (the shaded band). The grey bars show where the music's level was in "
        L"the last seconds, the white dot where it is now. Drag the marks: the threshold (left / right), the ratio (up / "
        L"down, at the right end) and the make-up (up / down, at the left; all the way down = auto).",
        L"Attack (1 .. 100 ms): a hit 10 dB over the threshold in its first 150 ms. Dashed: the hit as it comes in; "
        L"the line: what comes out. What is above the line before the mark still gets through - the punch. Shorter "
        L"catches the hit sooner, longer lets it through.\n\nSet it here: drag the mark, or turn the mouse wheel over "
        L"this graph for one step of 1 ms. The arrow in the corner sets it back.",
        L"Release (20 .. 1000 ms): the gain on the rest of the music after that hit. It is still down when the hit "
        L"ends and comes back in the release time. Short: back before the next beat - lively, but the music goes up "
        L"and down with every beat (pumping). Long: smooth.\n\nSet it here: drag the mark, or turn the mouse wheel "
        L"over this graph for one step of 10 ms. The arrow in the corner sets it back.",
        L"The last 4 seconds of this deck: the level at the compressor (grey - what its side chain hears), the level it "
        L"lets out (line) and, from the top, how much it turns down (orange). The dashed line is the threshold.",
        L"The side chain: the music's spectrum (grey) and the part of it the compressor listens to (coloured) - what the "
        L"high-pass leaves of the bass. A kick the compressor hears pulls the whole mix down with every beat. Drag the "
        L"mark left or right; all the way left = off.",
    };
    return i >= 0 && i < kPanels ? kPanelTips[i] : L"";
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

// Five panels side by side: the curve, attack, release, the level, the side chain - the two live ones a little wider.
// A strip much taller than its panels are wide (a narrow window) gets two rows instead: the curve, attack and release
// above, the level and the side chain - the wide ones - below.
void CompressorGraphs::layoutPanels(int w, int h) {
    static const double kShare[kPanels] = {1.0, 0.8, 1.0, 1.3, 1.2};
    const int gap = px(5), pad = px(5);
    const double oneRow = (w - 2 * pad - (kPanels - 1) * gap) / static_cast<double>(kPanels); // a panel's width in one row
    const bool twoRows = h - 2 * pad >= 2 * px(110) + gap && h - 2 * pad > 1.9 * oneRow;
    auto row = [&](int first, int count, int top, int bottom) {
        double sum = 0.0;
        for (int i = first; i < first + count; ++i) sum += kShare[i];
        const double usable = w - 2 * pad - (count - 1) * gap;
        double x = pad;
        for (int i = first; i < first + count; ++i) {
            const double pw = usable * kShare[i] / sum;
            panel_[i] = {static_cast<LONG>(x + 0.5), top, static_cast<LONG>(x + pw + 0.5), bottom};
            x += pw + gap;
        }
    };
    if (twoRows) {
        const int mid = pad + (h - 2 * pad - gap) / 2;
        row(0, 3, pad, mid);
        row(3, 2, mid + gap, h - pad);
    } else {
        row(0, kPanels, pad, h - pad);
    }
    // the reset icons: in the title line of the attack and release panels, at the right (above the plot)
    for (int k = 0; k < kResets; ++k) {
        const RECT& p = panel_[1 + k];
        reset_[k] = {p.right - px(3) - px(22), p.top + px(1), p.right - px(3), p.top + px(1) + px(18)};
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
    static const wchar_t* const kTitle[kPanels] = {L"Curve", L"Attack", L"Release", L"Level and reduction, 4 s",
                                                   L"Side chain"};
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
        Pen grid(col(kGrid), lw), dots(col(kGrid), lw), dash(col(kOrange), lw), curve(col(kTeal), 2.0f * lw),
            inputDash(col(kInput), 1.5f * lw), edge(col(kEdge), 1.0f);
        dots.SetDashStyle(DashStyleDot);
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
            label(p.left + px(6), p.top + px(3), i == 0 && !on ? std::wstring(L"Curve (compressor off)") : kTitle[i],
                  kDim);
            Plot& q = plot_[i];
            q.l = p.left + px(8);
            q.r = p.right - px(8);
            q.t = p.top + th + px(7);
            q.b = p.bottom - px(8);
            q.logX = false;
        }
        const Region whole(Rect(0, 0, w, h));
        auto clipTo = [&](const Plot& q) {
            g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                            static_cast<REAL>(q.b - q.t + 2)));
        };

        // ---- 1. the curve ----
        {
            Plot& q = plot_[0];
            q.x0 = q.y0 = kLo;
            q.x1 = q.y1 = kHi;
            const double side = std::min(q.r - q.l, q.b - q.t); // in and out share a scale: square, centred
            q.l += (q.r - q.l - side) / 2;
            q.r = q.l + side;
            q.t += (q.b - q.t - side) / 2;
            q.b = q.t + side;
            clipTo(q);
            g.DrawLine(&dots, pt(q.X(0), q.t), pt(q.X(0), q.b));
            g.DrawLine(&dots, pt(q.l, q.Y(0)), pt(q.r, q.Y(0)));
            if (knee > 0.0)
                g.FillRectangle(&orangeFill, static_cast<REAL>(q.X(thr - knee / 2)), static_cast<REAL>(q.t),
                                static_cast<REAL>(q.X(thr + knee / 2) - q.X(thr - knee / 2)), static_cast<REAL>(q.b - q.t));
            g.DrawLine(&dots, pt(q.X(kLo), q.Y(kLo + mk)), pt(q.X(kHi - mk), q.Y(kHi))); // no compression
            g.DrawLine(&dash, pt(q.X(thr), q.t), pt(q.X(thr), q.b));
            std::vector<PointF> line;
            for (int i = 0; i <= 96; ++i) {
                const double in = kLo + (kHi - kLo) * i / 96.0;
                line.push_back(pt(q.X(in), q.Y(out(in))));
            }
            g.DrawLines(&curve, line.data(), static_cast<INT>(line.size()));
            g.SetClip(&whole);
            const double xr = kHi - kRatioMarkLu, xm = kLo + kMakeupMarkLu;
            drawMark(g, mThreshold, q.X(thr), q.Y(out(thr)));
            drawMark(g, mRatio, q.X(xr), q.Y(out(xr)));
            drawMark(g, mMakeup, q.X(xm), q.Y(out(xm)));
            const bool thrRight = q.X(thr) > q.l + 0.62 * (q.r - q.l);
            // the texts keep off the line: the threshold's at the top of its dashed line, the ratio's above its mark
            // (the line comes from below there), the make-up's under its mark (the line goes up from it)
            label(q.X(thr) + (thrRight ? -px(4) : px(4)), q.t, number(thr, 1, L" LU", true), kOrange,
                  thrRight ? TA_RIGHT : TA_LEFT);
            label(q.X(xr) + px(2), std::max(q.t, q.Y(out(xr)) - th - px(7)), number(ratio, 1, L":1"), kText, TA_RIGHT);
            label(q.X(xm) + px(8), std::min(q.b - th, q.Y(out(xm)) + px(2)),
                  values_[pCompMakeup] < 0.0 ? std::wstring(L"auto") : number(mk, 1, L" dB", true), kText);
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

        // ---- 4 and 5: their scales (drawn with the audio, in paintLive) ----
        plot_[3].x0 = 0.0;
        plot_[3].x1 = kTimePoints - 1.0;
        plot_[3].y0 = kLo;
        plot_[3].y1 = kHi;
        plot_[4].logX = true;
        plot_[4].x0 = kSpecLoHz;
        plot_[4].x1 = kSpecHiHz;
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

// The parts that move with the audio, on a copy of the still picture: the level and the reduction of the last 4 s,
// the side chain's spectrum with its filter, and on the curve where the level was and is.
void CompressorGraphs::paintLive(HDC dc, int th) {
    const bool live = source_ && source_->audioActive() && source_->meters(deck_).live;
    const DeckAnalysis* an = source_ ? &source_->analysis(deck_) : nullptr;
    const bool have = an && live && an->compCount > 0;
    const double thr = threshold(), mk = makeup(), corner = values_[pCompSideHpf], level0 = aim();
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

    // ---- 4. the level and the reduction of the last 4 s ----
    {
        const Plot& q = plot_[3];
        const RECT& p = panel_[3];
        const int saved = clip(q);
        double most = 0.0;
        if (have) {
            std::vector<POINT> in, outLine, red;
            in.push_back(ipt(q.l, q.b));
            red.push_back(ipt(q.l, q.t));
            const double perDb = 0.4 * (q.b - q.t) / 12.0;
            for (int i = 0; i < kTimePoints; ++i) {
                const DeckAnalysis::CompPoint& c = an->compAt(DeckAnalysis::kComp - kTimePoints + i);
                const double lu = std::max(kLo, c.in - level0), x = q.X(i);
                in.push_back(ipt(x, q.Y(lu)));
                outLine.push_back(ipt(x, q.Y(c.in - level0 < kLo ? kLo : lu - c.reduction + mk)));
                red.push_back(ipt(x, q.t + std::min(12.0, static_cast<double>(c.reduction)) * perDb));
                most = std::max(most, static_cast<double>(c.reduction));
            }
            in.push_back(ipt(q.r, q.b));
            red.push_back(ipt(q.r, q.t));
            polygon(in, inputArea);
            polygon(red, blend(kOrange, kPanel, 0.6));
            polyline(outLine, kTeal, std::max(1, px(2)));
        }
        polyline({ipt(q.l, q.Y(0)), ipt(q.r, q.Y(0))}, kGrid, 1, PS_DOT);
        polyline({ipt(q.l, q.Y(thr)), ipt(q.r, q.Y(thr))}, kOrange, 1, PS_DASH);
        RestoreDC(dc, saved);
        if (have) text(p.right - px(6), p.top + px(3), number(-most, 1, L" dB"), kOrange, TA_RIGHT);
        else text((q.l + q.r) / 2, (q.t + q.b) / 2 - th / 2, source_ ? L"no audio on this deck" : L"no deck plays this", kDim, TA_CENTER);
    }

    // ---- 5. the side chain: the spectrum (GDI), then its filter and mark (GDI+, below) ----
    Plot& qs = plot_[4];
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
        for (double hz : {100.0, 1000.0, 10000.0}) polyline({ipt(qs.X(hz), qs.t), ipt(qs.X(hz), qs.b)}, kGrid, 1, PS_DOT);
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
        // the curve: where the level was in the last seconds (bars), the last half second (a trail) and now
        if (have) {
            const Plot& q = plot_[0];
            g.SetClip(RectF(static_cast<REAL>(q.l - 1), static_cast<REAL>(q.t - 1), static_cast<REAL>(q.r - q.l + 2),
                            static_cast<REAL>(q.b - q.t + 2)));
            const int bins = static_cast<int>(kHi - kLo);
            std::vector<int> count(static_cast<size_t>(bins), 0);
            int most = 1;
            for (int i = 0; i < DeckAnalysis::kComp; ++i) {
                const int b = static_cast<int>(std::floor(an->compAt(i).in - level0 - kLo));
                if (b >= 0 && b < bins) most = std::max(most, ++count[static_cast<size_t>(b)]);
            }
            SolidBrush bar(col(kInput, 90));
            const double bw = (q.r - q.l) / bins, top = 0.3 * (q.b - q.t);
            for (int b = 0; b < bins; ++b) {
                const double bh = top * count[static_cast<size_t>(b)] / most;
                if (bh >= 0.5)
                    g.FillRectangle(&bar, static_cast<REAL>(q.l + b * bw + 0.5), static_cast<REAL>(q.b - bh),
                                    static_cast<REAL>(std::max(1.0, bw - 1.0)), static_cast<REAL>(bh));
            }
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
        }
    }
    text(qs.X(1000.0) + px(3), qs.b - th - px(2), L"1 k", kDim);
    text(qs.X(10000.0) + px(3), qs.b - th - px(2), L"10 k", kDim);
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
        still_ = CreateCompatibleBitmap(target, w, h);
        frame_ = CreateCompatibleBitmap(target, w, h);
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

// The mark being dragged follows the mouse along its own direction; its value goes on one of the slider's positions.
void CompressorGraphs::drag(POINT p) {
    int param = -1;
    double v = 0.0;
    switch (drag_) {
    case mThreshold:
        param = pCompThreshold;
        v = plot_[0].fromX(p.x);
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
        const double hz = plot_[4].fromX(p.x);
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
    static const int kParamOf[kMarks] = {pCompThreshold, pCompRatio, pCompMakeup, pCompAttack, pCompRelease, pCompSideHpf};
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
    case WM_MOUSEWHEEL: { // over the attack or the release graph: one of the value's steps per notch (no slider)
        POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd_, &p);
        for (int k = 0; k < kResets && drag_ == mNone; ++k) {
            if (!PtInRect(&panel_[1 + k], p)) continue;
            wheel_ += GET_WHEEL_DELTA_WPARAM(wp);
            const int steps = wheel_ / WHEEL_DELTA;
            wheel_ -= steps * WHEEL_DELTA;
            // the parameter's own step (1 ms, 10 ms), not a slider position: on these logarithmic scales one
            // position often is the same value, and at the long end a position skips values
            const int param = resetParam(k);
            const ParamInfo& in = paramInfo(param);
            const double now = values_[static_cast<size_t>(param)];
            const double v = std::min(in.max, std::max(in.min, std::round(now / in.step + steps) * in.step));
            if (steps && v != now) change(param, v, true);
            return 0;
        }
        break;
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
            const int m = drag_ != mNone ? drag_ : markAt(p);
            if (m != mNone) {
                SetCursor(LoadCursorW(nullptr, m == mRatio || m == mMakeup ? IDC_SIZENS : IDC_SIZEWE));
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
