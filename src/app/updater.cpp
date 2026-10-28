#include "updater.h"

#include "appstate.h"

#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cwchar>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ap {

namespace fs = std::filesystem;

namespace {

struct Url {
    bool https = false;
    std::wstring host, path;
    INTERNET_PORT port = 0;
};

bool crack(const std::string& url, Url& u) {
    const std::wstring w = fromUtf8(url);
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof c;
    c.lpszHostName = host;
    c.dwHostNameLength = 256;
    c.lpszUrlPath = path;
    c.dwUrlPathLength = 2048;
    c.lpszExtraInfo = extra;
    c.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &c)) return false;
    if (c.nScheme != INTERNET_SCHEME_HTTPS && c.nScheme != INTERNET_SCHEME_HTTP) return false;
    u.https = c.nScheme == INTERNET_SCHEME_HTTPS;
    u.host = host;
    u.path = std::wstring(path) + extra;
    u.port = c.nPort;
    return !u.host.empty();
}

bool localHost(const std::wstring& h) { return h == L"127.0.0.1" || h == L"localhost"; }

bool endsWith(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

// Where the program may ask and download: https from GitHub (its API, its pages and the asset storage it redirects
// to), or - for tests - the host of the settings' Url when that is this computer.
bool allowed(const std::string& url, const std::string& api) {
    Url u, a;
    if (!crack(url, u)) return false;
    if (crack(api, a) && localHost(a.host) && _wcsicmp(u.host.c_str(), a.host.c_str()) == 0) return true;
    return u.https && (_wcsicmp(u.host.c_str(), L"github.com") == 0 || _wcsicmp(u.host.c_str(), L"api.github.com") == 0 ||
                       endsWith(u.host, L".githubusercontent.com"));
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string netError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "no connection to GitHub (offline?)";
    case ERROR_WINHTTP_TIMEOUT:
        return "GitHub did not answer in time";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "the secure connection to GitHub failed";
    default:
        return "network error " + std::to_string(e);
    }
}

enum class Got { Ok, NotFound, Failed };

// GET `url`: the body into `body` (at most 4 MB), or into `out` with a SHA-256 and the progress in `percent`. Where
// the redirects (which WinHTTP follows) ended must be allowed too (`api`: see allowed()).
// `status`: the HTTP status (0: none came); `landed`: the address after the redirects.
Got get(const std::string& url, const std::string& api, const wchar_t* accept, std::string* body, std::ofstream* out,
        Sha256* sha, std::atomic<int>* percent, uint64_t expected, const std::atomic<bool>& quit, std::string& error,
        DWORD* status = nullptr, std::string* landed = nullptr) {
    Url u;
    if (!crack(url, u)) {
        error = "not a web address: " + url;
        return Got::Failed;
    }
    Handle s{WinHttpOpen(L"DJAP-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!s.h) {
        error = netError(GetLastError());
        return Got::Failed;
    }
    WinHttpSetTimeouts(s.h, 10000, 10000, 15000, 30000);
    Handle c{WinHttpConnect(s.h, u.host.c_str(), u.port, 0)};
    Handle r{c.h ? WinHttpOpenRequest(c.h, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
                 : nullptr};
    const std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
    if (!r.h || !WinHttpSendRequest(r.h, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r.h, nullptr)) {
        error = netError(GetLastError());
        return Got::Failed;
    }
    {
        // the address the answer came from, after the redirects: GitHub's asset storage, not just anywhere
        DWORD bytes = 0;
        WinHttpQueryOption(r.h, WINHTTP_OPTION_URL, nullptr, &bytes);
        std::wstring finalUrl(bytes / sizeof(wchar_t) + 1, L'\0');
        if (bytes == 0 || !WinHttpQueryOption(r.h, WINHTTP_OPTION_URL, finalUrl.data(), &bytes)) {
            error = "the download's address cannot be read";
            return Got::Failed;
        }
        finalUrl.resize(wcsnlen(finalUrl.c_str(), finalUrl.size()));
        if (!allowed(toUtf8(finalUrl), api)) {
            error = "the download was redirected away from GitHub: " + toUtf8(finalUrl);
            return Got::Failed;
        }
        if (landed) *landed = toUtf8(finalUrl);
    }
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(r.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (status) *status = code;
    if (code == 404) return Got::NotFound;
    if (code != 200) {
        error = code == 403 || code == 429 ? "GitHub refused (" + std::to_string(code) +
                                                 "): most likely its limit of questions an hour from this network is used up"
                                           : "the server answered " + std::to_string(code);
        return Got::Failed;
    }
    uint64_t total = 0;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        if (quit) {
            error = "stopped";
            return Got::Failed;
        }
        DWORD read = 0;
        if (!WinHttpReadData(r.h, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
            error = netError(GetLastError());
            return Got::Failed;
        }
        if (read == 0) break;
        total += read;
        if (body) {
            if (body->size() + read > 4u * 1024 * 1024) {
                error = "the answer is too large";
                return Got::Failed;
            }
            body->append(buf.data(), read);
        }
        if (out) {
            out->write(buf.data(), read);
            if (!*out) {
                error = "the download could not be written";
                return Got::Failed;
            }
        }
        if (sha) sha->update(buf.data(), read);
        if (percent && expected > 0) *percent = static_cast<int>(std::min<uint64_t>(100, total * 100 / expected));
    }
    return Got::Ok;
}

std::string timeNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof b, "%02u:%02u", static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute));
    return b;
}

fs::path updateFolder() {
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, tmp);
    return fs::path(tmp) / L"DJAP-update";
}

} // namespace

Updater::~Updater() {
    quit_ = true;
    join();
}

UINT Updater::message() {
    static const UINT msg = RegisterWindowMessageW(L"DJAP.Updater");
    return msg;
}

void Updater::join() {
    if (worker_.joinable()) worker_.join();
}

ReleaseInfo Updater::release() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return release_;
}

std::string Updater::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::string Updater::checkedAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checkedAt_;
}

void Updater::finish(HWND notify, WPARAM what, State s, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
    }
    state_ = s;
    if (notify) PostMessageW(notify, message(), what, 0);
}

void Updater::check(HWND notify, const std::string& url, const std::string& current) {
    if (busy()) return;
    join();
    const std::string api = url.empty() ? kLatestUrl : url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = api;
    }
    state_ = State::Checking;
    worker_ = std::thread([this, notify, api, current] {
        // the setups of earlier updates (kept: setup.log, the last update's report)
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(updateFolder(), ec)) {
            const std::wstring ext = f.path().extension().wstring();
            if (_wcsicmp(ext.c_str(), L".exe") == 0 || _wcsicmp(ext.c_str(), L".part") == 0) {
                std::error_code rm;
                fs::remove(f.path(), rm); // one that still runs stays until the next check
            }
        }
        if (!allowed(api, api)) {
            finish(notify, 0, State::Failed, "not an address of GitHub: " + api);
            return;
        }
        std::string body, err;
        DWORD status = 0;
        const Got got = get(api, api, L"application/vnd.github+json", &body, nullptr, nullptr, nullptr, 0, quit_, err,
                            &status);
        ReleaseInfo r;
        if (got == Got::Failed && (status == 403 || status == 429) && api == kLatestUrl) {
            // GitHub's API answers 60 questions an hour per network without a login - every program behind the same
            // router counts. Its web page of the latest release has no such limit: it redirects to the release's
            // tag, and the setup and its .sha256 sit at fixed addresses under it (the checksum is still required)
            std::string page, landed, err2;
            const Got pageGot =
                get(kLatestPage, api, L"text/html", &page, nullptr, nullptr, nullptr, 0, quit_, err2, nullptr, &landed);
            if (pageGot == Got::Ok && releaseFromTagUrl(landed, kRepo, r)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    release_ = r;
                    checkedAt_ = timeNow();
                }
                finish(notify, 0, compareVersions(r.version, current) > 0 ? State::Available : State::UpToDate);
                return;
            }
            // both failed: say both (seventh outside review: only the API's answer was shown)
            finish(notify, 0, State::Failed,
                   err + "; the release page did not help either: " +
                       (pageGot == Got::Ok ? "it did not lead to a release" : err2.empty() ? "not found" : err2));
            return;
        }
        if (got == Got::Failed) {
            finish(notify, 0, State::Failed, err);
            return;
        }
        if (got == Got::NotFound) { // no release published yet
            {
                std::lock_guard<std::mutex> lock(mutex_);
                release_ = ReleaseInfo();
                checkedAt_ = timeNow();
            }
            finish(notify, 0, State::UpToDate, "no release on GitHub yet");
            return;
        }
        if (!parseLatestRelease(body, r, err)) {
            finish(notify, 0, State::Failed, err);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_ = r;
            checkedAt_ = timeNow();
        }
        finish(notify, 0, compareVersions(r.version, current) > 0 ? State::Available : State::UpToDate);
    });
}

void Updater::download(HWND notify) {
    if (busy()) return;
    const ReleaseInfo r = release();
    if (r.setupUrl.empty()) return;
    join();
    std::string api;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        api = url_;
        setup_.clear();
    }
    percent_ = 0;
    state_ = State::Downloading;
    worker_ = std::thread([this, notify, r, api] {
        std::string err;
        // the checksum: GitHub's digest of the asset, else the release's .sha256 file; none - no install
        std::string sha = r.sha256;
        if (sha.empty() && !r.sha256Url.empty() && allowed(r.sha256Url, api)) {
            std::string text;
            if (get(r.sha256Url, api, L"application/octet-stream", &text, nullptr, nullptr, nullptr, 0, quit_, err) == Got::Ok)
                sha = checksumFor(text, r.setupName);
        }
        if (sha.empty()) {
            finish(notify, 1, State::Failed, "GitHub gives no checksum for this setup: it is not installed");
            return;
        }
        if (!allowed(r.setupUrl, api)) {
            finish(notify, 1, State::Failed, "the setup is not on GitHub: " + r.setupUrl);
            return;
        }
        // the file name from the release, reduced to plain characters
        std::wstring name;
        for (wchar_t ch : fromUtf8(r.setupName))
            if (iswalnum(ch) || ch == L'.' || ch == L'-' || ch == L'_') name += ch;
        if (name.size() < 5 || _wcsicmp(name.c_str() + name.size() - 4, L".exe") != 0) name = L"DJAP-Setup.exe";
        std::error_code ec;
        const fs::path dir = updateFolder();
        fs::create_directories(dir, ec);
        const fs::path file = dir / name, part = dir / (name + L".part");
        Sha256 h;
        Got got;
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out) {
                finish(notify, 1, State::Failed, "the download folder is not writable");
                return;
            }
            got = get(r.setupUrl, api, L"application/octet-stream", nullptr, &out, &h, &percent_, r.setupSize, quit_, err);
        }
        if (got != Got::Ok) {
            fs::remove(part, ec);
            finish(notify, 1, State::Failed, got == Got::NotFound ? "the setup is gone from GitHub" : err);
            return;
        }
        if (r.setupSize > 0 && fs::file_size(part, ec) != r.setupSize) {
            fs::remove(part, ec);
            finish(notify, 1, State::Failed, "the download is not as long as GitHub says: it is not installed");
            return;
        }
        if (h.hex() != sha) {
            fs::remove(part, ec);
            finish(notify, 1, State::Failed, "the download does not match GitHub's checksum: it is not installed");
            return;
        }
        fs::remove(file, ec);
        fs::rename(part, file, ec);
        if (ec) {
            finish(notify, 1, State::Failed, "the download could not be kept: " + ec.message());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            setup_ = toUtf8(file.wstring());
            setupSha_ = sha;
        }
        percent_ = 100;
        finish(notify, 1, State::Ready);
    });
}

bool Updater::launch(const fs::path& testIni, std::string& error) {
    std::string setup, sha;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        setup = setup_;
        sha = setupSha_;
    }
    if (state_ != State::Ready || setup.empty()) {
        error = "no downloaded setup";
        return false;
    }
    // this program's folder; a per-user installation (not under Program Files) stays one
    const fs::path dir = programDir();
    bool perUser = true;
    PWSTR pf = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &pf))) {
        const std::wstring d = dir.wstring(), p = pf;
        perUser = !(d.size() >= p.size() && _wcsnicmp(d.c_str(), p.c_str(), p.size()) == 0);
    }
    CoTaskMemFree(pf);
    const std::wstring log = (updateFolder() / L"setup.log").wstring();
    std::wstring params = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE=1 /DIR=\"" + dir.wstring() + L"\" /LOG=\"" + log +
                          L"\"";
    if (perUser) params += L" /CURRENTUSER";
    if (!testIni.empty()) params += L" /RESTARTINI=\"" + testIni.wstring() + L"\"";
    const std::wstring file = fromUtf8(setup);
    // checked once more right before it runs, through a handle that lets nobody change or replace the file until the
    // setup has started (the download folder is the user's own %TEMP%: another program could swap it meanwhile)
    HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (held == INVALID_HANDLE_VALUE) {
        error = "the downloaded setup cannot be read any more: it is not installed";
        return false;
    }
    {
        Sha256 h;
        std::vector<char> buf(1 << 16);
        DWORD got = 0;
        while (ReadFile(held, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0) h.update(buf.data(), got);
        if (sha.empty() || h.hex() != sha) {
            CloseHandle(held);
            error = "the downloaded setup was changed after its check: it is not installed";
            return false;
        }
    }
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    const BOOL started = ShellExecuteExW(&sei);
    const DWORD e = GetLastError();
    CloseHandle(held);
    if (!started) {
        error = e == ERROR_CANCELLED ? "Windows' permission prompt was declined: nothing was installed"
                                     : "the setup could not be started (error " + std::to_string(e) + ")";
        return false;
    }
    return true;
}

} // namespace ap
