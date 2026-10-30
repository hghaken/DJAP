// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysiswnd.h"

#include "placement.h"
#include "resource.h"
#include "stages.h"
#include "colour.h"
#include "deck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ap {

namespace {

const wchar_t* const kClass = L"APAnalysisWindow";

// The module the window class belongs to: the program, or the plug-in DLL (AnalysisWindow::setModule).
HINSTANCE g_module = nullptr;
bool g_registered = false;
HINSTANCE moduleInstance() { return g_module ? g_module : GetModuleHandleW(nullptr); }

// the meter panel's palette (meterview.cpp)
const COLORREF kBack = RGB(28, 30, 33), kPanel = RGB(20, 22, 25), kEdge = RGB(55, 58, 64), kGrid = RGB(70, 74, 80),
               kText = RGB(214, 218, 224), kDim = RGB(120, 126, 134), kBlue = RGB(80, 160, 235),
               kOrange = RGB(240, 130, 50), kTeal = RGB(80, 215, 195), kPurple = RGB(175, 145, 245),
               kTarget = RGB(250, 220, 90), kWell = RGB(44, 47, 52), kRed = RGB(235, 70, 70),
               kPink = RGB(235, 120, 175); // the noise reduction's line

const wchar_t* const kTips[8] = {
    L"The compressor's curve for this deck's loudness preset: output against input level, both in LU to the target (dashed: "
    L"1:1, no compression). Above the threshold the line bends by the ratio, with a soft knee; make-up lifts it "
    L"back. The white dot is where the compressor works now, the amber dots where it worked in the last 3 s (the "
    L"fainter, the older): the further up the bend, the more it compresses.",
    L"Loudness of the last 60 s, momentary (400 ms), in LU to the loudness preset's target: orange = original (the deck's "
    L"input), turquoise = processed (the output), -30 .. +18 LU. The processed line should stay close to the target line; the "
    L"original shows how much the songs' levels differ.",
    L"Gains of the last 60 s: leveller (blue; up = lifted), compressor (turquoise; its reduction, 0 or down), "
    L"density (purple; the average of its bands) and noise (pink; how far the noise reduction turned its most reduced "
    L"band down - it dips in pauses and quiet passages, a deeper cut than 18 dB lies on the bottom edge). Scale "
    L"-18 .. +18 dB.",
    L"Goniometer of the output (the last 43 ms): mono is a vertical line, left alone lies on the L diagonal, right "
    L"alone on the R diagonal; the wider the cloud, the wider the stereo image. A flat, horizontal cloud means left "
    L"and right are opposite (a phase problem).",
    L"Correlation of the output's left and right channel over 0.3 s: +1 = mono, 0 = unrelated (very wide), below "
    L"0 = opposite - parts of the sound cancel on a mono system.",
    L"Stereo width of the deck's input: the side (L - R) level against the mid (L + R) level. 0 % = mono, 100 % = as "
    L"much side as mid; music is usually 20 - 60 %.",
    L"Stereo width of the deck's output, as for the input: compare the two to see what the width stage does.",
    L"Scope of the last 2 s, left channel above, right below: the waveform's highest and lowest sample at each moment. "
    L"Red where samples stood at full scale (0 dBFS): clipping. Output: the dotted yellow lines are the limiter's "
    L"ceiling, which the output should stay inside; the output's own clip guard holds anything over full scale. "
    L"Input: what comes in - clipping there is in the source. Right-click: input or output.",
};

// The signal-chain panel (client, 2026-10-25: "for every step of the audio process a bar: does it do anything; start
// with an input volume bar and end with one"): a row per step in chain order. A level row shows loudness; a change
// row the level after the step against the level before it (0.4 s); a reduction row the step's own gain reduction.
enum class RowKind { level, change, reduction };
struct ChainRow {
    const wchar_t* name;
    RowKind kind;
    int bit;           // MeterRecord::active bit of the step (-1: always there)
    int from, to;      // change rows: the points of the chain before and after the step
    const wchar_t* tip;
};
const ChainRow kChain[14] = { // AnalysisWindow::kChainRows
    {L"Input", RowKind::level, -1, 0, 0,
     L"Input: the deck's loudness as it enters the processing - after the loudness preset's input gain (blue bar: momentary, "
     L"400 ms, LUFS) - and its peak (white mark: sample peak in dBFS, held and falling 20 dB/s). Scale -40 .. 0."},
    {L"Leveller", RowKind::change, sLeveller, ctInput, ctLeveller,
     L"Leveller: how much the slow automatic gain turns the song up or down now, in dB. Turquoise = louder, orange = "
     L"quieter."},
    {L"Noise", RowKind::reduction, sNoise, 0, 0,
     L"Noise reduction: how far its most turned-down band is down now (dB) - hiss, hum or rumble under the loudness "
     L"preset's noise threshold. Mostly nothing while music plays; bars in pauses and quiet passages."},
    {L"Colour", RowKind::change, kChainColourBit, ctNoise, ctColour,
     L"Colour: what the tone EQ, auto tone, sub bass and contour of the deck's colour do to the level (dB, "
     L"over 0.4 s). 'off' = a neutral colour."},
    {L"Compressor", RowKind::reduction, sCompressor, 0, 0,
     L"Compressor: its gain reduction now (dB). Bars only when the music is over its threshold."},
    {L"Density", RowKind::change, sDensity, ctCompressor, ctDensity,
     L"Density (multiband): what its bands do to the level together (dB, over 0.4 s): turquoise = lifts quiet "
     L"parts, orange = holds loud bands down."},
    {L"Bass enhancer", RowKind::change, sBass, ctDensity, ctBass,
     L"Bass enhancer: how much level its added bass harmonics bring (dB, over 0.4 s)."},
    {L"Exciter", RowKind::change, sExciter, ctBass, ctExciter,
     L"Exciter: how much level its added treble harmonics bring (dB, over 0.4 s)."},
    {L"De-esser", RowKind::reduction, sDeEsser, 0, 0,
     L"De-esser: its reduction of sharp 's' sounds now (dB). Bars only on sibilance."},
    {L"Stereo width", RowKind::change, sWidth, ctDeEsser, ctWidth,
     L"Stereo width: the level change from making the sound wider (turquoise: more side) or narrower / the bass "
     L"mono (orange), dB over 0.4 s."},
    {L"Soft clipper", RowKind::reduction, sClipper, 0, 0,
     L"Soft clipper: how much it rounds off the highest peaks now (dB). Its drive is part of the loudness preset's level "
     L"plan: the leveller aims that much lower and the clipper adds it back."},
    {L"Limiter", RowKind::reduction, sLimiter, 0, 0,
     L"Limiter: its gain reduction now (dB), keeping the true peak under the ceiling."},
    {L"Output gain", RowKind::change, -1, ctLimiter, ctOutput,
     L"Output gain: the loudness preset's last trim after the limiter (only down), dB. Also shows a bypass (the output is "
     L"then the input)."},
    {L"Output", RowKind::level, -1, 0, 0,
     L"Output: the deck's loudness as it leaves (blue bar: momentary, 400 ms, LUFS) and its peak (white mark: true "
     L"peak in dBTP, held and falling 20 dB/s, never over the limiter's ceiling); the yellow mark is the loudness preset's "
     L"target (after its output gain). Scale -40 .. 0."},
};

// The settings tab of each chain row (the preset window's stage; the colour row opens the colour window).
const char* const kChainTab[14] = {
    "General", "Leveller",     "Noise",        "Tone",    "Compressor", "Density", "Bass enhancer",
    "Exciter", "De-esser",     "Stereo width", "Soft clipper", "Limiter", "General", "General"};
constexpr int kColourRow = 3;

void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void frame(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

void text(HDC dc, int x, int y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT) {
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int style = PS_SOLID, int width = 1) {
    HPEN pen = CreatePen(style, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

// A polyline in pieces: a point with valid = false breaks it.
struct Trace {
    HDC dc;
    COLORREF color;
    int width;
    std::vector<POINT> pts;
    void add(int x, int y, bool valid) {
        if (!valid) {
            flush();
            return;
        }
        pts.push_back({x, y});
    }
    void flush() {
        if (pts.size() >= 2) {
            HPEN pen = CreatePen(PS_SOLID, width, color);
            HGDIOBJ old = SelectObject(dc, pen);
            Polyline(dc, pts.data(), static_cast<int>(pts.size()));
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        pts.clear();
    }
};

std::wstring num(const wchar_t* fmt, double v) {
    wchar_t b[32];
    swprintf(b, 32, fmt, v);
    return b;
}

} // namespace

AnalysisWindow::AnalysisWindow(const AnalysisSource& source) : source_(source) {
    for (auto& deck : trail_)
        for (double& x : deck) x = kFloorDb; // no point yet
}

AnalysisWindow::~AnalysisWindow() { close(); }

void AnalysisWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void AnalysisWindow::setModule(HINSTANCE instance) { g_module = instance; }

void AnalysisWindow::unregisterClass() {
    if (g_registered) UnregisterClassW(kClass, moduleInstance());
    g_registered = false;
}

void AnalysisWindow::show(HWND owner) {
    if (!hwnd_) {
        HINSTANCE inst = moduleInstance();
        if (!g_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
            wc.lpszClassName = kClass;
            g_registered = RegisterClassExW(&wc) != 0;
        }
        hwnd_ = CreateWindowExW(0, kClass, source_.windowTitle().c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, owner, nullptr, inst, this);
        if (!hwnd_) return;
        RECT r{0, 0, px(1560), px(640)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        placement::remember(hwnd_, "Analysis", owner);
    }
    ShowWindow(hwnd_, placement::showCommand(hwnd_));
    SetForegroundWindow(hwnd_);
}

void AnalysisWindow::refresh() {
    // the trail is kept while the window is closed too: it opens with the last 3 s
    trailPos_ = (trailPos_ + 1) % kTrail;
    for (int d = 0; d < source_.decks(); ++d) {
        const DeckMeters& m = source_.meters(d);
        trail_[d][trailPos_] = source_.audioActive() && m.live && m.compressorIn > -100.0 ? m.compressorIn : kFloorDb;
    }
    if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK AnalysisWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AnalysisWindow* self = reinterpret_cast<AnalysisWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<AnalysisWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return r;
}

LRESULT AnalysisWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        setFonts();
        tips_.create(hwnd_);
        for (int d = 0; d < 2; ++d)
            for (int p = 0; p < kParts; ++p) {
                const std::wstring tip = p < kGraphs ? std::wstring(kTips[p])
                                                     : std::wstring(kChain[p - kGraphs].tip) +
                                                           (p - kGraphs == kColourRow ? L" Click: opens the colour window."
                                                                                      : L" Click: opens its settings.");
                tips_.addRect(hwnd_, static_cast<UINT_PTR>(d * kParts + p), tip.c_str());
                tipRect_[d][p] = RECT{};
            }
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        setFonts();
        tips_.setDpi(dpi_);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(1060), px(480)};
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR: { // a hand over the chain rows: a click opens their settings
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        int deck, row;
        if (LOWORD(lp) == HTCLIENT && openStage_ && chainRowAt(pt, deck, row)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        int deck, row;
        if (openStage_ && chainRowAt(pt, deck, row)) openStage_(deck, kChainTab[row], row == kColourRow);
        return 0;
    }
    case WM_CONTEXTMENU: { // a right-click on a scope: the input or the output (client, 2026-10-27)
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (lp == -1) break; // from the keyboard: no point to go by
        POINT local = pt;
        ScreenToClient(hwnd_, &local);
        for (int d = 0; d < 2; ++d) {
            if (!PtInRect(&scopeRect_[d], local)) continue;
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING | (scopeInput_[d] ? MF_CHECKED : 0), 1, L"Scope of the &input");
            AppendMenuW(m, MF_STRING | (scopeInput_[d] ? 0 : MF_CHECKED), 2, L"Scope of the &output");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            // client, 2026-10-27: 20 / 50 / 200 ms and 2 s, then "1, 5 and 10 ms too"
            const int spans[7] = {1, 5, 10, 20, 50, 200, 2000};
            const wchar_t* spanNames[7] = {L"1 ms across",  L"5 ms across",   L"10 ms across", L"20 ms across",
                                           L"50 ms across", L"200 ms across", L"2 s across"};
            for (int k = 0; k < 7; ++k)
                AppendMenuW(m, MF_STRING | (scopeMs_[d] == spans[k] ? MF_CHECKED : 0), static_cast<UINT_PTR>(10 + k),
                            spanNames[k]);
            const int choice = TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, hwnd_, nullptr);
            DestroyMenu(m);
            if (choice == 1 || choice == 2) scopeInput_[d] = choice == 1;
            if (choice >= 10 && choice < 17) scopeMs_[d] = spans[choice - 10];
            if (choice) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        for (HFONT* f : {&font_, &small_, &bold_}) {
            if (*f) DeleteObject(*f);
            *f = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void AnalysisWindow::setFonts() {
    for (HFONT* f : {&font_, &small_, &bold_})
        if (*f) DeleteObject(*f);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 85 / 100;
    small_ = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = lf.lfHeight * 12 / 10;
    bold_ = CreateFontIndirectW(&lf);
}

bool AnalysisWindow::chainRowAt(POINT pt, int& deck, int& row) const {
    for (int d = 0; d < source_.decks(); ++d)
        for (int i = 0; i < kChainRows; ++i)
            if (PtInRect(&tipRect_[d][kGraphs + i], pt)) { // the rows as last painted
                deck = d;
                row = i;
                return true;
            }
    return false;
}

void AnalysisWindow::placeTip(int d, int part, const RECT& r) {
    if (EqualRect(&r, &tipRect_[d][part])) return;
    tipRect_[d][part] = r;
    tips_.setRect(hwnd_, static_cast<UINT_PTR>(d * kParts + part), r);
}

void AnalysisWindow::paint(HDC target) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, kPanel); // the gaps of dotted lines
    fill(dc, rc, kBack);
    const int rows = source_.decks(); // one channel (or the plug-in): one row over the whole height
    const int m = px(10), half = rows == 1 ? H - 2 * m : (H - 3 * m) / 2;
    for (int d = 0; d < rows; ++d) {
        const RECT area{m, m + d * (half + m), W - m, m + d * (half + m) + half};
        paintDeck(dc, d, area);
    }
    if (rows == 1)
        for (int p = 0; p < kParts; ++p) placeTip(1, p, RECT{}); // deck B's graphs are not there
    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void AnalysisWindow::paintDeck(HDC dc, int d, const RECT& area) {
    const DeckMeters& met = source_.meters(d);
    const DeckAnalysis& an = source_.analysis(d);
    const PresetValues v = source_.values(d);
    const double target = v[pTarget];
    const bool live = source_.audioActive() && met.live;
    TEXTMETRICW tm;
    SelectObject(dc, small_);
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight, g = px(8);

    // title
    SelectObject(dc, bold_);
    const std::wstring title = source_.title(d) + (source_.processing(d) ? L"" : L"   (bypassed)");
    text(dc, area.left, area.top, title, kText);
    TEXTMETRICW btm;
    GetTextMetricsW(dc, &btm);
    SelectObject(dc, small_);
    const int top = area.top + btm.tmHeight + px(4), bottom = area.bottom;
    const int h = bottom - top, barsW = 0, chainW = px(360); // correlation / width: inside the goniometer's panel
    // the signal chain first, from input to output
    const RECT chain{area.left, top, area.left + chainW, bottom};
    paintChain(dc, d, chain, live);
    const int left0 = chain.right + g, room = area.right - left0;
    // compressor and goniometer square, but never wider than a fifth of the room (one tall row); between them the
    // loudness and gain histories and the scope
    const int sq = std::min(h, (room - barsW - 5 * g) / 5);
    RECT panel[5];
    if (h >= 2 * sq + g) {
        // a tall row (one deck, the plug-in): the compressor and the goniometer one above the other in the first column
        // (client, 2026-10-29: "put the compressor and the stereo graph under each other") - the histories and the
        // scope get the width
        const int half = (h - g) / 2, col = std::min(half, (room - 3 * g) / 4);
        const int flexW = std::max(px(120), (room - col - 3 * g) / 3);
        panel[0] = RECT{left0, top, left0 + col, top + half};
        panel[4] = RECT{left0, top + half + g, left0 + col, bottom};
        int x = left0 + col + g;
        for (int i = 1; i <= 3; ++i) {
            panel[i] = RECT{x, top, x + flexW, bottom};
            x += flexW + g;
        }
    } else {
        const int flexW = std::max(px(120), (room - 2 * sq - barsW - 5 * g) / 3);
        int x = left0;
        const int widths[5] = {sq, flexW, flexW, flexW, sq};
        for (int i = 0; i < 5; ++i) {
            panel[i] = RECT{x, top, x + widths[i], bottom};
            x += widths[i] + g;
        }
    }
    for (int i = 0; i < 5; ++i) {
        fill(dc, panel[i], kPanel);
        frame(dc, panel[i], kEdge);
        if (i < 3) placeTip(d, i, panel[i]); // the goniometer's tip covers its plot only (below)
    }
    placeTip(d, 7, panel[3]);
    paintScope(dc, d, panel[3], live, th);

    // ---- compressor curve ----
    {
        const RECT& p = panel[0];
        text(dc, p.left + px(6), p.top + px(4), L"Compressor: out", kDim);
        const int l = p.left + px(8), r = p.right - px(8);
        int t = p.top + th + px(8), b = p.bottom - th - px(10);
        if (b - t > r - l) { // square, centred (a tall panel)
            const int extra = (b - t) - (r - l);
            t += extra / 2;
            b -= extra - extra / 2;
        }
        const double lo = -24.0, hi = 12.0;
        auto X = [&](double lu) { return l + static_cast<int>((r - l) * (lu - lo) / (hi - lo) + 0.5); };
        auto Y = [&](double lu) { return b - static_cast<int>((b - t) * (std::min(hi, std::max(lo, lu)) - lo) / (hi - lo) + 0.5); };
        line(dc, X(lo), Y(lo), X(hi), Y(hi), kGrid, PS_DOT); // 1:1
        line(dc, X(0), t, X(0), b, kGrid, PS_DOT);
        line(dc, l, Y(0), r, Y(0), kGrid, PS_DOT);
        text(dc, p.left + px(6), p.bottom - th - px(4), L"in (LU to the target)", kDim);
        const bool on = v[pCompOn] >= 0.5;
        const double thr = target + v[pCompThreshold], ratio = v[pCompRatio], knee = v[pCompKnee];
        const double makeup = v[pCompMakeup] < 0 ? target - compressorCurveDb(target, thr, ratio, knee) : v[pCompMakeup];
        auto out = [&](double in) {
            return on ? compressorCurveDb(target + in, thr, ratio, knee) + makeup - target : in;
        };
        Trace tr{dc, kTeal, std::max(1, px(2)), {}};
        for (int i = 0; i <= 72; ++i) {
            const double in = lo + (hi - lo) * i / 72.0;
            tr.add(X(in), Y(out(in)), true);
        }
        tr.flush();
        if (!on) text(dc, p.right - px(6), p.top + px(4), L"off", kDim, TA_RIGHT);
        if (on) {
            // the working point now (white) and of the last 3 s (amber, fading with age): where on its curve the
            // compressor works, and how far it moves
            auto dot = [&](double level, int rr, COLORREF c) {
                const double in = level - target;
                const int cx = X(std::min(hi, std::max(lo, in))), cy = Y(out(in));
                HBRUSH br = CreateSolidBrush(c);
                HGDIOBJ ob = SelectObject(dc, br);
                HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, cx - rr, cy - rr, cx + rr + 1, cy + rr + 1);
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(br);
            };
            auto blend = [](COLORREF a, COLORREF b, double t) { // a at t = 1, b at t = 0
                return RGB(static_cast<int>(GetRValue(b) + t * (GetRValue(a) - GetRValue(b))),
                           static_cast<int>(GetGValue(b) + t * (GetGValue(a) - GetGValue(b))),
                           static_cast<int>(GetBValue(b) + t * (GetBValue(a) - GetBValue(b))));
            };
            const COLORREF amber = RGB(235, 165, 55);
            for (int k = 1; k < kTrail; ++k) { // oldest first, so newer dots lie on top
                const double level = trail_[d][(trailPos_ + k) % kTrail];
                if (level <= -100.0) continue;
                const double fresh = static_cast<double>(k) / (kTrail - 1); // 0 oldest .. 1 newest
                dot(level, std::max(1, px(3)), blend(amber, kPanel, 0.25 + 0.75 * fresh));
            }
            if (live && met.compressorIn > -100.0) dot(met.compressorIn, px(4.5), RGB(250, 250, 250));
        }
    }

    // ---- loudness and gain history (60 s) ----
    for (int which = 0; which < 2; ++which) {
        const RECT& p = panel[1 + which];
        const int l = p.left + px(8), r = p.right - px(30), t = p.top + th + px(12), b = p.bottom - th - px(12);
        // room for what the music does (client, 2026-10-27: "the graphs clip at the bottom and top"): a loud original
        // in a quiet preset lies 15-18 LU over its target (-24 .. +6 cut it off), the leveller's range goes to 15 dB
        // (-12 .. +12 cut it off); loudness under -30 LU (a pause, a fade's end) is left out instead of lying on the edge
        const double lo = which == 0 ? -30.0 : -18.0, hi = 18.0;
        auto Y = [&](double vdb) { return b - static_cast<int>((b - t) * (std::min(hi, std::max(lo, vdb)) - lo) / (hi - lo) + 0.5); };
        auto X = [&](int i) { return l + static_cast<int>(static_cast<double>(r - l) * i / (DeckAnalysis::kHistory - 1) + 0.5); };
        text(dc, p.left + px(6), p.top + px(4),
             which == 0 ? L"Loudness, LU to the target - last 60 s" : L"Gain, dB - last 60 s", kDim);
        for (double gv = lo + 6.0; gv <= hi; gv += 6.0) {
            line(dc, l, Y(gv), r, Y(gv), gv == 0.0 ? RGB(110, 114, 120) : kGrid, PS_DOT);
            text(dc, p.right - px(4), Y(gv) - th / 2, num(gv > 0 ? L"+%.0f" : L"%.0f", gv), kDim, TA_RIGHT);
        }
        for (int s = 10; s < 60; s += 10) { // a tick every 10 s
            const int xs = X(DeckAnalysis::kHistory - 1 - s * 10);
            line(dc, xs, b, xs, b + px(3), kGrid);
        }
        if (which == 0) text(dc, r, Y(0) - th - px(1), L"target", kDim, TA_RIGHT);
        { // legend, bottom left
            const wchar_t* names[2][4] = {{L"original", L"processed", nullptr, nullptr},
                                          {L"leveller", L"compressor", L"density", L"noise"}};
            const COLORREF cols[2][4] = {{kOrange, kTeal, 0, 0}, {kBlue, kTeal, kPurple, kPink}};
            int xl = p.left + px(6);
            for (int k = 0; k < 4 && names[which][k]; ++k) {
                text(dc, xl, p.bottom - th - px(4), names[which][k], cols[which][k]);
                SIZE sz{};
                GetTextExtentPoint32W(dc, names[which][k], static_cast<int>(wcslen(names[which][k])), &sz);
                xl += sz.cx + px(12);
            }
        }
        const int lines = which == 0 ? 2 : 4;
        const COLORREF colors[2][4] = {{kOrange, kTeal, 0, 0}, {kBlue, kTeal, kPurple, kPink}};
        for (int k = 0; k < lines; ++k) {
            // one pixel: 600 points of 100 ms in a few hundred pixels - a thicker line ran together into a band
            // (client, 2026-10-27: "thinner, no thick mush")
            Trace tr{dc, colors[which][k], 1, {}};
            for (int i = 0; i < DeckAnalysis::kHistory; ++i) {
                const DeckAnalysis::Point& pt = an.at(i);
                double val;
                bool ok = pt.live;
                if (which == 0) {
                    const double lufs = k == 0 ? pt.inM : pt.outM;
                    val = lufs - target;
                    ok = ok && lufs > -70.0 && val >= lo; // under the scale: a gap, not a line on the edge
                } else {
                    val = k == 0 ? pt.leveller : k == 1 ? pt.compressor : k == 2 ? pt.density : pt.noise;
                }
                tr.add(X(i), Y(val), ok);
            }
            tr.flush();
        }
    }

    // ---- goniometer, with the correlation and width bars under it (client, 2026-10-25: "the three values in the
    // same stereo graph; do not make it taller"): plot and bars as one block, centred in the panel; the plot as large
    // a square as the panel leaves after the bars ----
    const RECT& gp = panel[4];
    const int rowH = th + px(10), barsH = 3 * rowH;
    const int side = std::max(px(60), std::min(static_cast<int>(gp.right - gp.left),
                                                static_cast<int>(gp.bottom - gp.top) - barsH - px(10)));
    const int y0 = gp.top + std::max(0, static_cast<int>(gp.bottom - gp.top) - side - barsH - px(4)) / 2;
    const RECT plot{gp.left + (gp.right - gp.left - side) / 2, y0, gp.left + (gp.right - gp.left - side) / 2 + side,
                    y0 + side};
    placeTip(d, 3, plot);
    {
        const RECT& p = plot;
        const int cx = (p.left + p.right) / 2, cy = (p.top + p.bottom) / 2;
        const int rad = std::min(p.right - p.left, p.bottom - p.top) / 2 - th - px(6);
        line(dc, cx - rad, cy - rad, cx + rad, cy + rad, kGrid, PS_DOT);
        line(dc, cx + rad, cy - rad, cx - rad, cy + rad, kGrid, PS_DOT);
        line(dc, cx, cy - rad, cx, cy + rad, kGrid, PS_DOT);
        line(dc, cx - rad, cy, cx + rad, cy, kGrid, PS_DOT);
        text(dc, p.left + px(6), p.top + px(4), L"L", kDim);
        text(dc, cx, p.top + px(4), L"mono", kDim, TA_CENTER);
        text(dc, p.right - px(6), p.top + px(4), L"R", kDim, TA_RIGHT);
        text(dc, p.left + px(6), p.bottom - th - px(4), L"output", kDim);
        if (live) {
            const double k = rad * 0.7071;
            HBRUSH br = CreateSolidBrush(kTeal);
            const int dot = std::max(1, px(1.5));
            for (const auto& pt : an.stereoOut.dots()) {
                const int gx = cx + static_cast<int>((pt[1] - pt[0]) * k), gy = cy - static_cast<int>((pt[0] + pt[1]) * k);
                if (std::abs(gx - cx) > rad || std::abs(gy - cy) > rad) continue;
                const RECT d0{gx, gy, gx + dot, gy + dot};
                FillRect(dc, &d0, br);
            }
            DeleteObject(br);
        }
    }

    // ---- correlation and width bars, under the plot ----
    {
        const wchar_t* names[3] = {L"Correlation", L"Width in", L"Width out"};
        const int by = plot.bottom + px(4);
        for (int i = 0; i < 3; ++i) {
            const RECT box{gp.left + px(2), by + i * rowH, gp.right - px(2), by + (i + 1) * rowH};
            placeTip(d, 4 + i, box);
            const int cy = (box.top + box.bottom) / 2;
            text(dc, box.left + px(6), cy - th / 2, names[i], kText);
            const StereoAnalyzer& st = i == 2 ? an.stereoOut : i == 1 ? an.stereoIn : an.stereoOut;
            const bool show = live && !st.silent();
            const double val = i == 0 ? st.correlation() : st.widthPercent();
            text(dc, box.right - px(6), cy - th / 2, show ? num(i == 0 ? L"%+.2f" : L"%.0f %%", val) : L"--", kText,
                 TA_RIGHT);
            // the bar between the name and the number
            const int l = box.left + px(78), r = box.right - px(46), t = cy - px(4), b = cy + px(4);
            if (r - l < px(20)) continue;
            const RECT well{l, t, r, b};
            fill(dc, well, kWell);
            if (i == 0) {
                const int mid = (l + r) / 2;
                line(dc, mid, t - px(2), mid, b + px(2), kDim);
                if (show) {
                    const int xv = mid + static_cast<int>((r - l) / 2 * val);
                    fill(dc, RECT{std::min(mid, xv), t, std::max(mid, xv) + 1, b}, val >= 0 ? kTeal : kOrange);
                }
            } else if (show) {
                const int xv = l + static_cast<int>((r - l) * std::min(200.0, val) / 200.0);
                fill(dc, RECT{l, t, std::max(l + 1, xv), b}, kBlue);
                const int x100 = (l + r) / 2;
                line(dc, x100, t - px(2), x100, b + px(2), kDim); // 100 %
            }
        }
    }
}

// The scope (client, 2026-10-27: "an oscilloscope of L and R, to check whether it clips - of the input and the output,
// chosen with a right-click"; then "the sines are too close together"): 20, 50 (the default) or 200 ms of the samples
// themselves, ending at the newest rising zero crossing of the left channel (a trigger: a tone stands still), or the
// last 2 s as each moment's lowest and highest sample; left above, right below, on a linear scale (-1 .. +1 = full
// scale); red where samples stood at full scale. The output also gets the limiter's ceiling (yellow, dotted). The
// clip count and the peaks under it are always over the last 2 s.
void AnalysisWindow::paintScope(HDC dc, int d, const RECT& p, bool live, int th) {
    scopeRect_[d] = p;
    const bool input = scopeInput_[d];
    const WaveScope& s = input ? source_.analysis(d).scopeIn : source_.analysis(d).scopeOut;
    const PresetValues v = source_.values(d);
    const bool ceilingShown = !input && v[pLimOn] >= 0.5 && source_.processing(d);
    const double ceiling = dbToLin(v[pLimCeiling]);
    const int span = scopeMs_[d];
    // the span that fits the kept samples (eighth outside review: at 88.2 kHz and up "200 ms" showed 85-186 ms)
    const double fitsMs = s.fs > 0.0 ? 1000.0 * (WaveScope::kRaw / 2) / s.fs : span;
    wchar_t spanText[24];
    if (span >= 1000) swprintf(spanText, 24, L"last 2 s");
    else if (span > fitsMs) swprintf(spanText, 24, L"%.0f ms", fitsMs);
    else swprintf(spanText, 24, L"%d ms", span);
    text(dc, p.left + px(6), p.top + px(4),
         std::wstring(input ? L"Scope: input" : L"Scope: output") + L", L / R - " + spanText + L"  (right-click: more)",
         kDim);
    const int l = p.left + px(22), r = p.right - px(8), t = p.top + th + px(10), b = p.bottom - th - px(12);
    const int gap = px(8), laneH = (b - t - gap) / 2;
    if (laneH < px(20) || r - l < px(40)) return;
    long clipped[2] = {0, 0};
    float peak[2] = {0.0f, 0.0f};
    for (int k = 0; k < WaveScope::kBuckets; ++k) {
        const WaveScope::Bucket& bk = s.at(k);
        for (int c = 0; c < 2; ++c) {
            clipped[c] += bk.clipped[c];
            peak[c] = std::max(peak[c], std::max(std::fabs(bk.lo[c]), std::fabs(bk.hi[c])));
        }
    }
    for (int c = 0; c < 2; ++c) {
        const int top = t + c * (laneH + gap), mid = top + laneH / 2;
        const double amp = laneH / 2.0 - px(2); // full scale a little inside the lane
        auto yOf = [&](double x) { return mid - static_cast<int>(std::max(-1.0, std::min(1.0, x)) * amp + (x >= 0 ? 0.5 : -0.5)); };
        text(dc, p.left + px(8), mid - th / 2, c == 0 ? L"L" : L"R", kDim);
        line(dc, l, mid, r, mid, kGrid, PS_DOT);
        line(dc, l, yOf(1.0), r, yOf(1.0), RGB(120, 60, 60), PS_DOT);
        line(dc, l, yOf(-1.0), r, yOf(-1.0), RGB(120, 60, 60), PS_DOT);
        if (ceilingShown) {
            line(dc, l, yOf(ceiling), r, yOf(ceiling), RGB(150, 130, 60), PS_DOT);
            line(dc, l, yOf(-ceiling), r, yOf(-ceiling), RGB(150, 130, 60), PS_DOT);
        }
        if (!live) continue;
        const COLORREF wave = input ? kOrange : kTeal;
        if (span >= 1000) { // the last 2 s: each bucket's range; the clipped ones last, so a narrow panel (several
                            // buckets on one column) never paints over them (eighth outside review)
            for (const bool red : {false, true})
                for (int k = 0; k < WaveScope::kBuckets; ++k) {
                    const WaveScope::Bucket& bk = s.at(k);
                    if ((bk.clipped[c] > 0) != red) continue;
                    const int x = l + static_cast<int>(static_cast<double>(r - l) * k / WaveScope::kBuckets);
                    line(dc, x, yOf(bk.hi[c]), x, yOf(bk.lo[c]) + 1, red ? kRed : wave);
                }
            continue;
        }
        // the samples of `span` ms, ending at the newest rising zero crossing of the left channel within the last
        // `span` ms (none: at the newest sample)
        const long long frames = std::min<long long>(WaveScope::kRaw / 2, std::llround(s.fs * span / 1000.0));
        const long long newest = s.rawWritten;
        if (newest < frames + 2) continue;
        // the trigger looks back at least 50 ms: a short span (1 ms) holds no zero crossing of a low tone
        const long long look = std::max(frames, std::llround(s.fs * 0.05));
        long long end = newest;
        for (long long k = newest - 1; k > newest - look && k > newest - WaveScope::kRaw + frames + 1; --k)
            if (s.sample(0, k - 1) < 0.0f && s.sample(0, k) >= 0.0f) {
                end = k;
                break;
            }
        const long long start = end - frames;
        const int cols = r - l;
        if (frames < cols) { // fewer samples than pixels (1 - 10 ms): a line through the samples
            int px0 = l, py0 = yOf(s.sample(c, start));
            for (long long k = 1; k <= frames; ++k) {
                const float v0 = s.sample(c, start + k - 1), v1 = s.sample(c, start + k);
                const int x1 = l + static_cast<int>(static_cast<double>(cols) * k / frames), y1 = yOf(v1);
                const bool clip = std::max(std::fabs(v0), std::fabs(v1)) >= WaveScope::kClipLevel;
                line(dc, px0, py0, x1, y1, clip ? kRed : wave);
                px0 = x1;
                py0 = y1;
            }
            continue;
        }
        int prevY = yOf(s.sample(c, start));
        for (int x = 0; x < cols; ++x) {
            const long long a = start + frames * x / cols, z = std::max(a + 1, start + frames * (x + 1) / cols);
            float lo = s.sample(c, a), hi = lo;
            for (long long k = a; k < z; ++k) {
                lo = std::min(lo, s.sample(c, k));
                hi = std::max(hi, s.sample(c, k));
            }
            const bool clip = std::max(std::fabs(lo), std::fabs(hi)) >= WaveScope::kClipLevel;
            // from where the last column ended, so the line stays one line
            const int yHi = std::min(yOf(hi), prevY), yLo = std::max(yOf(lo), prevY);
            line(dc, l + x, yHi, l + x, yLo + 1, clip ? kRed : wave);
            prevY = yOf(s.sample(c, z - 1));
        }
    }
    // under it: the peak of each channel and the samples at full scale in these 2 s
    std::wstring info;
    if (!live) {
        info = L"no audio";
    } else {
        // in full, else (a narrow panel: the counts ran into the next one) without the units and spaces
        for (int form = 0; form < 2; ++form) {
            const bool full = form == 0;
            info.clear();
            for (int c = 0; c < 2; ++c) {
                info += c == 0 ? L"L " : (full ? L"   R " : L"  R ");
                info += peak[c] > 0.0f ? num(full ? L"%.1f dBFS" : L"%.1f", linToDb(peak[c])) : std::wstring(L"silent");
            }
            if (!full) info += L" dBFS";
            info += clipped[0] + clipped[1] > 0
                        ? (full ? L"   clipped: " : L"  clipped ") + std::to_wstring(clipped[0]) + L" / " +
                              std::to_wstring(clipped[1]) + (full ? L" samples" : L"")
                        : std::wstring(full ? L"   no clipping" : L"  no clipping");
            SIZE sz{};
            GetTextExtentPoint32W(dc, info.c_str(), static_cast<int>(info.size()), &sz);
            if (sz.cx <= p.right - p.left - px(12)) break;
        }
    }
    text(dc, p.left + px(6), p.bottom - th - px(4), info, live && clipped[0] + clipped[1] > 0 ? kRed : kDim);
}

void AnalysisWindow::paintChain(HDC dc, int d, const RECT& p, bool live) {
    const DeckMeters& met = source_.meters(d);
    const PresetValues v = source_.values(d);
    fill(dc, p, kPanel);
    frame(dc, p, kEdge);
    TEXTMETRICW tm;
    SelectObject(dc, small_);
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight;
    text(dc, p.left + px(6), p.top + px(4),
         met.flags & mfBypassed ? L"Signal chain - bypassed: the output is the input"
                                : L"Signal chain - what each step does now (dB; input / output: LUFS, peak dB)",
         kDim);
    int t = p.top + th + px(10), b = p.bottom - px(6);
    const int lampX = p.left + px(13), nameX = p.left + px(24), barL = p.left + px(124), barR = p.right - px(16);
    const int mid = (barL + barR) / 2, halfW = (barR - barL) / 2;
    // the scales: dB from the middle on a square-root scale (0.5 dB shows, 12 dB fills); LUFS / dB -40 .. 0 for the
    // input and output rows
    auto xDb = [&](double db) {
        return mid + static_cast<int>(halfW * (db < 0 ? -1 : 1) * std::sqrt(std::min(12.0, std::fabs(db)) / 12.0));
    };
    auto xLevel = [&](double lvl) { return barL + static_cast<int>((barR - barL) * (std::min(0.0, std::max(-40.0, lvl)) + 40.0) / 40.0); };
    auto scale = [&](bool level, int y) { // the numbers of a scale, centred under their ticks, tops at y
        // from the middle outwards, so 0 and the large values win when two numbers would touch
        const double dbs[] = {0, -1, 1, -6, 6, -12, 12}, lvls[] = {0, -10, -20, -30, -40};
        std::vector<std::pair<int, int>> used; // left / right of the numbers drawn
        for (double s : level ? std::vector<double>(lvls, lvls + 5) : std::vector<double>(dbs, dbs + 7)) {
            const int xs = level ? xLevel(s) : xDb(s);
            const std::wstring label = level || s <= 0 ? num(L"%.0f", s) : num(L"+%.0f", s);
            SIZE sz{};
            GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &sz);
            const int l = xs - sz.cx / 2 - px(2), r = xs + sz.cx / 2 + px(2);
            bool free = true;
            for (const auto& u : used) free = free && (r <= u.first || l >= u.second);
            line(dc, xs, y - px(3), xs, y - px(1), kGrid);
            if (!free) continue;
            used.push_back({l, r});
            text(dc, xs, y, label, kDim, TA_CENTER);
        }
    };
    // a scale under every bar when the rows have room, else the dB scale once at the top and the LUFS scale at the
    // bottom (two decks in a small window)
    const int wellH = px(10), need = wellH + th + px(8);
    const bool perRow = (b - t) / kChainRows >= need + px(2);
    if (!perRow) {
        scale(false, t + px(2));
        t += th + px(6);
        b -= th + px(4);
        scale(true, b + px(4));
        text(dc, nameX, t - th - px(4), L"dB", kDim);
        text(dc, nameX, b + px(4), L"LUFS / dB", kDim);
    }
    const int rowH = (b - t) / kChainRows;
    // the signal flows down through the lamps
    line(dc, lampX, t + rowH / 2, lampX, t + (kChainRows - 1) * rowH + rowH / 2, kGrid);
    const bool signal = live && met.chain[ctInput] > 1e-10; // -100 dB: no music, no changes to show
    for (int i = 0; i < kChainRows; ++i) {
        const ChainRow& row = kChain[i];
        const RECT box{p.left + px(2), t + i * rowH, p.right - px(2), t + (i + 1) * rowH};
        placeTip(d, kGraphs + i, box);
        const int cy = (box.top + box.bottom) / 2;
        const bool on = row.bit < 0 || (live ? (met.active >> row.bit & 1) != 0
                                             : row.bit == kChainColourBit ? makeColourConfig(v, 48000, 3000).any()
                                                                          : v[kStageParam[row.bit]] >= 0.5);
        // the value: LUFS for a level row, dB for the others. The input is shown after the loudness preset's input gain
        // (what enters the processing); the measured points around the leveller include that gain, so the
        // leveller's row takes it out (nothing is applied while bypassed)
        const double inGain = source_.processing(d) ? v[pInputGain] : 0.0;
        double val = 0;
        bool known = live;
        if (row.kind == RowKind::level) {
            val = i == 0 ? met.inM + inGain : met.outM;
            known = known && val > -70.0;
        } else if (row.kind == RowKind::change) {
            known = signal && met.chain[row.from] > 1e-12 && met.chain[row.to] > 1e-12;
            if (known) val = 10.0 * std::log10(met.chain[row.to] / met.chain[row.from]) - (i == 1 ? inGain : 0.0);
        } else {
            const double red = row.bit == sCompressor ? met.compressorDb : row.bit == sDeEsser ? met.deEsserDb
                               : row.bit == sClipper  ? met.clipperDb   : row.bit == sNoise ? met.noiseMaxDb()
                                                                                             : met.limiterDb;
            val = -red;
        }
        const bool acting = known && on && row.kind != RowKind::level && std::fabs(val) >= 0.1;
        // lamp: lit while the step changes the sound by 0.1 dB or more
        {
            const int rr = px(4);
            HBRUSH br = CreateSolidBrush(acting ? (val > 0 ? kTeal : kOrange) : row.kind == RowKind::level ? kBlue : kWell);
            HPEN pen = CreatePen(PS_SOLID, 1, on ? kDim : kEdge);
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
            Ellipse(dc, lampX - rr, cy - rr, lampX + rr + 1, cy + rr + 1);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            DeleteObject(br);
        }
        SelectObject(dc, row.kind == RowKind::level ? font_ : small_);
        TEXTMETRICW ntm;
        GetTextMetricsW(dc, &ntm);
        text(dc, nameX, cy - ntm.tmHeight / 2, row.name, on ? kText : kDim);
        SelectObject(dc, small_);
        // the bar, raised by half the scale's height when its numbers go under it
        const int wy = perRow ? cy - (th + px(4)) / 2 : cy;
        const RECT well{barL, wy - wellH / 2, barR, wy + wellH / 2};
        fill(dc, well, kWell);
        if (perRow) scale(row.kind == RowKind::level, well.bottom + px(4));
        if (row.kind == RowKind::level) {
            // loudness in LUFS from the left (blue), the peak in dB on the same axis (white mark; input: sample peak
            // in dBFS, output: true peak in dBTP); the output's target in yellow
            const double peak = i == 0 ? std::max(met.inPeak[0], met.inPeak[1]) + inGain : std::max(met.tpNow[0], met.tpNow[1]);
            const bool peakKnown = live && peak > -70.0;
            if (known) fill(dc, RECT{barL, well.top, std::max(barL + 1, xLevel(val)), well.bottom}, kBlue);
            if (peakKnown) {
                const int xp = std::min(barR - 1, xLevel(peak));
                fill(dc, RECT{xp - std::max(1, px(1)), well.top - px(2), xp + std::max(1, px(1)), well.bottom + px(2)}, kText);
            }
            if (i == kChainRows - 1) {
                const int xt = xLevel(v[pTarget] + v[pOutputGain]);
                line(dc, xt, well.top - px(2), xt, well.bottom + px(2), kTarget, PS_SOLID, std::max(1, px(2)));
            }
            continue;
        }
        // dB from the middle: turquoise up, orange down (no numbers: the client reads the bars against their scale)
        line(dc, mid, well.top - px(2), mid, well.bottom + px(2), kDim);
        if (known && on) {
            const int xv = xDb(val);
            if (xv != mid) fill(dc, RECT{std::min(mid, xv), well.top, std::max(mid, xv), well.bottom}, val > 0 ? kTeal : kOrange);
        }
        if (!on && !(known && std::fabs(val) >= 0.05)) text(dc, (barL + mid) / 2, wy - th / 2, L"off", kDim, TA_CENTER);
    }
}

} // namespace ap
