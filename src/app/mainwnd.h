// Main window: two deck strips side by side (preset and four bass sliders that change the preset - one set, on
// deck A, when the presets are linked -, processing / bypass, Graphs, meters), no menu bar (a right-click opens Setup / Exit)
// and a status bar (audio state, latency added by the processing). The stage switches are in the preset window.
// Every control has a tooltip that says what it does. Every change is heard at once and saved at once.
// The program lives in the notification area: closing or minimising the window hides it there and the audio goes
// on; Exit in the icon's menu or the window's right-click menu ends the program.
#pragma once

#include "aboutwnd.h"
#include "analysiswnd.h"
#include "audiodlg.h"
#include "audioio.h"
#include "editwnd.h"
#include "settings.h"
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
        HWND sliderLabel[4] = {}, slider[4] = {}, sliderValue[4] = {}; // bass level, enhancer, width, frequency
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void createControls();
    void setFonts();
    void layout();
    void command(int id, int code);
    void slider(HWND bar, int code); // WM_HSCROLL of a bass slider
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
    void windowMenu(int x, int y); // right-click: Setup, Exit
    void checkForUpdate();          // asks GitHub (the daily check, or Check now)
    void installUpdate();           // Install update: download, check, start the setup, end the program
    void updateEvent(WPARAM what);  // a check (0) or a download (1) of the updater ended
    void save();
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }

    Settings& settings_;
    std::filesystem::path ini_;
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
    AboutWindow about_;
    std::string saveError_, message_;
    int ticks_ = 0;
    WindowsVolume winVolume_;
    double shownVolume_ = -3.0; // the volume in the status bar (-2 not following, -1 muted)
    TrayIcon tray_;
    HICON iconSmall_ = nullptr;
    bool quitting_ = false; // Exit chosen (or Windows ends the session): WM_CLOSE really closes
    bool testRun_ = false;
    int startTries_ = 0; // attempts of startSavedAudio() so far
    Updater updater_;
    ULONGLONG nextCheck_ = 0;   // GetTickCount64() of the next daily update check
    std::string notified_;      // the version the notification-area message was shown for
};

} // namespace ap
