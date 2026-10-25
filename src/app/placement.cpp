#include "placement.h"

#include <algorithm>

namespace ap::placement {
namespace {

// The owner's rectangle on screen as it is when shown: a minimised window reports its normal position in workspace
// coordinates (relative to the work area of its monitor), turned into screen coordinates here.
RECT shownRect(HWND owner) {
    RECT r{};
    if (!IsIconic(owner)) {
        GetWindowRect(owner, &r);
        return r;
    }
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    GetWindowPlacement(owner, &wp);
    r = wp.rcNormalPosition;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi) &&
        !(GetWindowLongW(owner, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
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

} // namespace

void centreOn(HWND wnd, HWND owner) {
    if (!wnd || !owner || !IsWindow(owner)) return;
    const RECT o = shownRect(owner);
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

} // namespace ap::placement
