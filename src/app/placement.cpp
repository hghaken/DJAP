// SPDX-License-Identifier: GPL-3.0-or-later
#include "placement.h"

#include <commctrl.h>

#include <algorithm>

namespace ap::placement {
namespace {

// A window's normal rectangle on screen: as shown, or - minimised or maximised - its normal position, which Windows
// reports in workspace coordinates (relative to the work area of its monitor), turned into screen coordinates here.
RECT normalRect(HWND wnd) {
    RECT r{};
    if (!IsIconic(wnd) && !IsZoomed(wnd)) {
        GetWindowRect(wnd, &r);
        return r;
    }
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    GetWindowPlacement(wnd, &wp);
    r = wp.rcNormalPosition;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi) &&
        !(GetWindowLongW(wnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
    return r;
}

thread_local HHOOK g_hook = nullptr;
thread_local HWND g_owner = nullptr;

LRESULT CALLBACK cbtProc(int code, WPARAM wp, LPARAM lp) {
    const HHOOK hook = g_hook;
    if (code == HCBT_ACTIVATE && hook) {
        // the message box's own window: the first one activated while the hook is set
        UnhookWindowsHookEx(hook);
        g_hook = nullptr;
        centreOn(reinterpret_cast<HWND>(wp), g_owner);
    }
    return CallNextHookEx(hook, code, wp, lp);
}

// ---- remembered places ----
std::map<std::string, WindowPlace>* g_places = nullptr;
std::function<void()> g_save;
std::string g_prefix; // the plug-in's: "Plugin"
const UINT_PTR kSubclass = 0x4A50; // the tracking subclass's id

struct Tracked {
    std::string name;
    bool maximiseFirst = false; // shown maximised the first time (it was maximised last)
    bool moving = false;        // the user drags or sizes it now: saved when that ends
};

// The window's place now into the store, and the file saved when it changed. A hidden or minimised window is not
// remembered (hiding into the notification area changes nothing).
void store(HWND wnd, const Tracked& t) {
    if (!g_places || !IsWindowVisible(wnd) || IsIconic(wnd)) return;
    const RECT r = normalRect(wnd);
    const WindowPlace p{r.left, r.top, r.right - r.left, r.bottom - r.top, IsZoomed(wnd) != FALSE};
    WindowPlace& slot = (*g_places)[t.name];
    if (slot.x == p.x && slot.y == p.y && slot.w == p.w && slot.h == p.h && slot.maximised == p.maximised) return;
    slot = p;
    if (g_save) g_save();
}

LRESULT CALLBACK trackProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    Tracked* t = reinterpret_cast<Tracked*>(ref);
    const LRESULT result = DefSubclassProc(wnd, msg, wp, lp);
    switch (msg) {
    case WM_ENTERSIZEMOVE:
        t->moving = true;
        break;
    case WM_EXITSIZEMOVE:
        t->moving = false;
        store(wnd, *t);
        break;
    case WM_WINDOWPOSCHANGED: { // maximise, restore, snap, Win + arrow keys, a window that fits its content
        const WINDOWPOS* p = reinterpret_cast<const WINDOWPOS*>(lp);
        if (!t->moving && (p->flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) store(wnd, *t);
        break;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(wnd, trackProc, kSubclass);
        delete t;
        break;
    default:
        break;
    }
    return result;
}

// Centred on the work area of the owner's monitor (the main monitor without an owner).
void centreOnScreen(HWND wnd, HWND owner) {
    HMONITOR mon = nullptr;
    if (owner && IsWindow(owner)) {
        const RECT o = normalRect(owner);
        mon = MonitorFromRect(&o, MONITOR_DEFAULTTONEAREST);
    } else {
        mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& a = mi.rcWork;
    RECT w;
    GetWindowRect(wnd, &w);
    const int cx = w.right - w.left, cy = w.bottom - w.top;
    const int x = std::max<int>(a.left, (a.left + a.right - cx) / 2), y = std::max<int>(a.top, (a.top + a.bottom - cy) / 2);
    SetWindowPos(wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// The remembered place, if its title bar is still on a monitor (the screens may have changed since).
bool restore(HWND wnd, const WindowPlace& p) {
    RECT w;
    GetWindowRect(wnd, &w);
    const bool sizable = (GetWindowLongW(wnd, GWL_STYLE) & WS_THICKFRAME) != 0;
    const int cx = sizable && p.w > 0 ? p.w : w.right - w.left, cy = sizable && p.h > 0 ? p.h : w.bottom - w.top;
    const POINT title{p.x + cx / 2, p.y + GetSystemMetricsForDpi(SM_CYCAPTION, GetDpiForWindow(wnd)) / 2 + 4};
    HMONITOR mon = MonitorFromPoint(title, MONITOR_DEFAULTTONULL);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const int y = std::max<int>(p.y, mi.rcWork.top); // the title bar stays reachable
    SetWindowPos(wnd, nullptr, p.x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
    // on a monitor of another scale the window got that monitor's DPI on the way (and a scaled size): once more
    SetWindowPos(wnd, nullptr, p.x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

} // namespace

void centreOn(HWND wnd, HWND owner) {
    if (!wnd || !owner || !IsWindow(owner)) return;
    const RECT o = normalRect(owner);
    RECT w;
    GetWindowRect(wnd, &w);
    const int cx = w.right - w.left, cy = w.bottom - w.top;
    int x = (o.left + o.right - cx) / 2, y = (o.top + o.bottom - cy) / 2;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromRect(&o, MONITOR_DEFAULTTONEAREST), &mi)) {
        const RECT& a = mi.rcWork;
        x = std::max<int>(a.left, std::min<int>(x, a.right - cx));
        y = std::max<int>(a.top, std::min<int>(y, a.bottom - cy)); // the title bar stays reachable
    }
    SetWindowPos(wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type) {
    if (owner && !g_hook) {
        g_owner = owner;
        g_hook = SetWindowsHookExW(WH_CBT, cbtProc, nullptr, GetCurrentThreadId());
    }
    const int result = MessageBoxW(owner, text, caption, type);
    if (g_hook) { // the box never became active
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return result;
}

void setStore(std::map<std::string, WindowPlace>* places, std::function<void()> save, const std::string& prefix) {
    g_places = places;
    g_save = std::move(save);
    g_prefix = prefix;
}

void remember(HWND wnd, const char* name, HWND owner) {
    if (!wnd) return;
    // the store's name: with the plug-in's prefix ("PluginAbout" has it already)
    const std::string key = std::string(name).compare(0, g_prefix.size(), g_prefix) == 0 ? name : g_prefix + name;
    const WindowPlace* p = nullptr;
    if (g_places) {
        auto it = g_places->find(key);
        if (it != g_places->end()) p = &it->second;
    }
    if (!p || !restore(wnd, *p)) centreOnScreen(wnd, owner);
    if (!g_places) return;
    Tracked* t = new Tracked{key, p && p->maximised, false};
    if (!SetWindowSubclass(wnd, trackProc, kSubclass, reinterpret_cast<DWORD_PTR>(t))) delete t;
}

int showCommand(HWND wnd) {
    if (IsIconic(wnd)) return SW_RESTORE;
    if (IsWindowVisible(wnd)) return SW_SHOW;
    if (IsZoomed(wnd)) return SW_SHOWMAXIMIZED; // hidden while maximised
    DWORD_PTR ref = 0;
    if (GetWindowSubclass(wnd, trackProc, kSubclass, &ref) && ref) {
        Tracked* t = reinterpret_cast<Tracked*>(ref);
        if (t->maximiseFirst && (GetWindowLongW(wnd, GWL_STYLE) & WS_MAXIMIZEBOX)) {
            t->maximiseFirst = false;
            return SW_SHOWMAXIMIZED;
        }
    }
    return SW_SHOWNORMAL;
}

} // namespace ap::placement
