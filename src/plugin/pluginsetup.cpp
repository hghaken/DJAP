// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginsetup.h"

#include "deck.h"
#include "placement.h"
#include "plugin.h"
#include "pluginids.h"
#include "resource.h"

#include <commctrl.h>

#include <cmath>
#include <iterator>

namespace ap::plugin {

namespace {

const wchar_t* const kClass = L"DJAPPluginSetup";
HINSTANCE g_module = nullptr;
bool g_registered = false;
enum Id { idLook = 1, idOversampling, idTooltips, idUpdates, idClose };

} // namespace

void SetupWindow::setModule(HINSTANCE instance) { g_module = instance; }

void SetupWindow::unregisterClass() {
    if (g_registered) UnregisterClassW(kClass, g_module);
    g_registered = false;
}

void SetupWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void SetupWindow::show(HWND owner) {
    if (!plugin_) return;
    if (!hwnd_) {
        if (!g_registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = proc;
            wc.hInstance = g_module;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(g_module, MAKEINTRESOURCEW(IDI_APP));
            wc.lpszClassName = kClass;
            g_registered = RegisterClassExW(&wc) != 0;
        }
        const DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        dpi_ = owner ? GetDpiForWindow(owner) : 96;
        if (dpi_ == 0) dpi_ = 96;
        RECT r{0, 0, px(470), px(232)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi_);
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"Setup (DJAP plug-in)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, owner, nullptr, g_module, this);
        if (!hwnd_) return;
        create();
        placement::centreOn(hwnd_, owner);
    }
    refresh();
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

LRESULT CALLBACK SetupWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* w = reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    if (!w->hwnd_) w->hwnd_ = hwnd; // during CreateWindowExW
    const LRESULT r = w->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        w->hwnd_ = w->look_ = w->oversampling_ = w->tooltips_ = w->updates_ = w->close_ = nullptr;
    }
    return r;
}

LRESULT SetupWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, result)) return result;
    switch (msg) {
    case WM_COMMAND:
        command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void SetupWindow::create() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_module, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return c;
    };
    HWND lookLabel = make(L"STATIC", L"Look-ahead", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0, 14, 14, 110, 26);
    look_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idLook, 130, 14, 326, 300);
    HWND osLabel = make(L"STATIC", L"Oversampling", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0, 14, 48, 110, 26);
    oversampling_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idOversampling, 130, 48, 326, 300);
    tooltips_ = make(L"BUTTON", L"Show tooltips", BS_AUTOCHECKBOX | WS_TABSTOP, idTooltips, 14, 86, 440, 24);
    updates_ = make(L"BUTTON", L"Look for a newer DJAP each time the DJ software loads the plug-in", BS_AUTOCHECKBOX | WS_TABSTOP,
                    idUpdates, 14, 112, 440, 24);
    make(L"STATIC",
         L"The audio devices are the DJ software's: it chooses this plug-in's input and output. Tooltips and the update "
         L"check are shared with the DJAP program (its settings file).",
         SS_LEFT, 0, 14, 144, 442, 44);
    close_ = make(L"BUTTON", L"Close", BS_OWNERDRAW | WS_TABSTOP, idClose, 370, 194, 86, 28);
    theme::makeButton(close_);
    theme::makeCombo(look_);
    theme::makeCombo(oversampling_);
    for (double l : kLookChoicesMs) {
        wchar_t t[48];
        swprintf(t, 48, l == 0.0 ? L"0 ms (sample peak, no delay)" : l == 1.5 ? L"%.1f ms (default)" : L"%.1f ms", l);
        SendMessageW(look_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    }
    for (const wchar_t* t : {L"2x", L"4x (default)", L"8x"}) SendMessageW(oversampling_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));

    tips_.create(hwnd_);
    const wchar_t* lookTip = L"The limiter's look-ahead: the only delay the processing adds (the DJ software is told, so it "
                             L"keeps this channel in time with the others). Longer handles peaks more gently; every "
                             L"choice above 0 limits the true peak; 0 ms = no delay, sample peaks only. The same list "
                             L"as in the plug-in's window.";
    const wchar_t* osTip = L"How many times the sample rate the soft clipper and the exciter work at: 2x leaves a rest of "
                           L"folded-back overtones on hard-driven high tones, 4x and 8x take it away at a little more CPU. "
                           L"Adds no latency.";
    tips_.add(lookLabel, lookTip);
    tips_.add(look_, lookTip);
    tips_.add(osLabel, osTip);
    tips_.add(oversampling_, osTip);
    tips_.add(tooltips_, L"The explanations that appear when the mouse rests on a control, in the plug-in and in the "
                         L"program (one switch for both: the settings file).");
    tips_.add(updates_, L"Asks GitHub for a newer DJAP each time the DJ software loads the plug-in (the program asks once "
                        L"a day; one switch for both). A newer version lights the About button as \"Update\".");
}

void SetupWindow::refresh() {
    if (!hwnd_ || !plugin_) return;
    const double look = plugin_->plainOf(kLookId);
    int li = 0;
    for (int i = 0; i < static_cast<int>(std::size(kLookChoicesMs)); ++i)
        if (std::fabs(kLookChoicesMs[i] - look) < std::fabs(kLookChoicesMs[li] - look)) li = i;
    if (SendMessageW(look_, CB_GETCURSEL, 0, 0) != li && !SendMessageW(look_, CB_GETDROPPEDSTATE, 0, 0))
        SendMessageW(look_, CB_SETCURSEL, static_cast<WPARAM>(li), 0);
    const int os = static_cast<int>(plugin_->plainOf(kOversamplingId)), oi = os == 8 ? 2 : os == 4 ? 1 : 0;
    if (SendMessageW(oversampling_, CB_GETCURSEL, 0, 0) != oi && !SendMessageW(oversampling_, CB_GETDROPPEDSTATE, 0, 0))
        SendMessageW(oversampling_, CB_SETCURSEL, static_cast<WPARAM>(oi), 0);
    SendMessageW(tooltips_, BM_SETCHECK, plugin_->tooltips() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(updates_, BM_SETCHECK, plugin_->checkUpdates() ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SetupWindow::command(int id, int code) {
    if (!plugin_) return;
    if (id == idLook && code == CBN_SELCHANGE) {
        const int sel = static_cast<int>(SendMessageW(look_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(std::size(kLookChoicesMs))) // the parameter: a list of the same five
            plugin_->edit(kLookId, sel / static_cast<double>(std::size(kLookChoicesMs) - 1));
    } else if (id == idOversampling && code == CBN_SELCHANGE) {
        const int sel = static_cast<int>(SendMessageW(oversampling_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel <= 2) plugin_->edit(kOversamplingId, sel / 2.0);
    } else if ((id == idTooltips || id == idUpdates) && code == BN_CLICKED) {
        const bool tips = SendMessageW(tooltips_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool updates = SendMessageW(updates_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        plugin_->setSwitches(tips, updates);
        Tooltips::setEnabled(tips); // every window of this DLL, at once
    } else if (id == idClose && code == BN_CLICKED) {
        DestroyWindow(hwnd_);
    }
}

} // namespace ap::plugin
