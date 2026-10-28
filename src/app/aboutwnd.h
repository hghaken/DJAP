// About (the right-click menus' About DJAP...): the wide logo (icon and name on a metal plate) on the brushed metal, the
// version, what the program is, where the settings are kept, the licences, and the updates - Check now, Install
// update and what the last check found, as in the Setup window (client, 2026-10-27). Modal to the main window; OK,
// Enter, Escape or the close box end it.
#pragma once

#include "monitor.h"
#include "theme.h"
#include "tooltips.h"
#include "updater.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>

struct IStream;
namespace Gdiplus {
class Bitmap;
}

namespace ap {

class AboutWindow {
public:
    AboutWindow() = default;
    AboutWindow(const AboutWindow&) = delete;
    AboutWindow& operator=(const AboutWindow&) = delete;
    void show(HWND owner, const std::filesystem::path& settingsFile);
    // The updater as the Setup window shows it, and what Check now / Install update do (the main window's).
    void setUpdates(std::function<UpdateView()> view, std::function<void()> checkNow, std::function<void()> install) {
        view_ = std::move(view);
        checkNowDo_ = std::move(checkNow);
        installDo_ = std::move(install);
    }
    void refresh(); // the update line and buttons (from the main window's timer and the updater's messages)
    HWND hwnd() const { return hwnd_; }
    // Deck A's output samples (the Graphs window's scope), for the logo's pulse; GUI thread.
    void setBeatSource(std::function<const WaveScope*()> source) { beat_ = std::move(source); }
    // The message loop's key filter: true when `m` was a key of the About window's own (then not dispatched).
    static bool takeKey(const MSG& m);

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void setFonts();
    void close();
    void loadLogo();
    void releaseLogo();
    void paintTop(HDC dc);  // the logo (pulsing) or the credits, over the metal
    void animate();         // a timer tick of the pulse / the credits
    RECT topRect() const;   // the area they use
    double readBeat();      // 0..1: how far the bass now stands over its recent average
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }

    HWND hwnd_ = nullptr, owner_ = nullptr, text_ = nullptr, pathText_ = nullptr, legal_ = nullptr, ok_ = nullptr;
    HWND checkNow_ = nullptr, install_ = nullptr, updateText_ = nullptr;
    std::function<UpdateView()> view_;
    std::function<void()> checkNowDo_, installDo_;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    std::wstring body_, path_;
    std::wstring updateShown_; // the update line as set (compared whole: a long error was set again at every tick)
    Tooltips tips_;
    // five clicks on the logo: it pulses with deck A's bass for a while; Ctrl+Shift+D: the credits roll over it
    std::function<const WaveScope*()> beat_;
    int logoClicks_ = 0;
    ULONGLONG firstClick_ = 0, pulseUntil_ = 0;
    long long beatRead_ = -1;
    double lp_ = 0.0, fast_ = 0.0, slow_ = 0.0, pulse_ = 0.0;
    bool credits_ = false;
    double creditsY_ = 0.0;
    bool animating_ = false;
    IStream* stream_ = nullptr;
    Gdiplus::Bitmap* logo_ = nullptr;
    theme::Metal metal_;
};

} // namespace ap
