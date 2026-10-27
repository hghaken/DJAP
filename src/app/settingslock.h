// One writer of the settings file at a time across processes: the program and every plug-in instance read, merge and
// write the same %APPDATA%\DJAP\DJAP.ini (Settings::saveMerged). Two of them saving at the same moment could each
// merge against the file as it was before the other wrote, and the second write would drop the first one's change
// (outside review). A named mutex around the whole read - merge - write; after 2 s without it the save goes ahead
// anyway (a hung writer must not freeze the window).
#pragma once

#include <windows.h>

namespace ap {

class SettingsLock {
public:
    SettingsLock() : h_(CreateMutexW(nullptr, FALSE, L"Local\\DJAP.Settings")) {
        // WAIT_ABANDONED (a writer ended while holding it) also gives the mutex to this one
        if (h_) owned_ = WaitForSingleObject(h_, 2000) != WAIT_TIMEOUT;
    }
    ~SettingsLock() {
        if (h_ && owned_) ReleaseMutex(h_);
        if (h_) CloseHandle(h_);
    }
    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;

private:
    HANDLE h_ = nullptr;
    bool owned_ = false;
};

} // namespace ap
