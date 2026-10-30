// SPDX-License-Identifier: GPL-3.0-or-later
// Analysis window (docs/design.md section 8; the Graphs button of a deck strip): for each deck a row of graphs - the signal
// chain from input to output with a bar per step (what each step does to the level now), the compressor's curve with
// its working point, the last 60 s of loudness (original and processed, LU to the target),
// the last 60 s of the leveller / compressor / density gains, a goniometer of the output, and bars for the output's
// correlation and the input's and output's stereo width. Custom-drawn, refreshed by its owner's timer; every
// graph has a tooltip. One modeless window, owned by the main window - or by the plug-in's window (one deck).
#pragma once

#include "monitor.h"
#include "params.h"
#include "tooltips.h"

#include <windows.h>

#include <functional>
#include <string>

namespace ap {

// What the window shows: the program's decks (one or two) or the plug-in's one deck.
class AnalysisSource {
public:
    virtual ~AnalysisSource() = default;
    virtual int decks() const = 0; // the rows: 1 or 2
    virtual bool audioActive() const = 0;
    virtual const DeckMeters& meters(int d) const = 0;
    virtual const DeckAnalysis& analysis(int d) const = 0;
    virtual PresetValues values(int d) const = 0; // the deck's chain values: its loudness preset with its colour
    virtual std::wstring title(int d) const = 0;  // "Deck A   Medium"
    virtual bool processing(int d) const = 0;     // false: bypassed
    virtual std::wstring windowTitle() const { return L"Analysis"; } // the plug-in's names its deck
};

class AnalysisWindow {
public:
    explicit AnalysisWindow(const AnalysisSource& source);
    ~AnalysisWindow();
    void show(HWND owner);
    void close();   // the window goes (its owner goes)
    void refresh(); // from the owner's timer, 20 times a second
    HWND hwnd() const { return hwnd_; }
    // A click on a step of the signal chain: open its settings - the preset window (colour: the colour window) on
    // the stage's tab (client, 2026-10-25).
    using OpenStage = std::function<void(int deck, const std::string& stage, bool colour)>;
    void onOpenStage(OpenStage f) { openStage_ = std::move(f); }
    // The module the window class belongs to: the program (default) or the plug-in DLL, which removes the class
    // again before it is unloaded.
    static void setModule(HINSTANCE instance);
    static void unregisterClass();

private:
    // compressor, loudness, gain, goniometer, correlation, width in, width out, scope; then a row per step of the chain
    static constexpr int kGraphs = 8, kChainRows = 14, kParts = kGraphs + kChainRows;
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC target);
    void paintDeck(HDC dc, int d, const RECT& area);
    void paintChain(HDC dc, int d, const RECT& panel, bool live);
    void paintScope(HDC dc, int d, const RECT& panel, bool live, int th);
    void setFonts();
    void placeTip(int d, int part, const RECT& r);
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }
    // the chain row under a point (client coordinates): its deck and row, false when none
    bool chainRowAt(POINT pt, int& deck, int& row) const;

    const AnalysisSource& source_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, small_ = nullptr, bold_ = nullptr;
    UINT dpi_ = 96;
    Tooltips tips_;
    RECT tipRect_[2][kParts] = {};
    // the compressor's working point of the last 3 s (a point per refresh, 20 Hz), for the trail on its curve
    static constexpr int kTrail = 60;
    double trail_[2][kTrail] = {};
    int trailPos_ = 0;
    OpenStage openStage_;
    // the scope of each deck: the input or the output (a right-click on it chooses), and where it was painted
    bool scopeInput_[2] = {false, false};
    int scopeMs_[2] = {50, 50}; // the time across it: 1, 5, 10, 20, 50, 200 or 2000 ms (the right-click menu)
    RECT scopeRect_[2] = {};
};

} // namespace ap
