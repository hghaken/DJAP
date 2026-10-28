// Tooltips: one tooltip control per window, several lines wide, shown for 30 s while the mouse rests on a control
// (or on a rectangle of a custom-drawn window). The texts say what a control does; they are copied when added.
// One switch for all of them (client, 2026-10-26: "an option to turn all tooltips on or off", in Setup).
#pragma once

#include <windows.h>

#include <vector>

namespace ap {

class Tooltips {
public:
    // Creates the tooltip control for `owner` (destroyed with it). Call again after the owner was re-created.
    void create(HWND owner);
    // The whole control (a static control needs SS_NOTIFY to see the mouse). Again: replaces the text.
    void add(HWND control, const wchar_t* text);
    // A rectangle `id` of `window` (client coordinates); move it with setRect when the layout changes.
    void addRect(HWND window, UINT_PTR id, const wchar_t* text);
    void setRect(HWND window, UINT_PTR id, const RECT& r);
    // Line width for the window's DPI.
    void setDpi(UINT dpi);
    HWND hwnd() const { return tip_; }
    // Every tooltip of this module on or off at once (those created later too). GUI thread only.
    static void setEnabled(bool on);
    static bool enabled();

private:
    HWND tip_ = nullptr, owner_ = nullptr;
    std::vector<HWND> added_; // controls with a tool: a new text for one updates it in place
};

} // namespace ap
