#include "spectrumdraw.h"

#include "colour.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ap::spectrum {

namespace {

// the meter panel's palette (meterview.cpp)
const COLORREF kPlot = RGB(22, 24, 27), kGrid = RGB(70, 74, 80), kDim = RGB(120, 126, 134), kText = RGB(214, 218, 224),
               kOrange = RGB(240, 130, 50), kTeal = RGB(80, 215, 195),
               kPurple = RGB(175, 145, 245), kWell = RGB(44, 47, 52), kCurve = RGB(250, 220, 90);
// the tone sections' own colours (points and their thin curves); the loudness contour grey
const COLORREF kNodeColour[kNodes] = {RGB(120, 175, 255), RGB(205, 150, 255), RGB(255, 190, 110), RGB(130, 230, 160)};
const COLORREF kLoudColour = RGB(150, 155, 162);
// the density bands' gains: their own colours, apart from the spectra's orange and turquoise (client, 2026-10-26)
const COLORREF kUp = RGB(90, 140, 255), kDown = RGB(240, 70, 110);
constexpr double kSpecTop = 0.0, kSpecBottom = -96.0; // spectrum scale (dB): 8 grid lines of 12 dB
constexpr double kDensityRange = 12.0;                 // the density bands' gains: +-12 dB

void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    if (r <= l || b <= t) return;
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int style = PS_SOLID, int width = 1) {
    HPEN pen = CreatePen(style, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void text(HDC dc, int x, int y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT) {
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

void polyline(HDC dc, const std::vector<POINT>& pts, COLORREF c, int width, int style = PS_SOLID) {
    if (pts.size() < 2) return;
    HPEN pen = CreatePen(style, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(dc, old);
    DeleteObject(pen);
}

// c over the plot's background with opacity a (0..1): a shade without alpha blending (the background is one colour)
COLORREF over(COLORREF c, double a) {
    auto mix = [a](int fg, int bg) { return static_cast<int>(bg + (fg - bg) * a + 0.5); };
    return RGB(mix(GetRValue(c), 22), mix(GetGValue(c), 24), mix(GetBValue(c), 27)); // kPlot
}

std::wstring hzText(double f) {
    wchar_t b[16];
    if (f < 1000.0) swprintf(b, 16, L"%.0f", f);
    else if (std::fmod(f, 1000.0) == 0.0) swprintf(b, 16, L"%.0fk", f / 1000.0);
    else swprintf(b, 16, L"%.1fk", f / 1000.0);
    return b;
}

std::wstring dbText(double db) {
    wchar_t b[16];
    swprintf(b, 16, std::fabs(db) < 0.05 ? L"0.0" : L"%+.1f", db);
    return b;
}

} // namespace

const Param kNodeFreq[kNodes] = {pToneBassFreq, pToneLowMidFreq, pTonePresenceFreq, pToneTrebleFreq};
const Param kNodeGain[kNodes] = {pToneBass, pToneLowMid, pTonePresence, pToneTreble};

const wchar_t* nodeName(int n) {
    static const wchar_t* const names[kNodes] = {L"Bass", L"Low-mid", L"Presence", L"Treble"};
    return n >= 0 && n < kNodes ? names[n] : L"";
}

int Geometry::xOf(double hz) const {
    const double lo = SpectrumAnalyzer::kLowHz, hi = SpectrumAnalyzer::kHighHz;
    return plot.left + static_cast<int>((plot.right - plot.left) * std::log(hz / lo) / std::log(hi / lo) + 0.5);
}

double Geometry::hzAt(int x) const {
    const double lo = SpectrumAnalyzer::kLowHz, hi = SpectrumAnalyzer::kHighHz;
    const double t = static_cast<double>(x - plot.left) / std::max(1, static_cast<int>(plot.right - plot.left));
    return lo * std::pow(hi / lo, std::min(1.0, std::max(0.0, t)));
}

int Geometry::yOfEq(double db) const {
    const double t = (std::min(eqRange, std::max(-eqRange, db)) + eqRange) / (2.0 * eqRange);
    return plot.bottom - static_cast<int>(t * (plot.bottom - plot.top) + 0.5);
}

double Geometry::eqAt(int y) const {
    const double t = static_cast<double>(plot.bottom - y) / std::max(1, static_cast<int>(plot.bottom - plot.top));
    return -eqRange + 2.0 * eqRange * t;
}

int Geometry::handleAt(POINT pt, int tolerance) const {
    for (int b = 0; b < handles; ++b) {
        const RECT& r = handle[b];
        if (pt.x >= r.left && pt.x < r.right && pt.y >= (r.top + r.bottom) / 2 - tolerance &&
            pt.y <= (r.top + r.bottom) / 2 + tolerance)
            return b;
    }
    return -1;
}

int Geometry::nodeAt(POINT pt, int radius) const {
    if (!nodes) return -1;
    int best = -1;
    long bestD = static_cast<long>(radius) * radius;
    for (int n = 0; n < kNodes; ++n) {
        const long dx = pt.x - node[n].x, dy = pt.y - node[n].y;
        if (dx * dx + dy * dy <= bestD) {
            bestD = dx * dx + dy * dy;
            best = n;
        }
    }
    return best;
}

Geometry paint(HDC dc, const RECT& area, const Data& d, HFONT small, UINT dpi, bool big) {
    const double s = dpi / 96.0;
    auto px = [s](double x) { return static_cast<int>(x * s + 0.5); };
    Geometry g;
    fill(dc, area.left, area.top, area.right, area.bottom, kPlot);
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, kPlot); // the gaps of the dotted lines
    HGDIOBJ oldFont = SelectObject(dc, small);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight;
    const int leftW = big ? px(34) : 0, rightW = px(30);
    g.plot = RECT{area.left + px(4) + leftW, area.top + th + px(4), area.right - rightW, area.bottom - th - px(4)};
    g.eqRange = kDensityRange; // +-12 dB: 8 steps of 3 dB, each on a spectrum grid line (client, 2026-10-26)
    const RECT& p = g.plot;
    if (p.right - p.left < px(60) || p.bottom - p.top < px(30)) {
        SelectObject(dc, oldFont);
        return g;
    }
    const double lo = SpectrumAnalyzer::kLowHz;
    auto yOfSpec = [&](double db) {
        const double t = (std::min(kSpecTop, std::max(kSpecBottom, db)) - kSpecBottom) / (kSpecTop - kSpecBottom);
        return p.bottom - static_cast<int>(t * (p.bottom - p.top) + 0.5);
    };
    // the bands: [edge b, edge b + 1] on the frequency axis
    const int bands = d.densityOn ? std::max(1, std::min(kMaxBands, d.bands)) : 0;
    auto edge = [&](int b) { return b <= 0 ? p.left : b >= bands ? p.right : g.xOf(d.splits[b - 1]); };
    auto bandColour = [](double db) { return std::fabs(db) < 0.05 ? kDim : db > 0.0 ? kUp : kDown; };

    // ---- grid: spectrum dB (right; in the window also left), frequencies, the EQ scale's 0 dB ----
    for (int db = -12; db >= -84; db -= 12) {
        const int y = yOfSpec(db);
        line(dc, p.left, y, p.right, y, kGrid, PS_DOT);
        text(dc, big ? p.left - px(4) : area.right - px(3), y - th / 2, std::to_wstring(db), kDim, TA_RIGHT);
    }
    if (big) {
        for (int db = -12; db <= 12; db += 3) {
            const int y = g.yOfEq(db);
            wchar_t b[8];
            swprintf(b, 8, db == 0 ? L"0" : L"%+d", db);
            text(dc, area.right - px(3), y - th / 2, b, kCurve, TA_RIGHT);
        }
    }
    const struct {
        double f;
        const wchar_t* label;
    } freqs[] = {{50, L"50"}, {100, L"100"}, {200, L"200"}, {500, L"500"}, {1000, L"1k"},
                 {2000, L"2k"}, {5000, L"5k"},  {10000, L"10k"}};
    for (const auto& f : freqs) {
        const int x = g.xOf(f.f);
        line(dc, x, p.top, x, p.bottom, kGrid, PS_DOT);
        text(dc, x, p.bottom + px(2), f.label, kDim, TA_CENTER);
    }

    // ---- density: splits, band numbers, and a line per band at its gain now ----
    if (bands > 1) {
        for (int b = 0; b + 1 < bands; ++b) {
            const bool snapped = b == d.snapSplit;
            line(dc, edge(b + 1), p.top, edge(b + 1), p.bottom, snapped ? RGB(225, 205, 255) : kPurple,
                 snapped ? PS_SOLID : PS_DOT, snapped ? std::max(2, px(2)) : 1);
            g.splitX[b] = edge(b + 1);
        }
        g.splits = bands - 1;
    }
    if (bands > 0) {
        if (!big) line(dc, p.left, g.yOfEq(0), p.right, g.yOfEq(0), kGrid);
        for (int b = 0; b < bands; ++b) {
            const int x0 = edge(b) + (b > 0 ? px(2) : 0), x1 = edge(b + 1) - (b + 1 < bands ? px(2) : 0);
            const double gain = std::max(-kDensityRange, std::min(kDensityRange, d.densityDb[b]));
            // on the EQ scale (the window) or the density scale over the whole height (the meter panel)
            const int y = g.yOfEq(gain);
            line(dc, x0, y, x1, y, bandColour(gain), PS_SOLID, std::max(2, px(2.5)));
            if (big && x1 - x0 > px(36)) text(dc, (x0 + x1) / 2, y + (gain >= 0 ? px(3) : -th - px(3)), dbText(gain), kText, TA_CENTER);
            text(dc, (edge(b) + edge(b + 1)) / 2, p.top, std::to_wstring(b + 1), kPurple, TA_CENTER);
        }
    }

    // ---- the density bands' fixed gains: a handle per band on the EQ scale ----
    if (d.bandHandles && bands > 0) {
        for (int b = 0; b < bands; ++b) {
            const int x0 = edge(b) + px(3), x1 = edge(b + 1) - px(3);
            const int y = g.yOfEq(d.bandGainDb[b]);
            const bool hot = b == d.hotBand;
            const COLORREF c = hot ? RGB(225, 205, 255) : kPurple;
            line(dc, x0, y, x1, y, c, PS_SOLID, std::max(2, px(hot ? 3.5 : 2.5)));
            const int gw = std::min(px(hot ? 22 : 16), std::max(px(6), (x1 - x0) / 3)), gh = px(hot ? 9 : 7);
            fill(dc, (x0 + x1) / 2 - gw / 2, y - gh / 2, (x0 + x1) / 2 + gw / 2, y + gh / 2 + 1, c); // the grip
            if (d.bandGainDb[b] != 0.0 && x1 - x0 > px(40))
                text(dc, x1 - px(2), y - th - px(2), dbText(d.bandGainDb[b]), c, TA_RIGHT);
            g.handle[b] = RECT{x0, y - px(2), x1, y + px(2)};
        }
        g.handles = bands;
    }

    // ---- the colour: the sub bass's range, the curve (total and per section), the points ----
    if (d.colour) {
        line(dc, p.left, g.yOfEq(0), p.right, g.yOfEq(0), RGB(110, 100, 60));
        const double subPct = d.values[pSubAmount];
        if (subPct > 0.0) { // an octave under the bass: shaded from half the sub frequency to it
            const int xa = g.xOf(std::max(lo, d.values[pSubFreq] / 2.0)), xb = g.xOf(d.values[pSubFreq]);
            fill(dc, xa, g.yOfEq(0) - px(6), xb, g.yOfEq(0), over(kNodeColour[0], 0.25 + 0.5 * subPct / 100.0));
            text(dc, (xa + xb) / 2, g.yOfEq(0) - px(6) - th, L"sub", kNodeColour[0], TA_CENTER);
        }
        const int n = std::max(64, static_cast<int>(p.right - p.left) / 3);
        std::vector<POINT> total, fixed, section[ColourConfig::kSections];
        for (int i = 0; i <= n; ++i) {
            const int x = p.left + (p.right - p.left) * i / n;
            const double f = g.hzAt(x);
            double sum = 0.0, sumFixed = 0.0;
            for (int sct = 0; sct < ColourConfig::kSections; ++sct) {
                const double one = colourSectionDb(d.values, sct, sct < 4 ? d.autoDb[sct] : 0.0, d.fs, f);
                sum += one;
                sumFixed += sct < 4 ? colourSectionDb(d.values, sct, 0.0, d.fs, f) : one;
                section[sct].push_back({x, g.yOfEq(sct < 4 ? colourSectionDb(d.values, sct, 0.0, d.fs, f) : one)});
            }
            total.push_back({x, g.yOfEq(sum)});
            fixed.push_back({x, g.yOfEq(sumFixed)});
        }
        for (int sct = 0; sct < ColourConfig::kSections; ++sct) {
            const double gain = sct < 4 ? d.values[kNodeGain[sct]] : d.values[pLoudness];
            if (gain != 0.0) polyline(dc, section[sct], sct < 4 ? over(kNodeColour[sct], 0.7) : kLoudColour, 1);
        }
        bool autoMoves = false;
        for (double a : d.autoDb) autoMoves = autoMoves || a != 0.0;
        if (autoMoves) polyline(dc, fixed, over(kCurve, 0.6), 1, PS_DASH); // without the auto tone
        polyline(dc, total, kCurve, std::max(2, px(2.5)));
        g.nodes = true;
        for (int nd = 0; nd < kNodes; ++nd) {
            const int x = g.xOf(d.values[kNodeFreq[nd]]), y = g.yOfEq(d.values[kNodeGain[nd]]);
            g.node[nd] = {x, y};
            const int r = nd == d.hotNode ? px(8) : px(6);
            HBRUSH br = CreateSolidBrush(nd == d.hotNode ? kNodeColour[nd] : kPlot);
            HPEN pen = CreatePen(PS_SOLID, std::max(2, px(2)), kNodeColour[nd]);
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
            Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
            DeleteObject(pen);
        }
    }

    // ---- the spectra, on top ----
    if (d.live) {
        const HRGN clip = CreateRectRgn(p.left, p.top, p.right, p.bottom); // a silent range (the floor) draws nothing
        SelectClipRgn(dc, clip);
        for (int curve = 0; curve < 2; ++curve) {
            const auto& pts = curve == 0 ? d.in : d.out;
            std::vector<POINT> poly(SpectrumAnalyzer::kPoints);
            for (int i = 0; i < SpectrumAnalyzer::kPoints; ++i)
                poly[static_cast<size_t>(i)] = {g.xOf(SpectrumAnalyzer::pointHz(i)), yOfSpec(pts[static_cast<size_t>(i)])};
            polyline(dc, poly, curve == 0 ? kOrange : kTeal, std::max(1, px(1.5)));
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    } else {
        text(dc, (p.left + p.right) / 2, (p.top + p.bottom) / 2 - th / 2, L"no audio", kDim, TA_CENTER);
    }

    // ---- title row ----
    int x = p.left;
    auto legend = [&](const wchar_t* s, COLORREF c) {
        text(dc, x, area.top + px(2), s, c);
        SIZE sz;
        GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
        x += sz.cx + px(12);
    };
    legend(L"Spectrum", kDim);
    legend(L"original", kOrange);
    legend(L"processed", kTeal);
    if (bands > 0) legend(L"density", kPurple);
    if (d.colour) legend(L"colour", kCurve);
    SelectObject(dc, oldFont);
    return g;
}

} // namespace ap::spectrum
