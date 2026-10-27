// One writer of the settings file at a time across processes: the program and every plug-in instance read, merge and
// write the same %APPDATA%\DJAP\DJAP.ini (Settings::saveMerged). Two of them saving at the same moment could each
// merge against the file as it was before the other wrote, and the second write would drop the first one's change
// (outside review). A named mutex around the whole read - merge - write; after 2 s without it the save goes ahead
// anyway (a hung writer must not freeze the window).
#pragma once

#include <windows.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace ap {

// A named mutex that every process of this user can open, elevated or not: with Windows' default security a mutex
// made by a program run as administrator is closed to the user's other programs (access denied - the settings lock
// was then skipped, and a second DJAP did not see the first; fourth outside review). Access for the user's own SID,
// and the medium integrity label, so a normal program may take it. Falls back to the default security.
inline HANDLE createUserMutex(const wchar_t* name) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        alignas(TOKEN_USER) unsigned char buf[256];
        DWORD len = 0;
        wchar_t* sid = nullptr;
        if (GetTokenInformation(token, TokenUser, buf, sizeof buf, &len) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &sid)) {
            wchar_t sddl[256];
            if (swprintf_s(sddl, L"D:(A;;GA;;;%s)(A;;GA;;;SY)S:(ML;;NW;;;ME)", sid) > 0 &&
                ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
                sa.lpSecurityDescriptor = sd;
            LocalFree(sid);
        }
        CloseHandle(token);
    }
    HANDLE h = CreateMutexW(sa.lpSecurityDescriptor ? &sa : nullptr, FALSE, name);
    if (!h && sd && GetLastError() != ERROR_ACCESS_DENIED) // the label refused (a low-integrity host): the default
        h = CreateMutexW(nullptr, FALSE, name);
    const DWORD err = GetLastError(); // ERROR_ALREADY_EXISTS / ERROR_ACCESS_DENIED for the caller
    if (sd) LocalFree(sd);
    SetLastError(err);
    return h;
}

class SettingsLock {
public:
    SettingsLock() : h_(createUserMutex(L"Local\\DJAP.Settings")) {
        // WAIT_ABANDONED (a writer ended while holding it) also gives the mutex to this one; a timeout or a failed
        // wait does not (the save then goes ahead without it)
        if (h_) {
            const DWORD r = WaitForSingleObject(h_, 2000);
            owned_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
        }
    }
    ~SettingsLock() {
        if (h_ && owned_) ReleaseMutex(h_);
        if (h_) CloseHandle(h_);
    }
    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;
    bool owned() const { return owned_; }

private:
    HANDLE h_ = nullptr;
    bool owned_ = false;
};

} // namespace ap
