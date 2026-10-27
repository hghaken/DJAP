// Setup window (client, 2026-10-25: "all settings in one window"; was the Audio window, docs/design.md section 6):
// the program-wide settings - limiter look-ahead, start with Windows, an About button - and the audio API - ASIO, WASAPI (shared or exclusive), DirectSound, MME.
// ASIO: the driver (from the registry; opened only on request, after a warning), the channel pair of each deck's
// input and output, sample rate, buffer size, the driver's control panel. Other APIs: each deck's input and output
// device (or none), sample rate, period. With ASIO the decks' inputs may instead come from WASAPI / DirectSound /
// MME devices ("Deck inputs"; drift-corrected, docs/design.md section 6.1). Start / stop, the latency (with such an
// input: its buffer and the measured drift), and the real-time health counters (handbook 11.7). Choices are saved
// at once.
#pragma once

#include "audioio.h"
#include "settings.h"
#include "theme.h"
#include "tooltips.h"
#include "updater.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ap {

class AudioWindow {
public:
    using Changed = std::function<void()>; // settings changed or the device started / stopped
    AudioWindow(Settings& settings, AudioIO& audio, Changed onChange);
    ~AudioWindow();
    void show(HWND owner);
    void refresh(); // from the main window's timer: state and health
    HWND hwnd() const { return hwnd_; }
    // Test runs (--ini / --simulate) never change the user's logon programs: the start-with-Windows box is greyed.
    void setTestRun(bool testRun) { testRun_ = testRun; }
    // The About button (the About window belongs to the main window).
    void onAbout(std::function<void(HWND owner)> f) { onAbout_ = std::move(f); }
    // Updates: the main window's updater (its state is shown here) and what Check now / Install update do.
    void setUpdater(const Updater* updater, std::function<void()> checkNow, std::function<void()> install) {
        updater_ = updater;
        checkNowDo_ = std::move(checkNow);
        installDo_ = std::move(install);
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void createControls();
    void setFonts();
    void layout();
    void fillAll();     // API, driver list, pairs or devices, rates, buffer sizes
    void command(int id, int code);
    bool openDriver();  // ASIO; asks first
    // Starts (or, while running, restarts) the audio with the settings; asks before an exclusive open when asked to.
    bool startAudio(bool askExclusive);
    void readChoices(); // combos -> settings
    bool asio() const;
    bool mixed() const; // ASIO, with the decks' inputs from devices of another API
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }

    Settings& settings_;
    AudioIO& audio_;
    Changed onChange_;
    HWND hwnd_ = nullptr, owner_ = nullptr;
    HWND labels_[14] = {};
    HWND decks_ = nullptr, api_ = nullptr, exclusive_ = nullptr, drift_ = nullptr, link_ = nullptr, inputs_ = nullptr, follow_ = nullptr;
    HWND driver_ = nullptr, open_ = nullptr, close_ = nullptr, panel_ = nullptr, info_ = nullptr;
    HWND inA_ = nullptr, outA_ = nullptr, inB_ = nullptr, outB_ = nullptr, rate_ = nullptr, buffer_ = nullptr;
    HWND start_ = nullptr, stop_ = nullptr, state_ = nullptr, health_ = nullptr, resetHealth_ = nullptr,
         note_ = nullptr;
    HWND look_ = nullptr, startup_ = nullptr, about_ = nullptr; // look-ahead, start with Windows, About
    HWND oversampling_ = nullptr; // the soft clipper's and the exciter's
    HWND tooltips_ = nullptr;                                    // the switch of all tooltips
    HWND checkUpdates_ = nullptr, checkNow_ = nullptr, install_ = nullptr, updateInfo_ = nullptr; // updates
    const Updater* updater_ = nullptr;
    std::function<void()> checkNowDo_, installDo_;
    bool testRun_ = false;
    std::function<void(HWND)> onAbout_;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    Tooltips tips_;
    theme::Metal metal_;
    bool filling_ = false;
    std::vector<long> bufferSizes_; // 0 = the driver's preferred size / the API's default
    std::vector<double> rates_;
    std::vector<SysDevice> sysInputs_, sysOutputs_; // devices of the chosen API (not ASIO; mixed: the inputs' API)
    std::string listError_;
};

} // namespace ap
