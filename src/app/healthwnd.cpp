// SPDX-License-Identifier: GPL-3.0-or-later
#include "healthwnd.h"

#include "placement.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

namespace ap {

namespace {

const wchar_t* const kClass = L"APHealthWindow";
const wchar_t* const kGraphClass = L"APHealthGraph";
enum Id { idSpan = 10, idReset, idGraph };

// the meter panel's palette (meterview.cpp)
const COLORREF kBack = RGB(28, 30, 33), kGrid = RGB(62, 66, 72), kText = RGB(214, 218, 224), kDim = RGB(120, 126, 134),
               kGreen = RGB(70, 190, 110), kAmber = RGB(235, 170, 60), kRed = RGB(230, 70, 60),
               kBlue = RGB(80, 160, 235), kOrange = RGB(240, 130, 50), kTeal = RGB(80, 215, 195),
               kPurple = RGB(175, 145, 245), kPink = RGB(240, 120, 190), kWhite = RGB(240, 240, 240);

struct SpanChoice {
    const wchar_t* name;
    int seconds, tick; // the whole width, a grid line every `tick` seconds
};
const SpanChoice kSpans[] = {{L"Last minute", 60, 10}, {L"Last 10 minutes", 600, 120}, {L"Last hour", 3600, 600}};

const wchar_t* const kPanelTips[] = {
    L"Processing load per second, in % of a period's time. Peak: the heaviest period of that second - at 100 % it was not "
    L"ready in time, a dropout. Average: the mean, the room left for the rest of the computer's work.",
    L"What went wrong, per second; beside each name the number in the time shown. Late: a period not ready in time (a "
    L"dropout). Over half: a period that needed more than half its time (a warning). Silence: a period answered with "
    L"silence, the engine was not ready. Underruns: the input fell behind. Overruns: input blocks dropped. Resyncs and "
    L"overloads: reported by the ASIO driver. A flat line at 0 is what you want.",
    L"How far the clock of an input of another audio API (or of another device) runs from the output's, in parts per "
    L"million; + = the input is faster. It stays about the same for hours; DJAP's drift correction follows it, a jump "
    L"means one of the clocks changed.",
    L"The drift buffer between such an input and its output, in ms. The thin line: the fill the loop holds, the latency it "
    L"adds. The thick line: the measured fill; it should stay close to the thin one."};

void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    const RECT rc{l, t, r, b};
    SetDCBrushColor(dc, c);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

int textWidth(HDC dc, const std::wstring& s) {
    SIZE z{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &z);
    return z.cx;
}

void text(HDC dc, int x, int y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT) {
    SetTextColor(dc, c);
    SetTextAlign(dc, align | TA_TOP);
    TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
}

std::wstring number(double v) {
    wchar_t b[32];
    swprintf(b, 32, std::fabs(v) >= 100.0 || v == std::floor(v) ? L"%.0f" : L"%.1f", v);
    return b;
}

// The next value of 1, 2, 5, 10, 20, 50, ... not under v.
double niceCeil(double v) {
    double step = 1.0;
    while (step * 10.0 < v) step *= 10.0;
    for (const double m : {1.0, 2.0, 5.0, 10.0})
        if (step * m >= v) return step * m;
    return step * 10.0;
}

struct Series {
    std::wstring name;
    COLORREF colour;
    int width; // 1 or 2 (at 96 DPI)
    bool peaks; // a screen column of several seconds shows the highest of them (else their mean)
    std::function<bool(const HealthPoint&, float&)> value;
    bool total = false; // the legend adds the sum over the time shown
};

enum class Scale { Load, Count, Free, Fill };

struct Panel {
    const wchar_t* title;
    Scale scale;
    std::vector<Series> series;
    const wchar_t* empty; // shown when no line has a value in the time shown
};

} // namespace

HealthWindow::HealthWindow(const HealthHistory& history, Source source) : history_(history), source_(std::move(source)) {}

HealthWindow::~HealthWindow() { close(); }

void HealthWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void HealthWindow::show(HWND owner) {
    if (!hwnd_) {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
            wc.lpszClassName = kClass;
            RegisterClassExW(&wc);
            WNDCLASSW gc{};
            gc.lpfnWndProc = graphProc;
            gc.hInstance = inst;
            gc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            gc.lpszClassName = kGraphClass;
            RegisterClassW(&gc);
            registered = true;
        }
        dpi_ = owner ? GetDpiForWindow(owner) : 96;
        if (dpi_ == 0) dpi_ = 96;
        RECT r{0, 0, px(980), px(780)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"Health", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, owner, nullptr, inst, this);
        if (!hwnd_) return;
        create();
        placement::remember(hwnd_, "Health", owner);
    }
    drawn_ = -1;
    refresh();
    ShowWindow(hwnd_, placement::showCommand(hwnd_));
    SetForegroundWindow(hwnd_);
}

LRESULT CALLBACK HealthWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* w = reinterpret_cast<HealthWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    if (!w->hwnd_) w->hwnd_ = hwnd; // during CreateWindowExW
    const LRESULT r = w->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        w->hwnd_ = w->state_ = w->counters_ = w->span_ = w->reset_ = w->graph_ = nullptr;
        w->stateShown_.clear();
        w->countersShown_.clear();
        w->stateLines_ = 1;
    }
    return r;
}

LRESULT CALLBACK HealthWindow::graphProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* w = reinterpret_cast<HealthWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (w && msg == WM_ERASEBKGND) return 1; // painted whole, through a bitmap
    if (w && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        w->paintGraphs(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HealthWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, result)) return result;
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == idReset && HIWORD(wp) == BN_CLICKED) {
            if (source_.reset) source_.reset();
        } else if (LOWORD(wp) == idSpan && HIWORD(wp) == CBN_SELCHANGE) {
            const LRESULT i = SendMessageW(span_, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < static_cast<LRESULT>(std::size(kSpans))) spanSeconds_ = kSpans[i].seconds;
            InvalidateRect(graph_, nullptr, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, px(760), px(560)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        setFonts();
        tips_.setDpi(dpi_);
        graphTips_.setDpi(dpi_);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        layout();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (font_) DeleteObject(font_);
        if (small_) DeleteObject(small_);
        font_ = small_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void HealthWindow::create() {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* t, DWORD style, int id, void* param = nullptr) {
        return CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, param);
    };
    state_ = make(L"STATIC", L"", SS_LEFT | SS_NOTIFY | SS_NOPREFIX, 0);
    counters_ = make(L"STATIC", L"", SS_LEFT | SS_NOTIFY | SS_NOPREFIX, 0);
    span_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idSpan);
    reset_ = make(L"BUTTON", L"Reset counters", BS_OWNERDRAW | WS_TABSTOP, idReset);
    graph_ = make(kGraphClass, L"", 0, idGraph, this);
    theme::makeButton(reset_);
    theme::makeCombo(span_);
    for (const SpanChoice& s : kSpans) SendMessageW(span_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.name));
    for (int i = 0; i < static_cast<int>(std::size(kSpans)); ++i)
        if (kSpans[i].seconds == spanSeconds_) SendMessageW(span_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    setFonts();
    tips_.create(hwnd_);
    tips_.setDpi(dpi_);
    tips_.add(state_, L"What runs now: sample rate and period, and per deck the latency from input to output - the "
                      L"device's input and output buffers plus the processing (the limiter look-ahead). With an "
                      L"input of another API also its drift buffer, the measured clock drift in ppm, and underruns "
                      L"(the input fell behind: a moment of silence) and overruns (input blocks dropped).");
    tips_.add(counters_, L"Periods: device periods processed. Heaviest / last: processing time in % of a period. Over "
                         L"half: periods that needed more than half their time (a warning). Late: periods not ready "
                         L"in time - a dropout. Answered with silence: the engine was not ready. Driver resyncs and "
                         L"overloads: reported by the ASIO driver.");
    tips_.add(span_, L"The time the graphs show. DJAP keeps the last hour, also while this window is closed.");
    tips_.add(reset_, L"Sets the counters back to zero. The graphs keep what they show.");
    graphTips_.create(graph_);
    graphTips_.setDpi(dpi_);
    for (int i = 0; i < kPanels; ++i) graphTips_.addRect(graph_, static_cast<UINT_PTR>(i), kPanelTips[i]);
    for (RECT& r : tipRect_) r = RECT{};
}

void HealthWindow::setFonts() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    if (font_) DeleteObject(font_);
    if (small_) DeleteObject(small_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 9 / 10;
    small_ = CreateFontIndirectW(&lf);
    for (HWND h : {state_, counters_, span_, reset_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void HealthWindow::layout() {
    if (!state_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int m = px(12), g = px(6), h = px(28), bw = px(112), spanW = px(150), W = rc.right - 2 * m;
    int lh = px(16);
    if (HDC dc = GetDC(hwnd_)) {
        HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        lh = std::max(1, static_cast<int>(tm.tmHeight + tm.tmExternalLeading));
        // the state as high as its text
        RECT r{0, 0, std::max(1, W), 0};
        DrawTextW(dc, stateShown_.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        stateLines_ = std::max(1, static_cast<int>((r.bottom + lh - 1) / lh));
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }
    int y = m;
    MoveWindow(state_, m, y, W, stateLines_ * lh, TRUE);
    y += stateLines_ * lh + g;
    MoveWindow(counters_, m, y, std::max(0, W - spanW - bw - 2 * g), 3 * lh, TRUE);
    MoveWindow(span_, m + W - bw - g - spanW, y, spanW, px(200), TRUE);
    MoveWindow(reset_, m + W - bw, y, bw, h, TRUE);
    y += std::max(3 * lh, h) + g;
    MoveWindow(graph_, m, y, W, std::max(0, static_cast<int>(rc.bottom) - y - m), TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void HealthWindow::refresh() {
    if (!hwnd_) return;
    const std::wstring state = source_.state ? source_.state() : std::wstring();
    if (state != stateShown_) {
        stateShown_ = state;
        SetWindowTextW(state_, state.c_str());
        layout(); // it may take another number of lines
    }
    const std::wstring counters = source_.counters ? source_.counters() : std::wstring();
    if (counters != countersShown_) {
        countersShown_ = counters;
        SetWindowTextW(counters_, counters.c_str());
    }
    if (history_.added() != drawn_) {
        drawn_ = history_.added();
        InvalidateRect(graph_, nullptr, FALSE);
    }
}

void HealthWindow::paintGraphs(HDC dc, const RECT& rc) {
    fill(dc, 0, 0, rc.right, rc.bottom, kBack);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, small_);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight;

    const bool two = !source_.decks || source_.decks() == 2;
    auto deckName = [two](int d) { return std::wstring(!two ? L"Channel" : d == 0 ? L"Deck A" : L"Deck B"); };
    auto running = [](float HealthPoint::*f) {
        return [f](const HealthPoint& p, float& v) {
            if (!p.running) return false;
            v = p.*f;
            return true;
        };
    };
    auto ofDeck = [](float (HealthPoint::*f)[2], int d) {
        return [f, d](const HealthPoint& p, float& v) {
            if (!p.running || !p.inputOn[d]) return false;
            v = (p.*f)[d];
            return true;
        };
    };
    std::vector<Panel> panels(kPanels);
    panels[0] = {L"Processing, % of a period's time", Scale::Load,
                 {{L"peak", kAmber, 1, true, running(&HealthPoint::peakPercent)},
                  {L"average", kGreen, 2, false, running(&HealthPoint::averagePercent)}},
                 L"Not running"};
    panels[1] = {L"Problems per second", Scale::Count,
                 {{L"over half", kAmber, 1, true, running(&HealthPoint::overHalf), true},
                  {L"silence", kPurple, 1, true, running(&HealthPoint::refused), true},
                  {L"underruns", kBlue, 1, true, running(&HealthPoint::underruns), true},
                  {L"overruns", kTeal, 1, true, running(&HealthPoint::overruns), true},
                  {L"resyncs", kWhite, 1, true, running(&HealthPoint::resyncs), true},
                  {L"overloads", kPink, 1, true, running(&HealthPoint::overloads), true},
                  {L"late", kRed, 2, true, running(&HealthPoint::late), true}},
                 L"Not running"};
    panels[2] = {L"Clock drift, ppm", Scale::Free, {}, L"No input with a drift buffer"};
    panels[3] = {L"Drift buffer, ms", Scale::Fill, {}, L"No input with a drift buffer"};
    const COLORREF deckColour[2] = {kTeal, kOrange};
    for (int d = 0; d < (two ? 2 : 1); ++d) {
        panels[2].series.push_back({deckName(d), deckColour[d], 2, false, ofDeck(&HealthPoint::driftPpm, d)});
        panels[3].series.push_back({deckName(d) + L" target", kDim, 1, false, ofDeck(&HealthPoint::targetMs, d)});
        panels[3].series.push_back({deckName(d), deckColour[d], 2, false, ofDeck(&HealthPoint::fillMs, d)});
    }

    const int span = spanSeconds_;
    const SpanChoice* choice = &kSpans[1];
    for (const SpanChoice& s : kSpans)
        if (s.seconds == span) choice = &s;
    const int gap = px(6), axisH = th + px(2);
    const int panelH = std::max(px(40), (static_cast<int>(rc.bottom) - axisH - (kPanels - 1) * gap) / kPanels);
    const int left = px(44), right = static_cast<int>(rc.right) - px(12);

    for (int pi = 0; pi < kPanels; ++pi) {
        const Panel& panel = panels[static_cast<size_t>(pi)];
        const int top = pi * (panelH + gap), bottom = top + panelH;
        const RECT tip{0, top, rc.right, bottom};
        if (!EqualRect(&tip, &tipRect_[pi])) {
            tipRect_[pi] = tip;
            graphTips_.setRect(graph_, static_cast<UINT_PTR>(pi), tip);
        }
        const int plotT = top + th + px(6), plotB = bottom - px(4), plotW = std::max(1, right - left);

        // the lines per screen column: a column of several seconds keeps the highest (a dropout never disappears) or
        // the mean
        const int cols = std::min(span, plotW);
        const int have = history_.size();
        std::vector<std::vector<float>> values(panel.series.size(), std::vector<float>(static_cast<size_t>(cols), 0.0f));
        std::vector<std::vector<char>> valid(panel.series.size(), std::vector<char>(static_cast<size_t>(cols), 0));
        std::vector<double> totals(panel.series.size(), 0.0);
        bool any = false;
        double lo = 0.0, hi = 0.0;
        for (size_t s = 0; s < panel.series.size(); ++s) {
            const Series& line = panel.series[s];
            for (int c = 0; c < cols; ++c) {
                const int a0 = static_cast<int>(static_cast<long long>(c) * span / cols);
                const int a1 = static_cast<int>(static_cast<long long>(c + 1) * span / cols);
                double sum = 0.0, top1 = 0.0;
                int n = 0;
                for (int a = a0; a < a1 && a < have; ++a) {
                    float v = 0.0f;
                    if (!line.value(history_.point(a), v)) continue;
                    sum += v;
                    top1 = n == 0 ? v : std::max(top1, static_cast<double>(v));
                    ++n;
                }
                if (n == 0) continue;
                totals[s] += sum;
                const double shown = line.peaks ? top1 : sum / n;
                values[s][static_cast<size_t>(c)] = static_cast<float>(shown);
                valid[s][static_cast<size_t>(c)] = 1;
                lo = any ? std::min(lo, shown) : shown;
                hi = any ? std::max(hi, shown) : shown;
                any = true;
            }
        }

        // the scale
        double yLo = 0.0, yHi = 1.0;
        switch (panel.scale) {
        case Scale::Load:
            yHi = std::max(10.0, niceCeil(hi));
            break;
        case Scale::Count:
            yHi = std::max(1.0, niceCeil(hi));
            break;
        case Scale::Fill:
            yHi = std::max(5.0, std::ceil(hi * 1.15 / 5.0) * 5.0);
            break;
        case Scale::Free: {
            const double mid = (lo + hi) / 2.0, half = std::max(1.0, (hi - lo) * 0.6);
            const double unit = half > 5.0 ? 5.0 : 1.0;
            yLo = std::floor((mid - half) / unit) * unit;
            yHi = std::ceil((mid + half) / unit) * unit;
            break;
        }
        }
        auto yOf = [&](double v) {
            const double t = (std::min(yHi, std::max(yLo, v)) - yLo) / (yHi - yLo);
            return plotB - static_cast<int>(t * (plotB - plotT) + 0.5);
        };

        // the grid: a line per tick of time, three of value
        for (int t = choice->tick; t < span; t += choice->tick) {
            const int x = right - static_cast<int>(static_cast<long long>(t) * plotW / span);
            fill(dc, x, plotT, x + 1, plotB, kGrid);
        }
        for (int k = 0; k <= 2; ++k) {
            const double v = yLo + (yHi - yLo) * k / 2.0;
            if (panel.scale == Scale::Count && v != std::floor(v)) continue; // counts: whole numbers
            const int y = yOf(v);
            fill(dc, left, y, right, y + 1, kGrid);
            text(dc, left - px(5), y - th / 2, number(v), kDim, TA_RIGHT);
        }
        if (panel.scale == Scale::Load) { // the warning and the dropout
            if (yHi >= 50.0) fill(dc, left, yOf(50.0), right, yOf(50.0) + 1, kAmber);
            if (yHi >= 100.0) fill(dc, left, yOf(100.0), right, yOf(100.0) + 1, kRed);
        }

        // the title and the legend
        SelectObject(dc, font_);
        text(dc, left, top, panel.title, kText);
        int lx = left + textWidth(dc, panel.title) + px(14);
        SelectObject(dc, small_);
        for (size_t s = 0; s < panel.series.size(); ++s) {
            const Series& line = panel.series[s];
            std::wstring name = line.name;
            if (line.total) name += L" " + number(totals[s]);
            fill(dc, lx, top + th / 2, lx + px(10), top + th / 2 + std::max(1, px(line.width)), line.colour);
            lx += px(14);
            text(dc, lx, top + px(1), name, line.total && totals[s] > 0.0 ? line.colour : kDim);
            lx += textWidth(dc, name) + px(10);
        }

        // the lines; a gap where there is no value
        if (!any) {
            text(dc, (left + right) / 2, (plotT + plotB - th) / 2, panel.empty, kDim, TA_CENTER);
            continue;
        }
        for (size_t s = 0; s < panel.series.size(); ++s) {
            const Series& line = panel.series[s];
            HPEN pen = CreatePen(PS_SOLID, std::max(1, px(line.width)), line.colour);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            std::vector<POINT> run;
            auto flush = [&] {
                if (run.size() == 1) { // one second between two gaps: a dot
                    const POINT& p = run[0];
                    fill(dc, p.x - px(1), p.y - px(1), p.x + px(1) + 1, p.y + px(1) + 1, line.colour);
                } else if (run.size() > 1) {
                    Polyline(dc, run.data(), static_cast<int>(run.size()));
                }
                run.clear();
            };
            for (int c = 0; c < cols; ++c) {
                if (!valid[s][static_cast<size_t>(c)]) {
                    flush();
                    continue;
                }
                const int x = right - static_cast<int>((c + 0.5) * plotW / cols);
                run.push_back({x, yOf(values[s][static_cast<size_t>(c)])});
            }
            flush();
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
    }

    // the time, under the last panel
    const int axisY = kPanels * (panelH + gap) - gap + px(1);
    text(dc, right, axisY, L"now", kDim, TA_RIGHT);
    for (int t = choice->tick; t <= span; t += choice->tick) {
        wchar_t b[24];
        if (span < 300) swprintf(b, 24, L"-%d s", t);
        else swprintf(b, 24, L"-%d min", t / 60);
        const int x = right - static_cast<int>(static_cast<long long>(t) * (right - left) / span);
        text(dc, x, axisY, b, kDim, t == span ? TA_LEFT : TA_CENTER);
    }
    SelectObject(dc, oldFont);
}

} // namespace ap
