// About (the Setup window's button): the wide logo (icon and name on a metal plate) on the brushed metal, the version, what the program
// is, where the settings are kept and the licences. Modal to the main window; OK, Enter, Escape or the close box end
// it.
#pragma once

#include "theme.h"

#include <windows.h>

#include <filesystem>
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
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    std::wstring body_, path_;
    IStream* stream_ = nullptr;
    Gdiplus::Bitmap* logo_ = nullptr;
    theme::Metal metal_;
};

} // namespace ap
