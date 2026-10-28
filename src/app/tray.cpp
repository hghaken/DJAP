// SPDX-License-Identifier: GPL-3.0-or-later
#include "tray.h"

#include <shellapi.h>

#include <cwchar>
#include <iterator>

namespace ap {

namespace {

void copy(wchar_t* to, size_t size, const std::wstring& from) {
    wcsncpy_s(to, size, from.c_str(), _TRUNCATE);
}

} // namespace

bool TrayIcon::add(HWND owner, HICON icon, const std::wstring& tip) {
    data_ = {};
    data_.cbSize = sizeof data_;
    data_.hWnd = owner;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = kMessage;
    data_.hIcon = icon;
    copy(data_.szTip, std::size(data_.szTip), tip);
    return show();
}

bool TrayIcon::show() {
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (added_) {
        // version 4: NIN_SELECT on a click, WM_CONTEXTMENU on a right click, the position in wParam
        data_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data_);
    }
    return added_;
}

void TrayIcon::setTip(const std::wstring& tip) {
    if (!added_ || tip == data_.szTip) return;
    copy(data_.szTip, std::size(data_.szTip), tip);
    data_.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}

void TrayIcon::balloon(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW n = data_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_USER; // the program's own icon
    n.hBalloonIcon = data_.hIcon;
    copy(n.szInfoTitle, std::size(n.szInfoTitle), title);
    copy(n.szInfo, std::size(n.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayIcon::remove() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
    data_.hWnd = nullptr; // not added again after the owner is gone
}

bool TrayIcon::taskbarCreated(UINT msg) {
    if (msg != taskbarCreated_ || taskbarCreated_ == 0) return false;
    if (data_.hWnd) show();
    return true;
}

} // namespace ap
