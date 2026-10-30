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
HINSTANCE g_module = nullptr; // the program, or the plug-in DLL (setModule)
bool g_registered = false;
HINSTANCE moduleInstance() { return g_module ? g_module : GetModuleHandleW(nullptr); }

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
    L"", // Reset loudness, Reset colour: the source's (SpectrumSource::resetTip)
    L"",
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

// ---- the program's decks ----

std::wstring SettingsSpectrum::deckName(int d) const {
    return settings_.decks == 1 ? L"Channel" : d == 0 ? L"Deck A" : L"Deck B";
}

std::vector<std::string> SettingsSpectrum::names(bool colour) const {
    std::vector<std::string> n;
    for (const Preset& p : colour ? settings_.colours : settings_.presets) n.push_back(p.name);
    return n;
}

int SettingsSpectrum::chosen(int d, bool colour) const {
    const std::vector<Preset>& list = colour ? settings_.colours : settings_.presets;
    const std::string name = chosenName(d, colour);
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string SettingsSpectrum::chosenName(int d, bool colour) const {
    return colour ? settings_.deck[d].colour : settings_.deck[d].preset;
}

void SettingsSpectrum::choose(int d, bool colour, int index) {
    const std::vector<Preset>& list = colour ? settings_.colours : settings_.presets;
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    const std::string name = list[static_cast<size_t>(index)].name;
    for (int k = 0; k < 2; ++k) // the deck shown; both when the presets are linked
        if (k == d || (settings_.linkPresets && settings_.decks == 2))
            (colour ? settings_.deck[k].colour : settings_.deck[k].preset) = name;
    if (changed_) changed_(true); // heard at once, saved; the main window's lists follow
}

bool SettingsSpectrum::editable(int d, bool colour) const {
    return settings_.find(chosenName(d, colour), colour ? PresetKind::Colour : PresetKind::Dynamics) != nullptr;
}

void SettingsSpectrum::set(int d, const std::vector<std::pair<int, double>>& values, bool save) {
    Preset* p = settings_.find(settings_.deck[d].preset);
    Preset* c = settings_.find(settings_.deck[d].colour, PresetKind::Colour);
    for (const auto& [param, value] : values) {
        Preset* to = isColourParam(param) ? c : p;
        if (to) to->values[static_cast<size_t>(param)] = value;
    }
    if (changed_) changed_(save); // (no values: the end of a drag - saved now)
}

void SettingsSpectrum::setBass(int d, int slider, int pos, bool save) {
    if (bass::apply(settings_, d, slider, pos) && changed_) changed_(save);
}

void SettingsSpectrum::reset(int d, bool colour, HWND owner) {
    const std::string name = chosenName(d, colour);
    const PresetKind kind = colour ? PresetKind::Colour : PresetKind::Dynamics;
    const Preset* p = settings_.find(name, kind);
    if (!p) return;
    const std::wstring q = L"Set every value of \"" + fromUtf8(name) + L"\" back to " +
                           (p->shipped ? L"its shipped values?" : colour ? L"neutral?" : L"the generic defaults?");
    if (placement::messageBox(owner, q.c_str(), colour ? L"Reset colour" : L"Reset loudness",
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    settings_.resetPreset(name, kind);
    if (changed_) changed_(true); // heard at once, saved; the preset / colour window and the sliders follow
}

const wchar_t* SettingsSpectrum::resetTip(bool colour) const {
    return colour ? L"Reset colour: every value of the deck's colour back - a shipped colour to its shipped values, your "
                    L"own to neutral - as the colour window's button does (asks first). The coloured points go back with it."
                  : L"Reset loudness: every value of the deck's loudness preset back - a shipped one to its shipped "
                    L"values, your own to the generic defaults - as the loudness window's button does (asks first). The "
                    L"density band handles go back with it.";
}

std::wstring SettingsSpectrum::hint(int d) const {
    if (!editable(d, true)) return L"This deck has no colour.";
    return L"Drag a point: the colour \"" + fromUtf8(settings_.deck[d].colour) +
           L"\" (blue: bass level and frequency). A purple handle: a density band of \"" +
           fromUtf8(settings_.deck[d].preset) + L"\". Every deck using them changes; double-click resets.";
}

void SettingsSpectrum::setSnapToBands(bool on) {
    settings_.snapToBands = on;
    if (changed_) changed_(true);
}

// ---- the window ----

SpectrumWindow::SpectrumWindow(SpectrumSource& source) : source_(source) {}

SpectrumWindow::~SpectrumWindow() { close(); }

void SpectrumWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void SpectrumWindow::setModule(HINSTANCE instance) { g_module = instance; }

void SpectrumWindow::unregisterClass() {
    if (g_registered) UnregisterClassW(kClass, moduleInstance());
    g_registered = false;
}

void SpectrumWindow::show(HWND owner, int deck) {
    deck_ = std::max(0, std::min(source_.decks() - 1, deck));
    if (!hwnd_) {
        HINSTANCE inst = moduleInstance();
        if (!g_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
            wc.lpszClassName = kClass;
            g_registered = RegisterClassExW(&wc) != 0;
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
    if (deck_ >= source_.decks()) deck_ = 0; // one channel now: deck A
    if (layoutDecks_ != source_.decks()) layoutBar();
    setTitle(); // the deck's preset or colour may have been chosen elsewhere
    fillBoxes();
    fillSliders();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::setTitle() {
    if (!hwnd_) return;
    // the deck only (client, 2026-10-30: "(loudness FM Station, colour Oldies) is superfluous" - the window's own
    // lists show them)
    const std::wstring title = L"Spectrum: " + source_.deckName(deck_);
    if (title == title_) return; // only when it changed (refresh() asks 20 times a second)
    title_ = title;
    SetWindowTextW(hwnd_, title.c_str());
}

spectrum::Data SpectrumWindow::data() const {
    spectrum::Data d;
    const DeckMeters& m = source_.meters(deck_);
    const DeckAnalysis& an = source_.analysis(deck_);
    const PresetValues v = source_.values(deck_);
    const bool live = source_.audioActive() && m.live;
    d.in = an.specIn.points();
    d.out = an.specOut.points();
    d.live = live && an.specOut.frames() > 0;
    d.densityOn = source_.processing(deck_) && v[pDensOn] >= 0.5;
    d.bands = m.bands > 0 ? m.bands : static_cast<int>(v[pDensBands]);
    for (int k = 0; k < kMaxSplits; ++k) d.splits[k] = v[static_cast<size_t>(pDensSplit1 + k)];
    for (int k = 0; k < kMaxBands; ++k) d.densityDb[k] = live ? m.densityDb[k] : 0.0;
    d.colour = true;
    d.values = v;
    for (int k = 0; k < 4; ++k) d.autoDb[k] = live ? m.autoToneDb[k] : 0.0;
    d.fs = source_.sampleRate() > 0 ? source_.sampleRate() : 48000.0;
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
    if (!source_.editable(deck_, false) || draggingBand_ < 0) return;
    const int gp = pDensGain1 + draggingBand_;
    const double g = paramValueAt(gp, paramPositionOf(gp, geo_.eqAt(pt.y)));
    if (g == source_.values(deck_)[static_cast<size_t>(gp)]) return;
    source_.set(deck_, {{gp, g}}, false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::layoutBar() {
    if (!hwnd_ || !boxes_[0]) return;
    layoutDecks_ = source_.decks();
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    auto width = [&](const wchar_t* s) {
        SIZE sz;
        GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
        return static_cast<int>(sz.cx);
    };
    const int m = px(12), barH = px(30);
    int x = m;
    if (source_.decks() == 2) x += width(L"Deck") + px(8) + 2 * (width(L"A") + px(20)) + px(24); // as paint() draws them
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
        const std::vector<std::string> names = source_.names(k == 1);
        if (names != boxNames_[k]) {
            SendMessageW(boxes_[k], CB_RESETCONTENT, 0, 0);
            for (const std::string& n : names) SendMessageW(boxes_[k], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(n).c_str()));
            boxNames_[k] = names;
        }
        const LRESULT want = source_.chosen(deck_, k == 1);
        if (SendMessageW(boxes_[k], CB_GETCURSEL, 0, 0) != want && SendMessageW(boxes_[k], CB_GETDROPPEDSTATE, 0, 0) == 0)
            SendMessageW(boxes_[k], CB_SETCURSEL, static_cast<WPARAM>(want), 0);
    }
    filling_ = false;
}

void SpectrumWindow::fillSliders() {
    const PresetValues v = source_.values(deck_);
    for (int k = 0; k < bass::kSliders; ++k) {
        if (!bass_[k] || GetCapture() == bass_[k]) continue; // dragged now: it shows its own position
        const int pos = bass::position(v, k);
        if (SendMessageW(bass_[k], TBM_GETPOS, 0, 0) != pos) SendMessageW(bass_[k], TBM_SETPOS, TRUE, pos);
    }
}

void SpectrumWindow::choose(bool colour) {
    const LRESULT sel = SendMessageW(boxes_[colour ? 1 : 0], CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    source_.choose(deck_, colour, static_cast<int>(sel));
    setTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::endDrag() {
    const bool was = dragging_ >= 0 || draggingBand_ >= 0;
    dragging_ = draggingBand_ = snapSplit_ = -1;
    if (was) source_.set(deck_, {}, true); // saved when the mouse is let go
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SpectrumWindow::drag(POINT pt) {
    if (!source_.editable(deck_, true) || dragging_ < 0) return;
    const PresetValues v = source_.values(deck_);
    const int fp = spectrum::kNodeFreq[dragging_], gp = spectrum::kNodeGain[dragging_];
    // the parameters' own steps (frequencies on their log scale), within their ranges; with Snap to bands, on a
    // band split when the mouse is near it (and the split lies within the section's range)
    double f = paramValueAt(fp, paramPositionOf(fp, geo_.hzAt(pt.x)));
    snapSplit_ = -1;
    if (source_.snapToBands()) {
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
        const int posF = static_cast<int>(std::lround(f / 5.0)), posL = static_cast<int>(std::lround(g * 2.0));
        if (posF == bass::position(v, bass::kFreq) && posL == bass::position(v, bass::kLevel)) return;
        std::vector<std::pair<int, double>> both = bass::changes(bass::kFreq, posF);
        for (const auto& c : bass::changes(bass::kLevel, posL)) both.push_back(c);
        source_.set(deck_, both, false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (f == v[static_cast<size_t>(fp)] && g == v[static_cast<size_t>(gp)]) return;
    source_.set(deck_, {{fp, f}, {gp, g}}, false); // heard at once; saved when the mouse is let go
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT SpectrumWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        setFonts();
        tips_.create(hwnd_);
        for (int t = 0; t < kTips; ++t)
            tips_.addRect(hwnd_, static_cast<UINT_PTR>(t), t == tResetPreset   ? source_.resetTip(false)
                                                           : t == tResetColour ? source_.resetTip(true)
                                                                               : kTipText[t]);
        for (int k = 0; k < 2; ++k) {
            boxes_[k] = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST,
                                        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(idLoudnessBox + k)),
                                        moduleInstance(), nullptr);
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
                                       moduleInstance(), nullptr);
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
                // heard while dragging, saved when let go (keys: at once)
                source_.setBass(deck_, k, pos, LOWORD(wp) != TB_THUMBTRACK);
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
            deck_ = std::min(source_.decks() - 1, b == bDeckA ? 0 : 1);
            setTitle();
            fillBoxes();
            fillSliders();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (b == bResetPreset || b == bResetColour) {
            source_.reset(deck_, b == bResetColour, hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (b == bSnap) {
            source_.setSnapToBands(!source_.snapToBands());
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            const int node = source_.editable(deck_, true) ? geo_.nodeAt(pt, px(10)) : -1;
            const int band = node < 0 && source_.editable(deck_, false) ? geo_.handleAt(pt, px(6)) : -1;
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
        if (n >= 0 && source_.editable(deck_, true)) {
            source_.set(deck_, {{spectrum::kNodeGain[n], 0.0}}, true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (band >= 0 && source_.editable(deck_, false)) {
            source_.set(deck_, {{pDensGain1 + band, 0.0}}, true);
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
    const bool two = source_.decks() == 2;
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
    x = drawButton(bSnap, x, L"Snap to bands", source_.snapToBands(), true);
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
        const PresetValues v = source_.values(deck_);
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
    const int node = dragging_ >= 0 ? dragging_ : hot_;
    if (node >= 0 && source_.editable(deck_, true)) {
        wchar_t b[96];
        swprintf(b, 96, L"%ls: %ls, %+.1f dB", spectrum::nodeName(node),
                 hzText(d.values[static_cast<size_t>(spectrum::kNodeFreq[node])]).c_str(),
                 d.values[static_cast<size_t>(spectrum::kNodeGain[node])]);
        read = b;
        if (snapSplit_ >= 0 && dragging_ >= 0) {
            wchar_t s[48];
            swprintf(s, 48, L"  (on the split of bands %d and %d)", snapSplit_ + 1, snapSplit_ + 2);
            read += s;
        }
    } else if ((draggingBand_ >= 0 || hotBand_ >= 0) && source_.editable(deck_, false)) {
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
    const std::wstring hint = source_.hint(deck_);
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
