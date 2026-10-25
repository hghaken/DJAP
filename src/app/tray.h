// The program's icon in the notification area (system tray): a tooltip with the decks' state, a click shows the
// main window, a right click opens its menu (the owner window handles kMessage). Added again when Explorer restarts.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace ap {

class TrayIcon {
public:
    static constexpr UINT kMessage = WM_APP + 20; // to the owner: LOWORD(lParam) = NIN_SELECT, WM_CONTEXTMENU, ...
    ~TrayIcon() { remove(); }
    bool add(HWND owner, HICON icon, const std::wstring& tip);
    void setTip(const std::wstring& tip); // at most 127 characters (longer is cut)
    void balloon(const std::wstring& title, const std::wstring& text);
    void remove();
    // The owner passes every message here first: true for Explorer's "TaskbarCreated" (the icon is added again).
    bool taskbarCreated(UINT msg);

private:
    bool show();
    NOTIFYICONDATAW data_{};
    bool added_ = false;
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
};

} // namespace ap
