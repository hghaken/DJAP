// SPDX-License-Identifier: GPL-3.0-or-later
// The Health window (client, 2026-11-05: "the health part in a window of its own, from the right-click menu, with line
// graphs of the health values"; it was a section of the Setup window): what runs, the counters with Reset counters, and
// line graphs of the last minute, 10 minutes or hour - the processing load, the problems per second, the clock drift
// and the drift buffer. The history (HealthHistory) is kept by the main window, also while this window is closed.
#pragma once

#include "healthhistory.h"
#include "theme.h"
#include "tooltips.h"

#include <windows.h>

#include <functional>
#include <string>

namespace ap {

class HealthWindow {
public:
    struct Source {
        std::function<std::wstring()> state, counters; // the texts: what runs, the counters
        std::function<void()> reset;                   // Reset counters
        std::function<int()> decks;                    // 1 or 2: the graphs' names of the lines
    };
    HealthWindow(const HealthHistory& history, Source source);
    ~HealthWindow();
    HealthWindow(const HealthWindow&) = delete;
    HealthWindow& operator=(const HealthWindow&) = delete;
    void show(HWND owner);
    void close();
    void refresh(); // once a second, from the owner's timer
    HWND hwnd() const { return hwnd_; }

private:
    static constexpr int kPanels = 4;
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK graphProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void setFonts();
    void layout();
    void paintGraphs(HDC dc, const RECT& rc);
    int px(double v) const { return static_cast<int>(v * dpi_ / 96.0 + 0.5); }

    const HealthHistory& history_;
    Source source_;
    HWND hwnd_ = nullptr, state_ = nullptr, counters_ = nullptr, span_ = nullptr, reset_ = nullptr, graph_ = nullptr;
    HFONT font_ = nullptr, small_ = nullptr;
    UINT dpi_ = 96;
    int spanSeconds_ = 600;
    int stateLines_ = 1;
    long long drawn_ = -1; // the history's point the graphs show
    std::wstring stateShown_, countersShown_;
    RECT tipRect_[kPanels] = {};
    theme::Metal metal_;
    Tooltips tips_, graphTips_;
};

} // namespace ap
