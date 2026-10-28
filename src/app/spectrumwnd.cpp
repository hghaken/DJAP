// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectrumwnd.h"

#include "appstate.h"
#include "colour.h"
#include "placement.h"
#include "resource.h"
#include "theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ap {

namespace {

const wchar_t* const kClass = L"APSpectrumWindow";

// the meter panel's palette (meterview.cpp)
const COLORREF kBack = RGB(28, 30, 33), kText = RGB(214, 218, 224), kDim = RGB(120, 126, 134), kWell = RGB(44, 47, 52),
               kSelected = RGB(62, 98, 140), kEdge = RGB(70, 74, 80);

enum Tip { tGraph, tDecks, tSnap, tResetPreset, tResetColour, kTips };
enum BoxId { idLoudnessBox = 201, idColourBox, idBass0 = 210 };
const wchar_t* const kTipText[kTips] = {
    L"The deck's spectrum, 20 Hz - 20 kHz (left scale, dB): orange = the original (the deck's input), turquoise = "
    L"processed (the output). Purple: the density stage's bands; a line per band at its gain now (right scale; "
    L"blue = lifted, red = turned down). Yellow: the deck's colour as an EQ curve (right scale, dB) - thick: what it does now, the auto "
    L"tone's correction included; dashed: without the auto tone; thin: each tone section. Drag a point: left-right "
    L"its frequency, up-down its gain; double-click: back to 0 dB. It changes the colour preset (every deck that uses "
    L"it), heard at once and saved when you let go. A purple handle per density band: its fixed gain (the loudness "
    L"preset's Band N gain, -12 .. +12 dB, right-hand scale) - drag it up or down; double-click: 0 dB.\n\nThe blue "
    L"bass point is the bass sliders' Bass level (up-down) and Bass frequency (left-right; the enhancer and the width "
    L"follow).",
    L"The deck whose spectrum and colour are shown.",
    L"Snap to bands (lit): a colour point dragged near a density band split sits exactly on it, so a tone section "
    L"starts where a density band does.",
    L"Reset loudness: every value of the deck's loudness preset back - a shipped one to its shipped values, your own "
    L"to the generic defaults - as the loudness window's button does (asks first). The density band handles go back with it.",
    L"Reset colour: every value of the deck's colour back - a shipped colour to its shipped values, your own to neutral "
    L"- as the colour window's button does (asks first). The coloured points go back with it.",
};

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

std::wstring hzText(double f) {
    wchar_t b[24];
    if (f < 1000.0) swprintf(b, 24, L"%.0f Hz", f);
    else swprintf(b, 24, L"%.2f kHz", f / 1000.0);
    return b;
}

} // namespace

SpectrumWindow::SpectrumWindow(Settings& settings, const AudioIO& audio, Changed onChange)
    : settings_(settings), audio_(audio), onChange_(std::move(onChange)) {}

SpectrumWindow::~SpectrumWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void SpectrumWindow::show(HWND owner, int deck) {
    deck_ = std::max(0, std::min(settings_.decks - 1, deck));
    if (!hwnd_) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
            wc.lpszClassName = kClass;
            registered = RegisterClassExW(&wc) != 0;
        }
        hwnd_ = CreateWindowExW(0, kClass, L"Spectrum", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, owner, nullptr, inst, this);
        if (!hwnd_) return;
        RECT r{0, 0, px(1180), px(640)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        placement::remember(hwnd_, "Spectrum", owner);
    }
    setTitle();
    layoutBar();
    fillBoxes();
    ShowWindow(hwnd_, placement::showCommand(hwnd_));
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::refresh() {
    if (!hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    if (deck_ >= settings_.decks) deck_ = 0; // one channel now: deck A
    if (layoutDecks_ != settings_.decks) layoutBar();
    setTitle(); // the deck's preset or colour may have been chosen elsewhere
    fillBoxes();
    fillSliders();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::setTitle() {
    if (!hwnd_) return;
    const std::wstring deck = settings_.decks == 1 ? L"Channel" : deck_ == 0 ? L"Deck A" : L"Deck B";
    const std::wstring title = L"Spectrum: " + deck + L" (loudness " + fromUtf8(settings_.deck[deck_].preset) +
                               L", colour " + fromUtf8(settings_.deck[deck_].colour) + L")";
    if (title == title_) return; // only when it changed (refresh() asks 20 times a second)
    title_ = title;
    SetWindowTextW(hwnd_, title.c_str());
}

Preset* SpectrumWindow::colour() { return settings_.find(settings_.deck[deck_].colour, PresetKind::Colour); }

Preset* SpectrumWindow::preset() { return settings_.find(settings_.deck[deck_].preset); }

spectrum::Data SpectrumWindow::data() const {
    spectrum::Data d;
    const DeckMeters& m = audio_.meters(deck_);
    const DeckAnalysis& an = audio_.analysis(deck_);
    const PresetValues v = settings_.deckValues(deck_);
    const bool live = audio_.audioActive() && m.live;
    d.in = an.specIn.points();
    d.out = an.specOut.points();
    d.live = live && an.specOut.frames() > 0;
    d.densityOn = settings_.deck[deck_].processing && v[pDensOn] >= 0.5;
    d.bands = m.bands > 0 ? m.bands : static_cast<int>(v[pDensBands]);
    for (int k = 0; k < kMaxSplits; ++k) d.splits[k] = v[static_cast<size_t>(pDensSplit1 + k)];
    for (int k = 0; k < kMaxBands; ++k) d.densityDb[k] = live ? m.densityDb[k] : 0.0;
    d.colour = true;
    d.values = v;
    for (int k = 0; k < 4; ++k) d.autoDb[k] = live ? m.autoToneDb[k] : 0.0;
    d.fs = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
    d.hotNode = dragging_ >= 0 ? dragging_ : hot_;
    d.bandHandles = d.densityOn;
    for (int k = 0; k < kMaxBands; ++k) d.bandGainDb[k] = v[static_cast<size_t>(pDensGain1 + k)];
    d.hotBand = draggingBand_ >= 0 ? draggingBand_ : hot_ >= 0 ? -1 : hotBand_;
    d.snapSplit = dragging_ >= 0 ? snapSplit_ : -1;
    return d;
}

LRESULT CALLBACK SpectrumWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SpectrumWindow* self = reinterpret_cast<SpectrumWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<SpectrumWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
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

int SpectrumWindow::buttonAt(POINT pt) const {
    for (int b = 0; b < kButtons; ++b)
        if (PtInRect(&button_[b], pt)) return b;
    return -1;
}

void SpectrumWindow::hover(POINT pt) {
    const int hot = geo_.nodeAt(pt, px(10));
    const int band = hot >= 0 ? -1 : geo_.handleAt(pt, px(6));
    mouse_ = PtInRect(&geo_.plot, pt) ? pt : POINT{-1, -1};
    if (hot != hot_ || band != hotBand_ || mouse_.x >= 0) {
        hot_ = hot;
        hotBand_ = band;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SpectrumWindow::dragBand(POINT pt) {
    Preset* p = preset();
    if (!p || draggingBand_ < 0) return;
    const int gp = pDensGain1 + draggingBand_;
    const double g = paramValueAt(gp, paramPositionOf(gp, geo_.eqAt(pt.y)));
    if (g == p->values[static_cast<size_t>(gp)]) return;
    p->values[static_cast<size_t>(gp)] = g;
    if (onChange_) onChange_(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::reset(bool colour) {
    const std::string& name = colour ? settings_.deck[deck_].colour : settings_.deck[deck_].preset;
    const PresetKind kind = colour ? PresetKind::Colour : PresetKind::Dynamics;
    const Preset* p = settings_.find(name, kind);
    if (!p) return;
    const std::wstring q = L"Set every value of \"" + fromUtf8(name) + L"\" back to " +
                           (p->shipped ? L"its shipped values?" : colour ? L"neutral?" : L"the generic defaults?");
    if (placement::messageBox(hwnd_, q.c_str(), colour ? L"Reset colour" : L"Reset loudness",
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    settings_.resetPreset(name, kind);
    if (onChange_) onChange_(true); // heard at once, saved; the preset / colour window and the sliders follow
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::layoutBar() {
    if (!hwnd_ || !boxes_[0]) return;
    layoutDecks_ = settings_.decks;
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    auto width = [&](const wchar_t* s) {
        SIZE sz;
        GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
        return static_cast<int>(sz.cx);
    };
    const int m = px(12), barH = px(30);
    int x = m;
    if (settings_.decks == 2) x += width(L"Deck") + px(8) + 2 * (width(L"A") + px(20)) + px(24); // as paint() draws them
    decksRight_ = x;
    const int boxW[2] = {px(170), px(140)};
    const wchar_t* const labels[2] = {L"Loudness", L"Colour"};
    for (int k = 0; k < 2; ++k) {
        boxLabelX_[k] = x;
        x += width(labels[k]) + px(8);
        MoveWindow(boxes_[k], x, m + px(2), boxW[k], px(300), TRUE);
        x += boxW[k] + px(16);
    }
    buttonsX_ = x + px(8);
    // the second row: the four bass sliders over the width, each name, trackbar, value
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int colW = (static_cast<int>(rc.right) - 2 * m) / bass::kSliders, nameW = width(L"Bass frequency") + px(10),
              valueW = width(L"100 % (off)") + px(6);
    bassRowY_ = m + barH + px(8);
    for (int k = 0; k < bass::kSliders; ++k) {
        const int cx = m + k * colW;
        bassLabelX_[k] = cx;
        bassValueX_[k] = cx + colW - px(12);
        MoveWindow(bass_[k], cx + nameW, bassRowY_, std::max(px(40), colW - nameW - valueW - px(12)), barH, TRUE);
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    (void)barH;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::fillBoxes() {
    if (!boxes_[0]) return;
    filling_ = true;
    for (int k = 0; k < 2; ++k) {
        const std::vector<Preset>& list = k == 0 ? settings_.presets : settings_.colours;
        std::vector<std::string> names;
        for (const Preset& p : list) names.push_back(p.name);
        if (names != boxNames_[k]) {
            SendMessageW(boxes_[k], CB_RESETCONTENT, 0, 0);
            for (const std::string& n : names) SendMessageW(boxes_[k], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(n).c_str()));
            boxNames_[k] = names;
        }
        const std::string& chosen = k == 0 ? settings_.deck[deck_].preset : settings_.deck[deck_].colour;
        LRESULT want = -1;
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == chosen) want = static_cast<LRESULT>(i);
        if (SendMessageW(boxes_[k], CB_GETCURSEL, 0, 0) != want && SendMessageW(boxes_[k], CB_GETDROPPEDSTATE, 0, 0) == 0)
            SendMessageW(boxes_[k], CB_SETCURSEL, static_cast<WPARAM>(want), 0);
    }
    filling_ = false;
}

void SpectrumWindow::fillSliders() {
    const PresetValues v = settings_.deckValues(deck_);
    for (int k = 0; k < bass::kSliders; ++k) {
        if (!bass_[k] || GetCapture() == bass_[k]) continue; // dragged now: it shows its own position
        const int pos = bass::position(v, k);
        if (SendMessageW(bass_[k], TBM_GETPOS, 0, 0) != pos) SendMessageW(bass_[k], TBM_SETPOS, TRUE, pos);
    }
}

void SpectrumWindow::choose(bool colour) {
    const LRESULT sel = SendMessageW(boxes_[colour ? 1 : 0], CB_GETCURSEL, 0, 0);
    const std::vector<Preset>& list = colour ? settings_.colours : settings_.presets;
    if (sel < 0 || static_cast<size_t>(sel) >= list.size()) return;
    const std::string name = list[static_cast<size_t>(sel)].name;
    for (int d = 0; d < 2; ++d) // the deck shown; both when the presets are linked
        if (d == deck_ || (settings_.linkPresets && settings_.decks == 2))
            (colour ? settings_.deck[d].colour : settings_.deck[d].preset) = name;
    if (onChange_) onChange_(true); // heard at once, saved; the main window's lists follow
    setTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::endDrag() {
    const bool was = dragging_ >= 0 || draggingBand_ >= 0;
    dragging_ = draggingBand_ = snapSplit_ = -1;
    if (was && onChange_) onChange_(true); // saved when the mouse is let go
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::drag(POINT pt) {
    Preset* c = colour();
    if (!c || dragging_ < 0) return;
    const int fp = spectrum::kNodeFreq[dragging_], gp = spectrum::kNodeGain[dragging_];
    // the parameters' own steps (frequencies on their log scale), within their ranges; with Snap to bands, on a
    // band split when the mouse is near it (and the split lies within the section's range)
    double f = paramValueAt(fp, paramPositionOf(fp, geo_.hzAt(pt.x)));
    snapSplit_ = -1;
    if (settings_.snapToBands) {
        const PresetValues v = settings_.deckValues(deck_);
        const ParamInfo& in = paramInfo(fp);
        int best = px(10);
        for (int k = 0; k < geo_.splits; ++k) {
            const double split = v[static_cast<size_t>(pDensSplit1 + k)];
            const int dist = std::abs(static_cast<int>(pt.x) - geo_.splitX[k]);
            if (dist <= best && split >= in.min && split <= in.max) {
                best = dist;
                f = split;
                snapSplit_ = k;
            }
        }
    }
    const double g = paramValueAt(gp, paramPositionOf(gp, geo_.eqAt(pt.y)));
    if (dragging_ == 0) { // the bass point is the bass sliders' level and frequency (the enhancer and width follow)
        const PresetValues v = settings_.deckValues(deck_);
        const int posF = static_cast<int>(std::lround(f / 5.0)), posL = static_cast<int>(std::lround(g * 2.0));
        if (posF == bass::position(v, bass::kFreq) && posL == bass::position(v, bass::kLevel)) return;
        bass::apply(settings_, deck_, bass::kFreq, posF);
        bass::apply(settings_, deck_, bass::kLevel, posL);
        if (onChange_) onChange_(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (f == c->values[static_cast<size_t>(fp)] && g == c->values[static_cast<size_t>(gp)]) return;
    c->values[static_cast<size_t>(fp)] = f;
    c->values[static_cast<size_t>(gp)] = g;
    if (onChange_) onChange_(false); // heard at once; saved when the mouse is let go
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT SpectrumWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        setFonts();
        tips_.create(hwnd_);
        for (int t = 0; t < kTips; ++t) tips_.addRect(hwnd_, static_cast<UINT_PTR>(t), kTipText[t]);
        for (int k = 0; k < 2; ++k) {
            boxes_[k] = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST,
                                        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(idLoudnessBox + k)),
                                        GetModuleHandleW(nullptr), nullptr);
            theme::makeCombo(boxes_[k]);
            SendMessageW(boxes_[k], WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        }
        tips_.add(boxes_[0], L"The deck's loudness preset: its target loudness and every stage's values (as the main "
                             L"window's list; both decks when they are linked).");
        tips_.add(boxes_[1], L"The deck's colour: tone, auto tone, sub bass and contour (as the main window's list; both "
                             L"decks when they are linked).");
        for (int k = 0; k < bass::kSliders; ++k) {
            bass_[k] = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS | TBS_BOTTOM,
                                       0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(idBass0 + k)),
                                       GetModuleHandleW(nullptr), nullptr);
            bass::setup(bass_[k], k);
            tips_.add(bass_[k], (std::wstring(bass::kTip[k]) + L"\n\nAs the main window's slider: every deck that uses "
                                                                L"the same loudness preset or colour changes too.").c_str());
        }
        if (!backBrush_) backBrush_ = CreateSolidBrush(kBack);
        boxNames_[0].clear();
        boxNames_[1].clear();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        setFonts();
        tips_.setDpi(dpi_);
        for (HWND b : boxes_)
            if (b) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        layoutBar();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(760), px(440)};
        return 0;
    case WM_SIZE:
        layoutBar(); // the bass sliders spread over the width
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_CTLCOLORSTATIC: // the trackbars on the dark background
        if (backBrush_) return reinterpret_cast<LRESULT>(backBrush_);
        break;
    case WM_HSCROLL: {
        const HWND bar = reinterpret_cast<HWND>(lp);
        for (int k = 0; k < bass::kSliders; ++k)
            if (bar && bar == bass_[k]) {
                const int pos = static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0));
                if (bass::apply(settings_, deck_, k, pos) && onChange_)
                    onChange_(LOWORD(wp) != TB_THUMBTRACK); // heard while dragging, saved when let go (keys: at once)
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        return 0;
    }
    case WM_COMMAND:
        if ((LOWORD(wp) == idLoudnessBox || LOWORD(wp) == idColourBox) && HIWORD(wp) == CBN_SELCHANGE && !filling_)
            choose(LOWORD(wp) == idColourBox);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (dragging_ >= 0 || geo_.nodeAt(pt, px(10)) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        if (draggingBand_ >= 0 || geo_.handleAt(pt, px(6)) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
        if (buttonAt(pt) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (dragging_ >= 0) drag(pt);
        else if (draggingBand_ >= 0) dragBand(pt);
        else hover(pt);
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (dragging_ < 0 && draggingBand_ < 0) {
            hot_ = hotBand_ = -1;
            mouse_ = {-1, -1};
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int b = buttonAt(pt);
        if (b == bDeckA || b == bDeckB) {
            deck_ = std::min(settings_.decks - 1, b == bDeckA ? 0 : 1);
            setTitle();
            fillBoxes();
            fillSliders();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (b == bResetPreset || b == bResetColour) {
            reset(b == bResetColour);
        } else if (b == bSnap) {
            settings_.snapToBands = !settings_.snapToBands;
            if (onChange_) onChange_(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            const int node = colour() ? geo_.nodeAt(pt, px(10)) : -1;
            const int band = node < 0 && preset() ? geo_.handleAt(pt, px(6)) : -1;
            if (node >= 0) dragging_ = node;
            else if (band >= 0) draggingBand_ = band;
            if (node >= 0 || band >= 0) SetCapture(hwnd_);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging_ >= 0 || draggingBand_ >= 0) {
            ReleaseCapture(); // WM_CAPTURECHANGED ends the drag
            if (dragging_ >= 0 || draggingBand_ >= 0) endDrag();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging_ >= 0 || draggingBand_ >= 0) endDrag();
        return 0;
    case WM_LBUTTONDBLCLK: { // a point or a band handle: its gain back to 0 dB
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int n = geo_.nodeAt(pt, px(10));
        const int band = n < 0 ? geo_.handleAt(pt, px(6)) : -1;
        Preset* c = colour();
        Preset* p = preset();
        if (n >= 0 && c) c->values[static_cast<size_t>(spectrum::kNodeGain[n])] = 0.0;
        else if (band >= 0 && p) p->values[static_cast<size_t>(pDensGain1 + band)] = 0.0;
        if ((n >= 0 && c) || (band >= 0 && p)) {
            if (onChange_) onChange_(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
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
        if (backBrush_) DeleteObject(backBrush_);
        backBrush_ = nullptr;
        if (dragging_ >= 0 || draggingBand_ >= 0) ReleaseCapture();
        dragging_ = hot_ = draggingBand_ = hotBand_ = snapSplit_ = -1;
        for (HFONT* f : {&font_, &small_}) {
            if (*f) DeleteObject(*f);
            *f = nullptr;
        }
        for (RECT& r : tipRect_) r = RECT{};
        title_.clear();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void SpectrumWindow::setFonts() {
    for (HFONT* f : {&font_, &small_})
        if (*f) DeleteObject(*f);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 92 / 100;
    small_ = CreateFontIndirectW(&lf);
}

void SpectrumWindow::paint(HDC target) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, rc, kBack);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int th = tm.tmHeight, m = px(12), barH = px(30);

    // ---- the bar: deck, density view, read-out ----
    auto drawButton = [&](int b, int x, const wchar_t* label, bool on, bool shown) {
        if (!shown) {
            button_[b] = RECT{};
            return x;
        }
        SIZE sz;
        GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &sz);
        const RECT r{x, m, x + sz.cx + px(20), m + barH};
        fill(dc, r, on ? kSelected : kWell);
        frame(dc, r, kEdge);
        SetTextColor(dc, on ? RGB(255, 255, 255) : kText);
        SetTextAlign(dc, TA_CENTER | TA_TOP);
        TextOutW(dc, (r.left + r.right) / 2, r.top + (barH - th) / 2, label, static_cast<int>(wcslen(label)));
        button_[b] = r;
        return static_cast<int>(r.right);
    };
    auto label = [&](int x, const wchar_t* s) {
        SetTextColor(dc, kDim);
        SetTextAlign(dc, TA_LEFT | TA_TOP);
        TextOutW(dc, x, m + (barH - th) / 2, s, static_cast<int>(wcslen(s)));
        SIZE sz;
        GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
        return x + sz.cx + px(8);
    };
    const bool two = settings_.decks == 2;
    int x = m;
    if (two) {
        x = label(x, L"Deck");
        x = drawButton(bDeckA, x, L"A", deck_ == 0, true);
        x = drawButton(bDeckB, x, L"B", deck_ == 1, true) + px(24);
    } else {
        button_[bDeckA] = button_[bDeckB] = RECT{};
    }
    const int decksRight = x;
    label(boxLabelX_[0], L"Loudness");
    label(boxLabelX_[1], L"Colour");
    x = std::max(x, buttonsX_);
    const int snapLeft = x;
    x = drawButton(bSnap, x, L"Snap to bands", settings_.snapToBands, true);
    const int snapRight = x;
    const int resetPresetLeft = x + px(24);
    x = drawButton(bResetPreset, resetPresetLeft, L"Reset loudness", false, true);
    const int resetColourLeft = x + px(8);
    const int resetPresetRight = x;
    x = drawButton(bResetColour, resetColourLeft, L"Reset colour", false, true);
    const int resetColourRight = x;

    // ---- the graph ----
    const int hintH = th + px(10);
    // the bass sliders' names and values (their trackbars are child windows)
    {
        const PresetValues v = settings_.deckValues(deck_);
        for (int k = 0; k < bass::kSliders; ++k) {
            SetTextColor(dc, kDim);
            SetTextAlign(dc, TA_LEFT | TA_TOP);
            TextOutW(dc, bassLabelX_[k], bassRowY_ + (barH - th) / 2, bass::kName[k], static_cast<int>(wcslen(bass::kName[k])));
            const std::wstring val = bass::text(v, k);
            SetTextColor(dc, kText);
            SetTextAlign(dc, TA_RIGHT | TA_TOP);
            TextOutW(dc, bassValueX_[k], bassRowY_ + (barH - th) / 2, val.c_str(), static_cast<int>(val.size()));
        }
    }
    const RECT graph{m, bassRowY_ + barH + px(10), W - m, H - hintH - px(4)};
    const spectrum::Data d = data();
    geo_ = spectrum::paint(dc, graph, d, small_, dpi_, true);

    // ---- read-out (right of the bar): the point being dragged or under the mouse, or the frequency under it ----
    std::wstring read;
    const Preset* c = colour();
    const int node = dragging_ >= 0 ? dragging_ : hot_;
    if (node >= 0 && c) {
        wchar_t b[96];
        swprintf(b, 96, L"%ls: %ls, %+.1f dB", spectrum::nodeName(node),
                 hzText(c->values[static_cast<size_t>(spectrum::kNodeFreq[node])]).c_str(),
                 c->values[static_cast<size_t>(spectrum::kNodeGain[node])]);
        read = b;
        if (snapSplit_ >= 0 && dragging_ >= 0) {
            wchar_t s[48];
            swprintf(s, 48, L"  (on the split of bands %d and %d)", snapSplit_ + 1, snapSplit_ + 2);
            read += s;
        }
    } else if ((draggingBand_ >= 0 || hotBand_ >= 0) && preset()) {
        const int band = draggingBand_ >= 0 ? draggingBand_ : hotBand_;
        wchar_t b[128];
        swprintf(b, 128, L"Density band %d: fixed gain %+.1f dB, now %+.1f dB", band + 1, d.bandGainDb[band],
                 d.densityDb[band]);
        read = b;
    } else if (mouse_.x >= 0) {
        const double f = geo_.hzAt(mouse_.x);
        int i = 0; // the nearest spectrum point
        for (int k = 1; k < SpectrumAnalyzer::kPoints; ++k)
            if (std::fabs(std::log(SpectrumAnalyzer::pointHz(k) / f)) < std::fabs(std::log(SpectrumAnalyzer::pointHz(i) / f)))
                i = k;
        wchar_t b[160];
        if (d.live)
            swprintf(b, 160, L"%ls   original %.1f dB   processed %.1f dB   colour %+.1f dB", hzText(f).c_str(),
                     d.in[static_cast<size_t>(i)], d.out[static_cast<size_t>(i)], colourCurveDb(d.values, d.autoDb, d.fs, f));
        else
            swprintf(b, 160, L"%ls   colour %+.1f dB", hzText(f).c_str(), colourCurveDb(d.values, d.autoDb, d.fs, f));
        read = b;
    }
    if (!read.empty()) {
        // on the graph's title row, at the right (the bar is full with the lists and buttons)
        HGDIOBJ f = SelectObject(dc, small_);
        SetTextColor(dc, kText);
        SetTextAlign(dc, TA_RIGHT | TA_TOP);
        TextOutW(dc, geo_.plot.right, graph.top + px(2), read.c_str(), static_cast<int>(read.size()));
        SelectObject(dc, f);
    }

    // ---- hint ----
    const std::wstring hint = c ? L"Drag a point: the colour \"" + fromUtf8(c->name) +
                                      L"\" (blue: bass level and frequency). A purple handle: a density band of \"" +
                                      fromUtf8(settings_.deck[deck_].preset) + L"\". Every deck using them changes; "
                                      L"double-click resets."
                                : L"This deck has no colour.";
    SetTextColor(dc, kDim);
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    TextOutW(dc, m, H - hintH, hint.c_str(), static_cast<int>(hint.size()));

    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);

    // tooltips on the graph and the two groups of buttons (moved with the layout)
    const RECT tips[kTips] = {graph, two ? RECT{m, m, decksRight, m + barH} : RECT{}, RECT{snapLeft, m, snapRight, m + barH},
                              RECT{resetPresetLeft, m, resetPresetRight, m + barH},
                              RECT{resetColourLeft, m, resetColourRight, m + barH}};
    for (int t = 0; t < kTips; ++t)
        if (!EqualRect(&tips[t], &tipRect_[t])) {
            tipRect_[t] = tips[t];
            tips_.setRect(hwnd_, static_cast<UINT_PTR>(t), tips[t]);
        }
}

} // namespace ap
