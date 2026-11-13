// SPDX-License-Identifier: GPL-3.0-or-later
// The Sound window (client, 2026-11-13: "combine the Loudness and the EQ into one setting ... an Edit button where the
// Loudness and the EQ settings are made apart"): a sound's loudness preset and EQ, each with its list and an Edit
// button that opens the loudness or the EQ window; Save as, Rename, Delete and Reset of the sound. A change is heard
// at once on every deck that uses the sound and saved at once. One modeless window, owned by the main window (or the
// plug-in's), opened by the Edit button beside a deck's Sound list.
#pragma once

#include "settings.h"
#include "theme.h"
#include "tooltips.h"

#include <windows.h>

#include <functional>
#include <string>

namespace ap {

class SoundWindow {
public:
    // After every change; listChanged = sounds were added, renamed or deleted. The owner saves and applies.
    using Changed = std::function<void(bool listChanged)>;
    // An Edit button: open the loudness (Dynamics) or the EQ (Colour) window on this preset.
    using OpenEditor = std::function<void(PresetKind kind, const std::string& preset, int deck)>;
    SoundWindow(Settings& settings, Changed onChange, OpenEditor openEditor);
    ~SoundWindow();
    SoundWindow(const SoundWindow&) = delete;
    SoundWindow& operator=(const SoundWindow&) = delete;

    // Shows the window with `sound` (empty: the deck's parts, no sound yet); `deck` (0 / 1) is the strip it was
    // opened from: "Save as" moves that deck to the new sound.
    void show(HWND owner, const std::string& sound, int deck);
    // The sounds, presets or colours changed elsewhere.
    void refresh();
    HWND hwnd() const { return hwnd_; }
    // What the window calls deck A (the plug-in: "this plug-in"), and whether there is a deck B.
    void setDeckName(const std::wstring& name) { deckA_ = name; }
    // The module whose window class this is: the program (by default), or the plug-in DLL - which removes it before
    // it is unloaded.
    static void setModule(HINSTANCE instance);
    static void unregisterClass();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void createControls();
    void setFonts();
    void layout();
    void fill();   // the three lists and the texts
    void select(const std::string& sound);
    void command(int id, int code);
    void choosePart(PresetKind kind); // a choice in the Loudness or the EQ list
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }
    std::wstring usersOf(const std::string& sound) const; // "deck A", "deck A and deck B", ""

    Settings& settings_;
    Changed onChange_;
    OpenEditor openEditor_;
    HWND hwnd_ = nullptr, owner_ = nullptr;
    HWND soundLabel_ = nullptr, sound_ = nullptr, saveAs_ = nullptr, rename_ = nullptr, delete_ = nullptr,
         reset_ = nullptr, note_ = nullptr;
    HWND partLabel_[2] = {}, part_[2] = {}, edit_[2] = {}, about_[2] = {}; // [0] loudness, [1] EQ
    HFONT font_ = nullptr, bold_ = nullptr;
    UINT dpi_ = 96;
    Tooltips tips_;
    theme::Metal metal_;
    std::string current_; // the sound shown (empty: the deck has none)
    int deck_ = 0;
    std::wstring deckA_ = L"deck A";
    bool filling_ = false;
    RECT line_ = {}; // the line between the sound and its parts, painted by the window
};

} // namespace ap
