#include "tooltips.h"

#include <commctrl.h>

#include <algorithm>
#include <vector>

namespace ap {

namespace {
bool g_enabled = true;
std::vector<HWND> g_tips; // the tooltip controls created so far (the destroyed ones are dropped on the next switch)
} // namespace

void Tooltips::setEnabled(bool on) {
    g_enabled = on;
    g_tips.erase(std::remove_if(g_tips.begin(), g_tips.end(), [](HWND h) { return !IsWindow(h); }), g_tips.end());
    for (HWND h : g_tips) SendMessageW(h, TTM_ACTIVATE, on ? TRUE : FALSE, 0);
}

bool Tooltips::enabled() { return g_enabled; }

void Tooltips::create(HWND owner) {
    owner_ = owner;
    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!tip_) return;
    SendMessageW(tip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    SendMessageW(tip_, TTM_SETDELAYTIME, TTDT_INITIAL, 600);
    setDpi(GetDpiForWindow(owner));
    g_tips.erase(std::remove_if(g_tips.begin(), g_tips.end(), [](HWND h) { return !IsWindow(h); }), g_tips.end());
    g_tips.push_back(tip_);
    if (!g_enabled) SendMessageW(tip_, TTM_ACTIVATE, FALSE, 0);
}

void Tooltips::setDpi(UINT dpi) {
    if (tip_) SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, MulDiv(440, static_cast<int>(dpi), 96)); // wraps the lines
}

void Tooltips::add(HWND control, const wchar_t* text) {
    if (!tip_ || !control) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner_;
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = const_cast<wchar_t*>(text);
    if (!SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti)))
        SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

void Tooltips::addRect(HWND window, UINT_PTR id, const wchar_t* text) {
    if (!tip_) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = window;
    ti.uId = id;
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void Tooltips::setRect(HWND window, UINT_PTR id, const RECT& r) {
    if (!tip_) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.hwnd = window;
    ti.uId = id;
    ti.rect = r;
    SendMessageW(tip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
}

} // namespace ap
