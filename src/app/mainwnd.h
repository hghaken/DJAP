// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: two deck strips side by side (loudness preset and colour - one set, on deck A, when the presets are
// linked -, processing / bypass, Graphs, meters), no menu bar (a right-click opens Setup / Shutdown)
// and a status bar (audio state, latency added by the processing). The stage switches are in the preset window.
// Every control has a tooltip that says what it does. Every change is heard at once and saved at once.
// The program lives in the notification area: closing or minimising the window hides it there and the audio goes
// on; Shutdown in the icon's menu or the window's right-click menu ends the program.
#pragma once

#include "aboutwnd.h"
#include "analysiswnd.h"
#include "audiodlg.h"
#include "audioio.h"
#include "editwnd.h"
#include "settings.h"
#include "spectrumwnd.h"
#include "theme.h"
#include "tooltips.h"
#include "tray.h"
#include "updater.h"
#include "winvolume.h"

#include <windows.h>

#include <filesystem>

namespace ap {

class MainWindow {
public:
    MainWindow(Settings& settings, std::filesystem::path iniFile, AudioIO& audio);
    // `hidden`: straight into the notification area (started with Windows). `testRun` (--ini / --simulate): the
    // start-with-Windows switch is greyed out, so a test never changes the user's logon programs.
    bool create(HINSTANCE instance, bool hidden, bool testRun);
    // The audio of the last session at program start (StartAudio): the devices may not be there yet just after
    // Windows starts (a USB interface, a virtual cable), so a failed start is tried again every 5 s for 2 minutes.
    void startSavedAudio();
    HWND hwnd() const { return hwnd_; }
    HWND editor() const { return editor_.hwnd(); }
    // Shows a message in the status bar (e.g. an audio start at program start that failed).
    void setMessage(const std::string& text);
    // Saves nothing from now on (the settings file could not be read at the start); `why` shows in the status bar.
    void holdSaves(const std::string& why);
    // Publishes both decks' configurations (after the audio side changed its sample rate).
    void applyAll();
    void openAudioWindow(); // the first start: no device chosen yet
    // A second start of the program finds the first one's window by its class and posts it showMessage().
    static const wchar_t* windowClass();
    static UINT showMessage();

private:
    struct Strip {
        HWND title = nullptr, route = nullptr, presetLabel = nullptr, preset = nullptr, processing = nullptr, // bypass
             edit = nullptr, meter = nullptr, graphs = nullptr; // graphs: opens the analysis window
        HWND colourLabel = nullptr, colour = nullptr, editColour = nullptr;          // the deck's colour preset
        HWND routeOut = nullptr; // route = where the audio comes from, routeOut = where it goes (two lines)
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void createControls();
    void setFonts();
    void layout();
    void command(int id, int code);
    void fillPresets(int d); // and colours
    void presetsChanged(bool listChanged, bool saveNow); // from a preset or colour window
    void applyDeck(int d);
    void refreshStrip(int d);
    void updateMeters();
    void updateVolume(); // the outputs follow Windows' volume (Audio window switch)
    void updateStatus();
    void updateTrayTip();
    void toTray();       // hide the window (and its own windows); the icon stays
    void fromTray();     // show it again in front
    void trayMenu(int x, int y);
    void windowMenu(int x, int y); // right-click: Setup, Shutdown
    void checkForUpdate();          // asks GitHub (the daily check, or Check now)
    void installUpdate();           // Install update: download, check, start the setup, end the program
    void updateEvent(WPARAM what);  // a check (0) or a download (1) of the updater ended
    void save();
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }
    // where layout() puts the meter panels: under the title row and the preset row (the bass sliders moved to the
    // spectrum window, client 2026-10-26)
    int meterTop() const { return px(12) + 2 * (px(28) + px(6)); }
    // the smallest client widths at 96 DPI (the height follows from the meter panel: WM_GETMINMAXINFO)
    static constexpr int kMinWidthOne = 600, kMinWidthTwo = 900;

    Settings& settings_;
    std::filesystem::path ini_;
    Settings base_; // the preset lists as last read or written (the plug-in may add its own: Settings::saveMerged)
    AudioIO& audio_;
    HWND hwnd_ = nullptr, status_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr;
    UINT dpi_ = 96;
    Strip strips_[2];
    int layoutDecks_ = 0; // the strips were laid out for this many decks
    // presets linked: only with two decks
    bool linked() const { return settings_.linkPresets && settings_.decks == 2; }
    Tooltips tips_;
    theme::Metal metal_;
    EditWindow editor_, colourEditor_;
    AudioWindow audioWindow_;
    AnalysisWindow analysisWindow_;
    SpectrumWindow spectrumWindow_;
    AboutWindow about_;
    std::string saveError_, message_, holdSaves_;
    int ticks_ = 0;
    WindowsVolume winVolume_;
    double shownVolume_ = -3.0; // the volume in the status bar (-2 unknown, -1 muted)
    // the program's CPU use, as Task Manager shows it (% of the whole computer), measured over about a second
    double cpuPercent_ = -1.0;
    unsigned long long cpuLastBusy_ = 0, cpuLastWall_ = 0;
    double audioLastBusy_ = -1.0, audioPercent_ = -1.0; // the audio thread's processing: its share of one core
    int audioCore_ = -1;
    unsigned long long quietSince_ = 0; // GetTickCount64 when the decks last had sound (or the audio stopped)
    void measureCpu();
    TrayIcon tray_;
    HICON iconSmall_ = nullptr;
    bool quitting_ = false; // Shutdown chosen (or Windows ends the session): WM_CLOSE really closes
    bool testRun_ = false;
    int startTries_ = 0; // attempts of startSavedAudio() so far
    Updater updater_;
    ULONGLONG nextCheck_ = 0;   // GetTickCount64() of the next daily update check
    std::string notified_;      // the version the notification-area message was shown for
};

} // namespace ap
