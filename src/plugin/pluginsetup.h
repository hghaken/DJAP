// SPDX-License-Identifier: GPL-3.0-or-later
// The plug-in's Setup window (client, 2026-10-29: "a right-click with the Setup window and About", as in the
// program): what applies to a plug-in - the limiter's look-ahead and the oversampling (its parameters, as in its
// window), and the two switches it shares with the program through the settings file: Show tooltips and the daily
// update check (Settings::saveMerged merges them). No devices: the DJ software chooses the plug-in's input and output.
// On the brushed metal; one modeless window, owned by the host's window.
#pragma once

#include "theme.h"
#include "tooltips.h"

#include <windows.h>

namespace ap::plugin {

class Plugin;

class SetupWindow {
public:
    explicit SetupWindow(Plugin*& plugin) : plugin_(plugin) {}
    ~SetupWindow() { close(); }
    SetupWindow(const SetupWindow&) = delete;
    SetupWindow& operator=(const SetupWindow&) = delete;
    void show(HWND owner);
    void close();
    void refresh(); // from the plug-in window's timer: a host or the main window may have changed a choice
    static void setModule(HINSTANCE instance);
    static void unregisterClass();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void command(int id, int code);
    int px(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }

    Plugin*& plugin_; // the plug-in window's (nullptr after it ended)
    HWND hwnd_ = nullptr, look_ = nullptr, oversampling_ = nullptr, tooltips_ = nullptr, updates_ = nullptr,
         close_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    theme::Metal metal_;
    Tooltips tips_;
};

} // namespace ap::plugin
