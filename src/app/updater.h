// SPDX-License-Identifier: GPL-3.0-or-later
// Updates (docs/design.md section 8, "Updates"; client, 2026-10-25: "check daily, report, install after one click
// and an off switch in Setup"; 2026-10-26: running audio starts again after the update). Asks GitHub for the latest release, downloads its setup into
// %TEMP%\DJAP-update, checks it against the SHA-256 GitHub publishes for it (no checksum: no install), and starts it
// silently for the program's own folder; the program then ends and the setup starts it again. Only https from
// GitHub (or, for tests, the host of the settings' Url); all network work on a thread of its own.
#pragma once

#include "release.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ap {

// What the Setup and About windows show of the updater: a line of text and whether Check now / Install update work.
struct UpdateView {
    std::wstring text;
    bool canCheck = false, canInstall = false;
};

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
    static constexpr const char* kLatestUrl = "https://api.github.com/repos/hghaken/DJAP/releases/latest";
    // the same release's web page: asked when the API refuses (its hourly limit per network used up); it must lead to a
    // release tag of this repository
    static constexpr const char* kRepo = "https://github.com/hghaken/DJAP";
    static constexpr const char* kLatestPage = "https://github.com/hghaken/DJAP/releases/latest";

    Updater() = default;
    ~Updater(); // waits for a running check or download
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // GUI thread. When a check (wParam 0) or a download (wParam 1) ends, `notify` gets message() (posted).
    // `url`: the settings' Url, empty = GitHub. `current`: this program's version.
    void check(HWND notify, const std::string& url, const std::string& current);
    void download(HWND notify); // after Available
    // Starts the downloaded setup (Ready): silent, for this program's folder, the program started again afterwards.
    // False (and why) when Windows refused - the permission prompt declined, for example. `testIni`: a test run's
    // settings file (--ini), which the program is started again with; empty for the real program.
    bool launch(const std::filesystem::path& testIni, std::string& error);

    static UINT message();
    State state() const { return state_.load(); }
    bool busy() const { return state_ == State::Checking || state_ == State::Downloading; }
    int percent() const { return percent_.load(); } // of the download
    ReleaseInfo release() const;
    std::string error() const;
    std::string checkedAt() const; // the time of the last check that answered ("14:02")

private:
    void finish(HWND notify, WPARAM what, State s, const std::string& error = {});
    void join();

    std::atomic<State> state_{State::Idle};
    std::atomic<int> percent_{0};
    std::atomic<bool> quit_{false};
    mutable std::mutex mutex_; // release_, error_, setup_, setupSha_, checkedAt_, url_
    ReleaseInfo release_;
    std::string error_, setup_, setupSha_, checkedAt_, url_; // setupSha_: the checksum it was checked against
    std::thread worker_;
};

// The line and buttons the About window shows of `u` (the program's Setup / About and the plug-in's About share it).
// `checkEnabled`: the settings' daily check; `audioRuns`: the program's audio would pause for the install;
// `installHere`: the program installs the update itself - the plug-in (inside DJ software) opens the release's page.
UpdateView updateViewOf(const Updater& u, bool checkEnabled, bool audioRuns, bool installHere);

} // namespace ap
