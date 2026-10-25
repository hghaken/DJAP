#include "appstate.h"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <fstream>

namespace ap {

namespace fs = std::filesystem;

namespace {

const wchar_t* const kIniName = L"DJAP.ini";
// the program was called AudioProcessor up to version 0.1.57: its settings are taken over
const wchar_t* const kOldIniName = L"AudioProcessor.ini";
const wchar_t* const kOldFolder = L"AudioProcessor";

fs::path appDataDir(const wchar_t* folder = L"DJAP") {
    PWSTR p = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = fs::path(p) / folder;
    CoTaskMemFree(p);
    return dir;
}

bool present(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec);
}

// Can a file be created in `dir`? (Program Files is not writable for a normal user.)
bool writable(const fs::path& dir) {
    const fs::path probe = dir / L"DJAP.write-test";
    {
        std::ofstream f(probe, std::ios::binary | std::ios::trunc);
        if (!f) return false;
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

// The running program's own file.
fs::path programFile() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
}

} // namespace

fs::path programDir() {
    const fs::path exe = programFile();
    return exe.empty() ? fs::current_path() : exe.parent_path();
}

void adoptOldSettings() {
    std::error_code ec;
    const fs::path dir = programDir();
    if (!present(dir / kIniName) && present(dir / kOldIniName)) fs::copy_file(dir / kOldIniName, dir / kIniName, ec);
    const fs::path roaming = appDataDir(), old = appDataDir(kOldFolder);
    if (!roaming.empty() && !present(roaming / kIniName) && present(old / kOldIniName)) {
        fs::create_directories(roaming, ec);
        fs::copy_file(old / kOldIniName, roaming / kIniName, ec);
    }
}

fs::path settingsFile() {
    std::error_code ec;
    const fs::path local = programDir() / kIniName;
    if (present(local)) return local;
    const fs::path roaming = appDataDir();
    if (present(roaming / kIniName)) return roaming / kIniName;
    // not taken over yet (the command line reads them where they are)
    if (present(programDir() / kOldIniName)) return programDir() / kOldIniName;
    if (present(appDataDir(kOldFolder) / kOldIniName)) return appDataDir(kOldFolder) / kOldIniName;
    if (writable(programDir()) || roaming.empty()) return local;
    fs::create_directories(roaming, ec);
    return roaming / kIniName;
}

namespace {

const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
const wchar_t* const kRunValue = L"DJAP";

// The program file named by a Run command: the quoted path at its start (or the text up to the first space).
fs::path commandProgram(const std::wstring& cmd) {
    if (!cmd.empty() && cmd[0] == L'"') {
        const size_t end = cmd.find(L'"', 1);
        return fs::path(cmd.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1));
    }
    return fs::path(cmd.substr(0, cmd.find(L' ')));
}

} // namespace

bool startsWithWindows() {
    std::wstring cmd(2048, L'\0');
    DWORD bytes = static_cast<DWORD>(cmd.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, cmd.data(), &bytes) !=
        ERROR_SUCCESS)
        return false;
    cmd.resize(wcslen(cmd.c_str()));
    const fs::path exe = programFile();
    if (exe.empty() || _wcsicmp(commandProgram(cmd).lexically_normal().c_str(), exe.lexically_normal().c_str()) != 0)
        return false;
    // Windows' list of startup apps (Task Manager, Settings) marks a switched-off entry with an odd first byte
    BYTE mark[16] = {};
    DWORD size = sizeof mark;
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, mark, &size) ==
            ERROR_SUCCESS &&
        size > 0 && (mark[0] & 1))
        return false;
    return true;
}

bool setStartWithWindows(bool on, std::string& err) {
    LSTATUS s;
    if (on) {
        const std::wstring cmd = L"\"" + programFile().wstring() + L"\" --autostart";
        s = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        if (s == ERROR_SUCCESS) RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue); // no "disabled" left
    } else {
        s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        if (s == ERROR_FILE_NOT_FOUND) s = ERROR_SUCCESS;
    }
    if (s != ERROR_SUCCESS) err = "start with Windows not changed (registry error " + std::to_string(s) + ")";
    return s == ERROR_SUCCESS;
}

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool lessIgnoringCase(const std::string& a, const std::string& b) {
    const std::wstring wa = fromUtf8(a), wb = fromUtf8(b);
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, wa.c_str(),
                           static_cast<int>(wa.size()), wb.c_str(), static_cast<int>(wb.size()), nullptr, nullptr,
                           0) == CSTR_LESS_THAN;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

} // namespace ap
