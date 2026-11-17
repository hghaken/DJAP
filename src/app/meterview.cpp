// SPDX-License-Identifier: GPL-3.0-or-later
#include "meterview.h"

#include "settings.h"
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

// Tooltip parts: the four bars, the seven stage rows, the spectrum, the Volume faders (with their Link box), Mono.
enum TipPart { tInL, tInR, tOutL, tOutR, tRow0, tSpectrum = tRow0 + kMeterRows, tGain, tMono, kTipParts };
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
    L"line lies above the orange one the processing lifts that range. Purple: the MB Compressor's bands (dashed: "
    L"their splits, with the band numbers) and a line per band at its gain now, +-12 dB over the height - blue = "
    L"lifted, red = turned down.",
    L"Volume: the deck's volume after the limiter, the left (L) and the right (R) channel - from 0 dB at the top (the "
    L"most: only down, so the ceiling holds) to off at the bottom (no audio); fine near the top, coarser below, as a "
    L"mixer's fader. The Out bars show the result. Drag a fader, click in its slot, or turn the mouse wheel over it "
    L"(0.5 dB a step near the top); double-click: back to 0 dB. Link on: the two move together, as two faders held "
    L"by one hand (a difference between them stays until one reaches an end); off: each on its own. The deck's own "
    L"setting - another sound leaves it as it is. Grey: the deck is bypassed, and nothing is applied.",
    L"Mono: this deck's left and right are mixed, (L + R) / 2, and both of its outputs carry that one sound - for a "
    L"mono sound system, or a speaker on one side only. From the deck's input on, so the leveller keeps the loudness "
    L"on target (a mono mix of wide music is a little quieter) and the limiter its ceiling; a switch glides in over "
    L"20 ms. Off (stereo) by default; Bypass stays the unchanged input. The deck's own setting.",
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
    // the Volume faders as last painted: each one's column (empty: not drawn) and cap, the Link and Mono boxes, the
    // y of the range's top (0 dB) and bottom - a cap's centre; a drag's state
    RECT fader[2] = {}, cap[2] = {}, link = {}, monoBox = {};
    int gainTop = 0, gainBottom = 1;
    int dragging = -1;          // the fader being dragged (0 L, 1 R), -1 none
    int grab = 0;               // the cap's centre minus the mouse's y where the drag began (0: the slot was clicked)
    double start[2] = {};       // both values where the drag began
    double gain[2] = {};        // the values of the last message to the parent (drawn while dragging)
    int wheel = 0;              // the mouse wheel's part of a step not yet used
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

// A fader cap of brushed metal (as the window's background): light at the top, darker below, its channel's letter
// above and grip ridges under the dark centre line that the scale is read at; rounded, with a dark frame and a shadow.
void paintCap(HDC dc, const RECT& c, bool on, double s, const char* letter, int th) {
    auto px = [s](double x) { return static_cast<int>(x * s + 0.5); };
    fill(dc, c.left + px(1), c.bottom, c.right + px(1), c.bottom + px(2), RGB(12, 13, 15)); // shadow
    const int round = std::max(4, px(6));
    HRGN rgn = CreateRoundRectRgn(c.left, c.top, c.right + 1, c.bottom + 1, round, round);
    SelectClipRgn(dc, rgn);
    const int h = std::max(1, static_cast<int>(c.bottom - c.top));
    for (int y = c.top; y < c.bottom; ++y) {
        const double f = static_cast<double>(y - c.top) / h;
        const int g = on ? static_cast<int>(236 - 86 * f) : static_cast<int>(122 - 40 * f);
        fill(dc, c.left, y, c.right, y + 1, RGB(g, g + 2, g + 5));
    }
    fill(dc, c.left, c.top, c.right, c.top + 1, on ? RGB(252, 252, 253) : RGB(140, 142, 146)); // the lit top edge
    const int cy = (c.top + c.bottom) / 2, pitch = std::max(3, px(4)), inset = std::max(3, px(4));
    text(dc, (c.left + c.right) / 2, (c.top + cy) / 2 - th / 2, letter, on ? RGB(38, 40, 44) : RGB(54, 56, 60), TA_CENTER);
    for (int k = 1; k <= 3; ++k) {
        const int y = cy + k * pitch;
        fill(dc, c.left + inset, y, c.right - inset, y + 1, on ? RGB(112, 116, 122) : RGB(70, 72, 76));
        fill(dc, c.left + inset, y + 1, c.right - inset, y + 2, on ? RGB(246, 247, 249) : RGB(130, 132, 136));
    }
    fill(dc, c.left, cy - std::max(1, px(1)), c.right, cy + std::max(1, px(1)), on ? RGB(24, 25, 27) : RGB(58, 60, 64));
    SelectClipRgn(dc, nullptr);
    HBRUSH frame = CreateSolidBrush(RGB(10, 11, 12));
    FrameRgn(dc, rgn, frame, 1, 1);
    DeleteObject(frame);
    DeleteObject(rgn);
}

// The Volume faders in the column l .. l + w: the name over both and each one's value in the two header rows, two dark
// slots with a tick at each point of the scale (the labels right of the R one, "off" at the bottom), the caps at the
// values - joined by a blue line while they are linked - and under them the Link box and the Mono box (client,
// 2026-11-17: "take the Mono check box out of the Setup and put it under the Link check box").
void paintFaders(HWND hwnd, HDC dc, View& v, int l, int w, int margin, int top, int bottom, int th, double s) {
    auto px = [s](double x) { return static_cast<int>(x * s + 0.5); };
    const MeterViewData& d = v.d;
    const bool on = d.processing;
    const int capW = px(24), capH = px(34), cx[2] = {l + px(15), l + px(45)};
    // the rows of the Link and Mono boxes under the slots
    SelectObject(dc, v.small);
    TEXTMETRICW stm;
    GetTextMetricsW(dc, &stm);
    const int sth = stm.tmHeight, box = std::max(9, px(11)), rowH = sth + px(5);
    const int slotBottom = bottom - 2 * rowH - px(4);
    v.gainTop = top + capH / 2;
    v.gainBottom = std::max(v.gainTop + 1, slotBottom - capH / 2);
    auto yOf = [&](double db) {
        return v.gainTop + static_cast<int>((1.0 - volumePosition(db)) * (v.gainBottom - v.gainTop) + 0.5);
    };
    const double value[2] = {v.dragging >= 0 ? v.gain[0] : d.gainL, v.dragging >= 0 ? v.gain[1] : d.gainR};
    SelectObject(dc, v.font);
    text(dc, (cx[0] + cx[1]) / 2, margin, "Volume", on ? kText : kDim, TA_CENTER);
    SelectObject(dc, v.small);
    char b[24];
    const int slotW = std::max(4, px(6));
    for (int k = 0; k < 2; ++k) {
        if (volumeOff(value[k])) std::snprintf(b, sizeof b, "off");
        else std::snprintf(b, sizeof b, "%.1f", value[k]);
        text(dc, cx[k], margin + th, b, on ? kText : kDim, TA_CENTER);
        const int sl = cx[k] - slotW / 2;
        fill(dc, sl, top, sl + slotW, slotBottom, RGB(8, 9, 10));
        fill(dc, sl + slotW - 1, top, sl + slotW, slotBottom, RGB(66, 70, 76)); // the lit right edge
    }
    for (int i = 0; i <= kVolumeLawPoints; ++i) { // a tick at each point of the scale beside each slot; off at the bottom
        const double db = i < kVolumeLawPoints ? kVolumeLaw[i].db : kVolumeOffDb;
        const int y = yOf(db);
        fill(dc, cx[0] - capW / 2 + px(2), y, cx[0] - slotW / 2 - px(2), y + 1, kDim);
        fill(dc, cx[0] + slotW / 2 + px(3), y, cx[1] - slotW / 2 - px(2), y + 1, kDim);
        fill(dc, cx[1] + slotW / 2 + px(3), y, cx[1] + capW / 2 - px(2), y + 1, kDim);
        if (i < kVolumeLawPoints) std::snprintf(b, sizeof b, "%g", db);
        else std::snprintf(b, sizeof b, "off");
        text(dc, cx[1] + capW / 2 + px(3), y - sth / 2, b, db == 0.0 ? kText : kDim);
    }
    const char* const letters[2] = {"L", "R"};
    for (int k = 0; k < 2; ++k) {
        const int yc = yOf(value[k]);
        v.cap[k] = RECT{cx[k] - capW / 2, yc - capH / 2, cx[k] - capW / 2 + capW, yc - capH / 2 + capH};
    }
    if (d.gainLink) { // the two caps' centre lines joined
        HPEN pen = CreatePen(PS_SOLID, std::max(2, px(2)), on ? kGainLine : kDim);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, v.cap[0].right, (v.cap[0].top + v.cap[0].bottom) / 2, nullptr);
        LineTo(dc, v.cap[1].left, (v.cap[1].top + v.cap[1].bottom) / 2);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    for (int k = 0; k < 2; ++k) paintCap(dc, v.cap[k], on, s, letters[k], sth);
    // the Link and Mono boxes, one under the other: check boxes of the panel's own, blue with a white tick when on
    SIZE ts[2] = {};
    GetTextExtentPoint32W(dc, L"Link", 4, &ts[0]);
    GetTextExtentPoint32W(dc, L"Mono", 4, &ts[1]);
    const int lx = (cx[0] + cx[1]) / 2 - (box + px(5) + std::max(ts[0].cx, ts[1].cx)) / 2;
    auto checkBox = [&](int ty, bool checked, const char* name, LONG textW) {
        const int ly = ty + (sth - box) / 2;
        fill(dc, lx, ly, lx + box, ly + box, kDim);
        fill(dc, lx + 1, ly + 1, lx + box - 1, ly + box - 1, checked ? (on ? kGainLine : kDim) : RGB(16, 17, 19));
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, px(1.6)), kWhite);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, lx + box * 22 / 100, ly + box * 52 / 100, nullptr);
            LineTo(dc, lx + box * 42 / 100, ly + box * 72 / 100);
            LineTo(dc, lx + box * 78 / 100, ly + box * 28 / 100);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        text(dc, lx + box + px(5), ty, name, on ? kText : kDim);
        return RECT{lx - px(3), ty - px(2), lx + box + px(5) + textW + px(3), ty + sth + px(2)};
    };
    const int linkY = slotBottom + px(6), monoY = linkY + rowH;
    v.link = checkBox(linkY, d.gainLink, "Link", ts[0].cx);
    v.monoBox = checkBox(monoY, d.mono, "Mono", ts[1].cx);
    const int mid = (cx[0] + cx[1]) / 2;
    v.fader[0] = RECT{l, top - px(4), mid, slotBottom + px(2)};
    v.fader[1] = RECT{mid, top - px(4), l + w, slotBottom + px(2)};
    placeTip(hwnd, v, tGain, l, margin, l + w, monoY - px(1));
    placeTip(hwnd, v, tMono, l, monoY - px(1), l + w, bottom);
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

    // ---- the Volume faders beside the Out bars (their LUFS scale between them) ----
    const int faderL = barsRight + scaleW + px(4), faderW = px(82);
    const bool fader = d.gainFaders && bottom - top > px(140);
    if (fader) {
        paintFaders(hwnd, dc, v, faderL, faderW, margin, top, bottom, th, s);
        SelectObject(dc, v.font);
    } else if (v.fader[0].right > v.fader[0].left) {
        v.fader[0] = v.fader[1] = v.link = v.monoBox = RECT{};
        placeTip(hwnd, v, tGain, 0, 0, 0, 0);
        placeTip(hwnd, v, tMono, 0, 0, 0, 0);
    }

    // ---- gain of each stage ----
    const int gx = fader ? faderL + faderW + px(10) : barsRight + scaleW + px(8);
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
                                  {"MB Compressor", sDensity, 2, 0.0},
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

// The Volume faders: which one pt is on (0 L, 1 R, -1 none), the value at a cap centre's y (in steps), both values
// when fader k goes to db (linked: the other's cap moves as far along its slot, each held at the ends - as two faders
// moved by one hand), and values told to the parent - which keeps them and sends them back with the next
// setMeterView; until then these are drawn.
int faderAt(const View& v, POINT pt) {
    if (!v.d.gainFaders) return -1;
    for (int k = 0; k < 2; ++k)
        if (PtInRect(&v.fader[k], pt)) return k;
    return -1;
}

bool onLink(const View& v, POINT pt) { return v.d.gainFaders && PtInRect(&v.link, pt); }
bool onMono(const View& v, POINT pt) { return v.d.gainFaders && PtInRect(&v.monoBox, pt); }

double gainAt(const View& v, int y) {
    const double t = static_cast<double>(y - v.gainTop) / std::max(1, v.gainBottom - v.gainTop);
    return volumeAtPosition(1.0 - std::min(1.0, std::max(0.0, t)));
}

void moved(const View& v, const double from[2], int k, double db, double out[2]) {
    if (v.d.gainLink) return linkedVolumes(from, k, db, out);
    out[0] = from[0];
    out[1] = from[1];
    out[k] = clampVolume(db);
}

void tellGain(HWND hwnd, View& v, const double g[2], int code) {
    v.gain[0] = v.d.gainL = g[0];
    v.gain[1] = v.d.gainR = g[1];
    InvalidateRect(hwnd, nullptr, FALSE);
    SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), code), reinterpret_cast<LPARAM>(hwnd));
}

void endDrag(HWND hwnd, View& v) {
    v.dragging = -1;
    if (GetCapture() == hwnd) ReleaseCapture(); // (its WM_CAPTURECHANGED finds no drag)
    const double g[2] = {v.gain[0], v.gain[1]};
    tellGain(hwnd, v, g, kMeterGainSet); // kept: saved
}

// A press of the left button: on a fader a drag begins - from the cap where it was taken, or with the cap jumped to a
// click in the slot; on the Link or the Mono box it switches; elsewhere what the place opens.
void press(HWND hwnd, View& v, POINT pt) {
    const int k = faderAt(v, pt);
    if (k >= 0) {
        v.grab = PtInRect(&v.cap[k], pt) ? (v.cap[k].top + v.cap[k].bottom) / 2 - pt.y : 0;
        v.start[0] = v.gain[0] = v.d.gainL;
        v.start[1] = v.gain[1] = v.d.gainR;
        v.dragging = k;
        SetCapture(hwnd);
        double g[2];
        moved(v, v.start, k, gainAt(v, pt.y + v.grab), g);
        if (g[0] != v.gain[0] || g[1] != v.gain[1]) tellGain(hwnd, v, g, kMeterGainMoved);
        return;
    }
    if (onLink(v, pt)) {
        v.d.gainLink = !v.d.gainLink;
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), kMeterGainLink),
                     reinterpret_cast<LPARAM>(hwnd));
        return;
    }
    if (onMono(v, pt)) {
        v.d.mono = !v.d.mono;
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), kMeterMono),
                     reinterpret_cast<LPARAM>(hwnd));
        return;
    }
    const int target = clickTarget(v, pt);
    if (target >= 0)
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), kMeterOpenRow + target),
                     reinterpret_cast<LPARAM>(hwnd));
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
                                          : i == tGain || i == tMono ? L""
                                                       : L" Click: opens the big spectrum window, with the EQ "
                                                         L"curve and the bass settings.");
                v->tips.addRect(hwnd, static_cast<UINT_PTR>(i), tip.c_str());
            }
        }
        return 0;
    case WM_SETCURSOR: // a hand over the stage rows, the LED bars, the Link and Mono boxes; up / down on the faders
        if (v && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (v->dragging >= 0 || faderAt(*v, pt) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            if (onLink(*v, pt) || onMono(*v, pt) || clickTarget(*v, pt) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (v) press(hwnd, *v, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
        return 0;
    case WM_LBUTTONDBLCLK: // on a fader: 0 dB (linked: both); elsewhere a second press
        if (v) {
            const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            const int k = faderAt(*v, pt);
            if (k >= 0) {
                if (v->dragging >= 0) {
                    v->dragging = -1;
                    if (GetCapture() == hwnd) ReleaseCapture();
                }
                double g[2] = {v->d.gainL, v->d.gainR};
                if (v->d.gainLink) g[0] = g[1] = 0.0;
                else g[k] = 0.0;
                tellGain(hwnd, *v, g, kMeterGainSet);
            } else {
                press(hwnd, *v, pt);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (v && v->dragging >= 0) {
            double g[2];
            moved(*v, v->start, v->dragging, gainAt(*v, static_cast<short>(HIWORD(lp)) + v->grab), g);
            if (g[0] != v->gain[0] || g[1] != v->gain[1]) tellGain(hwnd, *v, g, kMeterGainMoved);
        }
        return 0;
    case WM_LBUTTONUP:
        if (v && v->dragging >= 0) endDrag(hwnd, *v);
        return 0;
    case WM_CAPTURECHANGED: // taken away during a drag (another window, Alt+Tab): the values so far are kept
        if (v && v->dragging >= 0) endDrag(hwnd, *v);
        return 0;
    case WM_MOUSEWHEEL: // over a fader: a step per notch (linked: both); elsewhere to the parent
        if (v) {
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            ScreenToClient(hwnd, &pt);
            const int k = faderAt(*v, pt);
            if (k >= 0 && v->dragging < 0) {
                v->wheel += GET_WHEEL_DELTA_WPARAM(wp);
                const int steps = v->wheel / WHEEL_DELTA;
                v->wheel -= steps * WHEEL_DELTA;
                if (steps != 0) {
                    const double now[2] = {v->d.gainL, v->d.gainR};
                    double g[2];
                    double db = now[k];
                    for (int i = 0; i < std::abs(steps); ++i) db = steps > 0 ? volumeUp(db) : volumeDown(db);
                    moved(*v, now, k, db, g);
                    if (g[0] != now[0] || g[1] != now[1]) tellGain(hwnd, *v, g, kMeterGainSet);
                }
                return 0;
            }
        }
        break;
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
    static const char* const tabs[kMeterTargets] = {"Leveller", "Noise",   "Compressor", "MB Compressor", "De-esser",
                                                    "Soft clipper", "Limiter", "General", "Limiter", "MB Compressor"};
    return target >= 0 && target < kMeterTargets ? tabs[target] : "General";
}

static HINSTANCE g_instance = nullptr; // the module the class is registered for (the program, or the plug-in DLL)

void registerMeterView(HINSTANCE instance) {
    g_instance = instance;
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS; // the Volume fader's double-click
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

void meterViewGain(HWND view, double& left, double& right) {
    const View* v = reinterpret_cast<const View*>(GetWindowLongPtrW(view, GWLP_USERDATA));
    left = v ? v->gain[0] : 0.0;
    right = v ? v->gain[1] : 0.0;
}

bool meterViewLink(HWND view) {
    const View* v = reinterpret_cast<const View*>(GetWindowLongPtrW(view, GWLP_USERDATA));
    return !v || v->d.gainLink;
}

bool meterViewMono(HWND view) {
    const View* v = reinterpret_cast<const View*>(GetWindowLongPtrW(view, GWLP_USERDATA));
    return v && v->d.mono;
}

} // namespace ap
