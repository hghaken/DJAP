// SPDX-License-Identifier: GPL-3.0-or-later
#include "meterview.h"

#include "spectrumdraw.h"
#include "tooltips.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ap {

namespace {

const wchar_t* const kClass = L"APMeterView";

// Panel palette (a dark panel in light and dark themes alike, like hardware meters).
const COLORREF kBack = RGB(28, 30, 33), kWell = RGB(44, 47, 52), kText = RGB(214, 218, 224),
               kDim = RGB(120, 126, 134), kGreen = RGB(70, 190, 110), kAmber = RGB(235, 170, 60),
               kRed = RGB(230, 70, 60), kBlue = RGB(80, 160, 235), kOrange = RGB(240, 130, 50),
               kTarget = RGB(250, 220, 90), kWhite = RGB(240, 240, 240), kTeal = RGB(80, 215, 195),
               kGainLine = RGB(40, 140, 255), // the input gain (the blue of the windows' sliders)
               kPurple = RGB(175, 145, 245);
constexpr double kSpecTop = 0.0, kSpecBottom = -90.0; // spectrum scale (dB)

constexpr double kTop = 3.0, kBottom = -40.0; // vertical bar scale (LUFS / dBTP)
constexpr int kShownRows = kMeterRows - 1;     // stage rows drawn: the density row's place went to the spectrum

// Tooltip parts: the four bars, the seven stage rows, the spectrum.
enum TipPart { tInL, tInR, tOutL, tOutR, tRow0, tSpectrum = tRow0 + kMeterRows, kTipParts };
const wchar_t* const kTips[kTipParts] = {
    L"Input, left channel, after the loudness preset's input gain (what enters the processing): its sample peak (dBFS, held, "
    L"falling 20 dB/s) as LEDs - amber from -6 dBFS, red from "
    L"-1 dBFS (near clipping); the single lit LED above holds the highest peak for 1.5 s. White line across In L / R: "
    L"the input's loudness (momentary, 400 ms, LUFS - right scale); yellow line: the loudness preset's target; blue "
    L"line: the loudness preset's input gain (dB - left scale).",
    L"Input, right channel, after the loudness preset's input gain (what enters the processing): its sample peak (dBFS, held, "
    L"falling 20 dB/s) as LEDs - amber from -6 dBFS, red from "
    L"-1 dBFS (near clipping); the single lit LED above holds the highest peak for 1.5 s. White line across In L / R: "
    L"the input's loudness (momentary, 400 ms, LUFS - right scale); yellow line: the loudness preset's target; blue "
    L"line: the loudness preset's input gain (dB - left scale).",
    L"Output, left channel: its true peak (dBTP: peaks between the samples included, falling 20 dB/s) as LEDs - "
    L"amber in the last 3 dB under the limiter's ceiling (red line; its value under the Limiter row), red over it; the single lit LED above holds the "
    L"highest peak for 1.5 s. White line across Out L / R: the output's loudness (momentary, LUFS); yellow: the target.",
    L"Output, right channel: its true peak (dBTP: peaks between the samples included, falling 20 dB/s) as LEDs - "
    L"amber in the last 3 dB under the limiter's ceiling (red line; its value under the Limiter row), red over it; the single lit LED above holds the "
    L"highest peak for 1.5 s. White line across Out L / R: the output's loudness (momentary, LUFS); yellow: the target.",
    L"Leveller gain now: blue to the right = lifted, orange to the left = turned down; the ends of the bar are the "
    L"loudness preset's leveller range. A grey name: the stage is off in the loudness preset, or the deck is "
    L"bypassed.",
    L"Noise reduction now: how far the most turned-down band is down (scale 0-24 dB) - hiss, hum or rumble under the "
    L"loudness preset's noise threshold. Mostly empty while music plays; it fills in pauses and quiet passages.",
    L"Compressor gain reduction now (scale 0-12 dB, a tick every 3 dB).",
    L"",
    L"De-esser gain reduction now (scale 0-12 dB): how much sharp 's' sounds are turned down.",
    L"Soft clipper gain reduction now (scale 0-12 dB): how much the peaks are rounded off.",
    L"Limiter gain reduction now (scale 0-12 dB). Often busy means the input is far louder than the loudness preset expects. "
    L"Under the name: the limiter's ceiling (dBTP), the red line across the Out bars.",
    L"Spectrum of this deck, 20 Hz - 20 kHz, averaged over about 0.3 s: orange = the original (the deck's input), "
    L"turquoise = processed (the output). The scale is dB of a sine's level (0 dB = full scale). Where the turquoise "
    L"line lies above the orange one the processing lifts that range. Purple: the density stage's bands (dashed: "
    L"their splits, with the band numbers) and a line per band at its gain now, +-12 dB over the height - blue = "
    L"lifted, red = turned down.",
};

void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int style = PS_SOLID, int width = 1) {
    HPEN pen = CreatePen(style, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

struct View {
    MeterViewData d;
    HFONT font = nullptr, small = nullptr;
    UINT fontDpi = 0;
    Tooltips tips;
    RECT tipRect[kTipParts] = {};
    double hold[4] = {kFloorDb, kFloorDb, kFloorDb, kFloorDb}; // peak-hold LED of each vertical bar
    ULONGLONG holdAt[4] = {};                                  // when it was set (GetTickCount64)
};

constexpr ULONGLONG kHoldMs = 1500; // the peak-hold LED stays this long, then follows the bar down

// A colour darkened towards the well: an unlit LED of that colour.
COLORREF unlit(COLORREF c) {
    auto mix = [](int a, int b) { return b + (a - b) * 28 / 100; };
    return RGB(mix(c & 0xFF, 44), mix((c >> 8) & 0xFF, 47), mix((c >> 16) & 0xFF, 52)); // towards kWell (44, 47, 52)
}

// Moves a tooltip part to its place in this paint (only when it moved).
void placeTip(HWND hwnd, View& v, int part, int l, int t, int r, int b) {
    const RECT rc{l, t, r, b};
    if (EqualRect(&rc, &v.tipRect[part])) return;
    v.tipRect[part] = rc;
    v.tips.setRect(hwnd, static_cast<UINT_PTR>(part), rc);
}

std::wstring w(const char* s) {
    std::wstring out;
    while (*s) out += static_cast<wchar_t>(static_cast<unsigned char>(*s++));
    return out;
}

std::string level(double v) {
    if (v <= kFloorDb + 1.0) return "--";
    char b[16];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    if (r <= l || b <= t) return;
    RECT rc{l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void text(HDC dc, int x, int y, const std::string& s, COLORREF c, UINT align = TA_LEFT) {
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    const std::wstring ws = w(s.c_str());
    TextOutW(dc, x, y, ws.c_str(), static_cast<int>(ws.size()));
}

void ensureFonts(HWND hwnd, View& v) {
    const UINT dpi = GetDpiForWindow(hwnd);
    if (v.font && v.fontDpi == dpi) return;
    if (v.font) DeleteObject(v.font);
    if (v.small) DeleteObject(v.small);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    v.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 85 / 100;
    v.small = CreateFontIndirectW(&lf);
    v.fontDpi = dpi;
}

void paint(HWND hwnd, HDC target, View& v) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    ensureFonts(hwnd, v);
    HGDIOBJ oldFont = SelectObject(dc, v.font);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, W, H, kBack);

    const double s = GetDpiForWindow(hwnd) / 96.0;
    auto px = [s](double x) { return static_cast<int>(x * s + 0.5); };
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight;
    const MeterViewData& d = v.d;
    const DeckMeters& m = d.m;
    const bool live = d.audio && m.live;

    const int margin = px(10), bottomH = margin - px(4);
    // ---- vertical LED bars (client, 2026-10-25: "only IN L/R and OUT L/R, with a dB and a LUFS scale"): the peak
    // of each channel - input sample peak (dBFS), output true peak (dBTP) - with a peak-hold LED; the loudness
    // (momentary LUFS of both channels together) as a white line across each pair and the target in yellow. One axis,
    // -40 .. +3: dB on the left, LUFS on the right ----
    const int scaleW = px(30), barW = px(16), gap = px(4), groupGap = px(14);
    const int top = margin + 2 * th + px(4), bottom = H - margin;
    const int barsLeft = margin + scaleW;
    auto barX = [&](int i) { return barsLeft + i * (barW + gap) + (i >= 2 ? groupGap - gap : 0); };
    const int barsRight = barX(3) + barW;
    auto yOf = [&](double db) {
        const double t = (std::min(kTop, std::max(kBottom, db)) - kBottom) / (kTop - kBottom);
        return bottom - static_cast<int>(t * (bottom - top) + 0.5);
    };
    if (bottom - top > px(40)) {
        SelectObject(dc, v.small);
        text(dc, barsLeft - px(6), margin + th, "dB", kDim, TA_RIGHT);
        text(dc, barsRight + px(6), margin + th, "LUFS", kDim);
        for (int db = 0; db >= -36; db -= 6) {
            const int y = yOf(db);
            fill(dc, barsLeft - px(4), y, barsLeft - px(1), y + 1, kDim);
            text(dc, barsLeft - px(6), y - th / 2, std::to_string(db), kDim, TA_RIGHT);
            fill(dc, barsRight + px(1), y, barsRight + px(4), y + 1, kDim);
            text(dc, barsRight + px(6), y - th / 2, std::to_string(db), kDim);
        }
        SelectObject(dc, v.font);
        text(dc, (barX(0) + barX(1) + barW) / 2, margin, "In", kText, TA_CENTER);
        text(dc, (barX(2) + barX(3) + barW) / 2, margin, "Out", kText, TA_CENTER);
        SelectObject(dc, v.small);
        // In: the input after the loudness preset's input gain - what enters the processing (client, 2026-10-25: the input
        // slider should move the In bars); the gain is a fixed factor, added here, not in the measurement
        auto afterGain = [&](double db) { return db > kFloorDb + 1.0 ? db + d.inputGain : kFloorDb; };
        const double values[4] = {afterGain(m.inPeak[0]), afterGain(m.inPeak[1]), m.tpNow[0], m.tpNow[1]};
        // LED bars (client, 2026-10-25: "make LED bars of the bars"): segments from the bottom, lit up to the value,
        // the rest dark in their zone's colour, and a peak-hold LED for kHoldMs
        const int pitch = std::max(3, px(5)), ledGap = std::max(1, px(1.2));
        auto dbAt = [&](int y) { return kBottom + (kTop - kBottom) * (bottom - y) / static_cast<double>(bottom - top); };
        const ULONGLONG now = GetTickCount64();
        for (int i = 0; i < 4; ++i) {
            const int x = barX(i);
            placeTip(hwnd, v, tInL + i, x - gap / 2, margin, x + barW + gap / 2, bottom);
            fill(dc, x, top, x + barW, bottom, kBack);
            text(dc, x + barW / 2, margin + th, i % 2 == 0 ? "L" : "R", kDim, TA_CENTER);
            const double val = live ? values[i] : kFloorDb;
            // input: green, amber from -6 dBFS, red from -1 dBFS (near clipping); output: green, amber in the last
            // 3 dB under the ceiling, red over it
            auto zone = [&](double db) {
                if (i < 2) return db > -1.0 ? kRed : db > -6.0 ? kAmber : kGreen;
                return db > d.ceiling ? kRed : db > d.ceiling - 3.0 ? kAmber : kGreen;
            };
            if (val >= v.hold[i] || now - v.holdAt[i] > kHoldMs) {
                if (val >= v.hold[i]) v.holdAt[i] = now;
                v.hold[i] = val;
            }
            const double held = live ? v.hold[i] : kFloorDb;
            int heldLed = -1; // the segment that holds the peak (above the lit ones)
            for (int k = 0, yb = bottom; yb - pitch + ledGap >= top; ++k, yb -= pitch) {
                const int yt = yb - pitch + ledGap;
                const double mid = dbAt((yt + yb) / 2);
                if (held > kBottom && held >= dbAt(yb) && held < dbAt(yt - ledGap)) heldLed = k;
                const bool lit = val > kBottom && val >= mid;
                fill(dc, x, yt, x + barW, yb, lit || k == heldLed ? zone(mid) : unlit(zone(mid)));
            }
        }
        // across Out L / R: the limiter's ceiling (red; 2 px across the pair as the other lines - client, 2026-10-30:
        // "at the output it is not quite clear where the limiter is set in dB"; its value is under the Limiter row)
        {
            const int yc = yOf(d.ceiling);
            fill(dc, barX(2) - px(2), yc, barX(3) + barW + px(2), yc + px(2), kRed);
        }
        // across In L / R: the input gain on the dB scale (blue; client, 2026-10-30: "a line at the In meter for the
        // input gain that is set") - where the input's 0 dBFS lands after it
        {
            const int yg = yOf(d.inputGain);
            fill(dc, barX(0) - px(2), yg, barX(1) + barW + px(2), yg + px(2), kGainLine);
        }
        // across each pair: the target (yellow) and the loudness now (white, momentary LUFS)
        for (int pair = 0; pair < 2; ++pair) {
            const int xl = barX(2 * pair) - px(2), xr = barX(2 * pair + 1) + barW + px(2);
            const int yt = yOf(d.target);
            fill(dc, xl, yt, xr, yt + px(2), kTarget);
            const double lufs = pair == 0 ? afterGain(m.inM) : m.outM;
            if (live && lufs > kBottom) {
                const int yl = yOf(lufs);
                fill(dc, xl, yl - px(1), xr, yl + px(2), kWhite);
            }
        }
    }

    // ---- gain of each stage ----
    const int gx = barsRight + scaleW + px(8);
    const int nameW = px(78);
    // every bar from barL to the right margin (as the spectrum)
    const int barL = gx + nameW, fullR = W - margin;
    struct Row {
        const char* name;
        int stage;
        int kind; // 0 = reduction 0..full dB, 1 = leveller +-range, 2 = density bands
        double value;
        int full = 12; // a reduction row's scale
    };
    const Row rows[kMeterRows] = {{"Leveller", sLeveller, 1, m.levellerDb},
                                  {"Noise", sNoise, 0, m.noiseMaxDb(), 24}, // its depth goes to 40 dB
                                  {"Compressor", sCompressor, 0, m.compressorDb},
                                  {"Density", sDensity, 2, 0.0},
                                  {"De-esser", sDeEsser, 0, m.deEsserDb},
                                  {"Clipper", sClipper, 0, m.clipperDb},
                                  {"Limiter", sLimiter, 0, m.limiterDb}};
    const int rowsTop = margin, rowsBottom = H - bottomH - px(4);
    // each row: the bar, its scale under it (small font); the spectrum takes the rest of the height
    SelectObject(dc, v.small);
    TEXTMETRICW stmRows;
    GetTextMetricsW(dc, &stmRows);
    const int sth = stmRows.tmHeight;
    SelectObject(dc, v.font);
    const int rowH = std::min(px(16) + sth + px(8), (rowsBottom - rowsTop) / kShownRows);
    auto hz = [](double f) {
        char b[16];
        if (f < 1000.0) std::snprintf(b, sizeof b, "%.0f", f);
        else if (std::fmod(f, 1000.0) == 0.0) std::snprintf(b, sizeof b, "%.0fk", f / 1000.0);
        else std::snprintf(b, sizeof b, "%.1fk", f / 1000.0);
        return std::string(b);
    };
    if (fullR - barL > px(40) && rowH > th) {
        for (int i = 0, shown = 0; i < kMeterRows; ++i) {
            if (rows[i].kind == 2) continue; // density: its bands are drawn in the spectrum
            const Row& r = rows[i];
            const int barR = fullR;
            const int y0 = rowsTop + shown++ * rowH, bt = y0 + px(3), bb = bt + px(12), yc = (bt + bb) / 2;
            const int ly = bb + px(2); // scale labels
            placeTip(hwnd, v, tRow0 + i, gx, y0, W, y0 + rowH);
            const bool on = d.stageOn[r.stage] && d.processing;
            text(dc, gx, yc - th / 2, r.name, on ? kText : kDim);
            fill(dc, barL, bt, barR, bb, kWell);
            SelectObject(dc, v.small);
            if (r.stage == sLimiter) { // its ceiling, under the name (the red line on the Out bars)
                char c[24];
                std::snprintf(c, sizeof c, "%.1f dBTP", d.ceiling);
                text(dc, gx, ly, c, on ? kRed : kDim);
            }
            if (r.kind == 0) { // reduction 0 .. full dB, a tick every quarter
                const double full = r.full;
                const double red = live && on ? std::min(full, std::max(0.0, r.value)) : 0.0;
                fill(dc, barL, bt, barL + static_cast<int>((barR - barL) * red / full), bb, kOrange);
                text(dc, barL, ly, "0", kDim);
                for (int t = r.full / 4; t < r.full; t += r.full / 4) {
                    const int x = barL + (barR - barL) * t / r.full;
                    fill(dc, x, bb, x + 1, bb + px(2), kDim);
                    text(dc, x, ly, "-" + std::to_string(t), kDim, TA_CENTER);
                }
                text(dc, barR, ly, "-" + std::to_string(r.full) + " dB", kDim, TA_RIGHT);
            } else if (r.kind == 1) { // leveller -range .. +range
                const double range = std::max(1.0, d.levRange);
                const int mid = (barL + barR) / 2;
                const double g = live && on ? std::max(-range, std::min(range, r.value)) : 0.0;
                const int x = mid + static_cast<int>((barR - barL) / 2 * g / range);
                if (g > 0) fill(dc, mid, bt, x, bb, kBlue);
                else fill(dc, x, bt, mid, bb, kOrange);
                fill(dc, mid, bt - px(2), mid + 1, bb + px(2), kDim);
                char b[16];
                std::snprintf(b, sizeof b, "-%g", range);
                text(dc, barL, ly, b, kDim);
                for (int q = 1; q <= 3; ++q) {
                    const int xq = barL + (barR - barL) * q / 4;
                    if (q != 2) fill(dc, xq, bb, xq + 1, bb + px(2), kDim);
                    std::snprintf(b, sizeof b, q == 2 ? "0" : q == 1 ? "-%g" : "+%g", range / 2);
                    text(dc, xq, ly, b, kDim, TA_CENTER);
                }
                std::snprintf(b, sizeof b, "+%g dB", range);
                text(dc, barR, ly, b, kDim, TA_RIGHT);
            } else { // density: each band up / down +-12 dB; under it the band's frequencies
                const int n = std::max(1, std::min(kMaxBands, m.bands > 0 ? m.bands : 1));
                const int halfH = (bb - bt) / 2;
                fill(dc, barL, bt, barR, bb, kBack);
                for (int b = 0; b < n; ++b) { // the outer blocks end on the bars' edges, a gap between blocks
                    const int x0 = barL + (barR - barL) * b / n + (b > 0 ? px(2) : 0);
                    const int x1 = barL + (barR - barL) * (b + 1) / n - (b + 1 < n ? px(2) : 0);
                    fill(dc, x0, bt, x1, bb, kWell);
                    const double g = live && on ? std::max(-12.0, std::min(12.0, m.densityDb[b])) : 0.0;
                    const int h = static_cast<int>(halfH * std::fabs(g) / 12.0);
                    if (g > 0) fill(dc, x0, yc - h, x1, yc, kBlue);
                    else fill(dc, x0, yc, x1, yc + h, kOrange);
                    fill(dc, x0, yc, x1, yc + 1, kDim);
                    const std::string band = n == 1 ? "all, +-12 dB"
                                             : b == 0 ? "< " + hz(d.splits[0]) + " Hz"
                                             : b + 1 == n ? "> " + hz(d.splits[b - 1])
                                                          : hz(d.splits[b - 1]) + "-" + hz(d.splits[b]);
                    text(dc, (x0 + x1) / 2, ly, band, kDim, TA_CENTER);
                }
            }
            SelectObject(dc, v.font);
        }
    }

    // ---- spectrum under the stage rows: original and processed, the density bands and their gains ----
    {
        const int sx0 = gx, sx1 = W - margin;
        const int sy0 = rowsTop + kShownRows * rowH + px(6), sy1 = rowsBottom - px(4);
        if (sx1 - sx0 > px(120) && sy1 - sy0 > px(80)) {
            placeTip(hwnd, v, tSpectrum, sx0, sy0, sx1, sy1);
            spectrum::Data sd;
            sd.in = d.specIn;
            sd.out = d.specOut;
            sd.live = d.spectrum;
            sd.densityOn = d.stageOn[sDensity] && d.processing;
            sd.bands = m.bands > 0 ? m.bands : 1;
            for (int k = 0; k < kMaxSplits; ++k) sd.splits[k] = d.splits[k];
            for (int k = 0; k < kMaxBands; ++k) sd.densityDb[k] = live ? m.densityDb[k] : 0.0;
            sd.colour = d.processing; // bypassed: no colour is applied
            sd.values = d.colourValues;
            for (int k = 0; k < 4; ++k) sd.autoDb[k] = live ? d.autoDb[k] : 0.0;
            sd.fs = d.fs;
            spectrum::paint(dc, RECT{sx0, sy0, sx1, sy1}, sd, v.small, GetDpiForWindow(hwnd), false);
        }
    }

    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// What a click at pt opens (meterStage's numbering): a stage row 0..6, the In bars 7, the Out bars 8, the spectrum 9;
// -1 = nothing.
// The parts as last painted (their tooltip rectangles).
int clickTarget(const View& v, POINT pt) {
    for (int i = 0; i < kMeterRows; ++i)
        if (PtInRect(&v.tipRect[tRow0 + i], pt)) return i;
    for (int b = tInL; b <= tOutR; ++b)
        if (PtInRect(&v.tipRect[b], pt)) return b <= tInR ? kMeterRows : kMeterRows + 1;
    if (PtInRect(&v.tipRect[tSpectrum], pt)) return kMeterSpectrum;
    return -1;
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    View* v = reinterpret_cast<View*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new View()));
        break;
    case WM_CREATE:
        if (v) {
            v->tips.create(hwnd);
            for (int i = 0; i < kTipParts; ++i) {
                const std::wstring tip = std::wstring(kTips[i]) +
                                         (i >= tRow0 && i < tRow0 + kMeterRows ? L" Click: opens its settings."
                                          : i <= tInR ? L" Click: opens the General tab (input gain, target)."
                                          : i <= tOutR ? L" Click: opens the Limiter tab (its ceiling)."
                                                       : L" Click: opens the big spectrum window, with the colour's EQ "
                                                         L"curve and the bass settings.");
                v->tips.addRect(hwnd, static_cast<UINT_PTR>(i), tip.c_str());
            }
        }
        return 0;
    case WM_SETCURSOR: // a hand over the stage rows and the LED bars: a click opens their settings
        if (v && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (clickTarget(*v, pt) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (v) {
            const int target = clickTarget(*v, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
            if (target >= 0)
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), kMeterOpenRow + target),
                             reinterpret_cast<LPARAM>(hwnd));
        }
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        if (v) v->tips.setDpi(GetDpiForWindow(hwnd));
        return 0;
    case WM_NCDESTROY:
        if (v) {
            if (v->font) DeleteObject(v->font);
            if (v->small) DeleteObject(v->small);
            delete v;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (v) paint(hwnd, dc, *v);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int meterViewMinHeight(UINT dpi) {
    // paint()'s layout: the six stage rows at their full height, then a spectrum whose dB labels (every 12 dB of its
    // 96) do not touch each other, its title row above and the frequencies below
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 85 / 100; // the small font
    HFONT small = CreateFontIndirectW(&lf);
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, small);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    DeleteObject(small);
    const double s = dpi / 96.0;
    auto px = [s](double x) { return static_cast<int>(x * s + 0.5); };
    const int sth = tm.tmHeight;
    const int margin = px(10), bottomH = margin - px(4);
    const int row = px(16) + sth + px(8);
    const int plot = (sth + px(2)) * 96 / 12 + 1; // spectrumdraw.cpp: 0 .. -96 dB
    const int spectrum = sth + px(4) + plot + sth + px(4);
    return margin + kShownRows * row + px(6) + spectrum + px(4) + bottomH + px(4);
}

const char* meterStage(int target) {
    static const char* const tabs[kMeterTargets] = {"Leveller", "Noise",   "Compressor", "Density", "De-esser",
                                                    "Soft clipper", "Limiter", "General", "Limiter", "Density"};
    return target >= 0 && target < kMeterTargets ? tabs[target] : "General";
}

static HINSTANCE g_instance = nullptr; // the module the class is registered for (the program, or the plug-in DLL)

void registerMeterView(HINSTANCE instance) {
    g_instance = instance;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
}

HWND createMeterView(HWND parent, int id) {
    return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

void unregisterMeterView() {
    if (g_instance) UnregisterClassW(kClass, g_instance);
    g_instance = nullptr;
}

void setMeterView(HWND view, const MeterViewData& data) {
    if (View* v = reinterpret_cast<View*>(GetWindowLongPtrW(view, GWLP_USERDATA))) {
        v->d = data;
        InvalidateRect(view, nullptr, FALSE);
    }
}

} // namespace ap
