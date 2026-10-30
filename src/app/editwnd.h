// SPDX-License-Identifier: GPL-3.0-or-later
// Preset window (docs/design.md 3, 3.1, 8): a tab per stage with its on / off switch first and a row per
// parameter - name, slider, value (typed values are taken too), unit, a small reset button (back to its reset value)
// and what it does. Save as, rename, delete, reset preset. Every change is heard at once on each deck using the
// preset and saved at once (a slider when it is let go). Tooltips on the buttons and on every row. One modeless
// window, owned by the main window; the same window edits colour presets (tabs Tone, Auto tone, Sub bass, Contour).
#pragma once

#include "settings.h"
#include "stages.h"
#include "theme.h"
#include "tooltips.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ap {

class EditWindow {
public:
    // Called after every change; listChanged = presets were added, renamed or deleted (or a deck changed preset);
    // save = false while a slider is dragged (heard, saved when it is let go).
    using Changed = std::function<void(bool listChanged, bool save)>;
    // kind: presets (dynamics; with the stage switches) or colour presets
    EditWindow(Settings& settings, Changed onChange, PresetKind kind = PresetKind::Dynamics);
    ~EditWindow();

    // Shows the window with `preset`; `deck` (0 / 1) is the strip it was opened from: "Save as" moves that deck to
    // the new preset. `tab`: the stage whose tab to show (a click on its bar in the analysis window); empty = as it was.
    void show(HWND owner, const std::string& preset, int deck, const std::string& tab = {});
    // Values or presets changed elsewhere (the main window's bass sliders).
    void refresh();
    HWND hwnd() const { return hwnd_; }
    // What the window calls deck A (the plug-in: "this plug-in").
    void setDeckName(const std::wstring& name) { deckA_ = name; }
    // A fixed title bar instead of "Loudness: <preset>" (the plug-in: "Settings Loudness: DJAP Deck 1" - client, 2026-10-30:
    // the preset's name in the title is superfluous, the list shows it).
    void setWindowTitle(const std::wstring& title) { windowTitle_ = title; }
    // The module whose window classes these are: the program (by default), or the plug-in DLL - which removes them
    // before it is unloaded (a class left behind would point into freed code).
    static void setModule(HINSTANCE instance);
    static void unregisterClasses();

private:
    // The controls of one parameter's row (created once for every parameter of the kind; a tab shows its own).
    struct Row {
        int param = -1;
        HWND name = nullptr, slider = nullptr, check = nullptr, value = nullptr, unit = nullptr, reset = nullptr,
             help = nullptr;
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK editProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void createControls();
    void createRows();
    void setFonts();
    void layout();
    int place(int cw, int ch, const std::string& stage, bool move); // lays out (move) or measures
    int neededHeight(int cw);                                        // the tallest tab's height at this width
    void fit();                                                      // grow to the minimum size if smaller
    void fillPresets();
    void fillTabs();       // tab labels (a stage that is off says so)
    void showRows();       // the current tab's rows (the band count decides which density rows exist), laid out
    void updateRows();
    void updateRow(size_t r, bool moveSlider = true);
    void updateButtons();
    void updateAbout(); // the description under the choice
    int textHeight(HWND h, const std::wstring& text, int width) const; // wrapped, in the window's font
    void select(const std::string& preset);
    void selectTab(const std::string& stage);
    bool setValue(int param, double value, bool save = true);
    void commitValue(size_t r); // the typed text of a row's value field
    void slider(HWND bar, int code); // WM_HSCROLL
    void command(int id, int code);
    int rowOf(HWND control) const;
    bool shown(size_t r) const;
    bool shownOn(size_t r, const std::string& stage) const;
    std::wstring nameOf(size_t r) const;
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }
    bool colour() const { return kind_ == PresetKind::Colour; }
    // the preset (or colour) deck d uses
    std::string& used(int d) { return colour() ? settings_.deck[d].colour : settings_.deck[d].preset; }

    Settings& settings_;
    Changed onChange_;
    PresetKind kind_;
    HWND hwnd_ = nullptr, owner_ = nullptr;
    HWND presetLabel_ = nullptr, preset_ = nullptr, saveAs_ = nullptr, rename_ = nullptr, delete_ = nullptr,
         resetPreset_ = nullptr, note_ = nullptr, tabs_ = nullptr;
    HWND soundTitle_ = nullptr, sound_ = nullptr; // under the rows: what the stage does to the sound
    HWND about_ = nullptr;                         // under the choice: what the chosen preset or colour does
    std::wstring aboutText_, noteText_;
    int aboutH_ = 0, noteH_ = 0;
    static constexpr int kMinClientWidth = 800; // at 96 DPI: the buttons beside the choice, the rows' columns
    // the line above it, painted by the window itself (an etched static control left old pixels in its rect)
    RECT soundLine_ = {};
    HFONT font_ = nullptr, bold_ = nullptr, symbol_ = nullptr; // symbol_: the reset buttons' arrow
    UINT dpi_ = 96;
    Tooltips tips_;
    theme::Metal metal_;
    std::string current_;
    int deck_ = 0;
    std::wstring deckA_ = L"deck A";
    std::wstring windowTitle_; // empty: "Loudness: <preset>" / "Colour: <colour>"
    std::vector<std::string> tabStages_; // the stage of each tab
    std::string tab_;                    // the stage shown
    std::vector<Row> rows_;
    bool filling_ = false;
};

// Asks for a preset name in a small modal window. Returns false when cancelled.
bool askName(HWND owner, const std::wstring& title, const std::wstring& prompt, std::string& name);

} // namespace ap
