// About (the right-click menus' About DJAP...): the wide logo (icon and name on a metal plate) on the brushed metal, the
// version, what the program is, where the settings are kept, the licences, and the updates - Check now, Install
// update and what the last check found, as in the Setup window (client, 2026-10-27). Modal to the main window; OK,
// Enter, Escape or the close box end it.
#pragma once

#include "theme.h"
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

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void setFonts();
    void close();
    void loadLogo();
    void releaseLogo();
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }

    HWND hwnd_ = nullptr, owner_ = nullptr, text_ = nullptr, pathText_ = nullptr, legal_ = nullptr, ok_ = nullptr;
    HWND checkNow_ = nullptr, install_ = nullptr, updateText_ = nullptr;
    std::function<UpdateView()> view_;
    std::function<void()> checkNowDo_, installDo_;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    std::wstring body_, path_;
    IStream* stream_ = nullptr;
    Gdiplus::Bitmap* logo_ = nullptr;
    theme::Metal metal_;
};

} // namespace ap
