// SPDX-License-Identifier: GPL-3.0-or-later
// The look of the main, preset and Audio windows (docs/design.md section 8): a brushed aluminium background - a light
// to darker gradient under a random pattern of fine horizontal brush strokes - and dark 3D push buttons that light up
// green while checked (owner-drawn, anti-aliased with GDI+). The meter panel and the analysis graphs keep their own
// dark look; small prompts keep the system look. Edit fields, drop-down lists and tabs are a light grey in the
// metal's tint instead of white.
#pragma once

#include <windows.h>

namespace ap::theme {

void init(); // GDI+ start-up: before the first window is created
void shutdown();

// The background of one top-level window: its whole client area rendered once per size into a bitmap, painted from
// there and handed to the child controls as a pattern brush (aligned to each control), so the strokes run on behind
// labels, check boxes and sliders without a seam.
class Metal {
public:
    Metal() = default;
    ~Metal();
    Metal(const Metal&) = delete;
    Metal& operator=(const Metal&) = delete;

    // The messages of the top-level window `top` this look takes care of: WM_ERASEBKGND, WM_PRINTCLIENT,
    // WM_CTLCOLORSTATIC / BTN (labels, check boxes, sliders on the metal; edit fields stay white), WM_DRAWITEM of the
    // 3D buttons, and WM_SIZE (re-renders; returns false so the window lays itself out too). True = handled.
    bool handle(HWND top, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    // Fills `r` of `dc`, in client coordinates of `child` (the top-level window itself or a window inside it).
    void paint(HDC dc, const RECT& r, HWND child);
    // A tab control drawn on this metal: tabs with rounded tops, the chosen one in the field grey (the Metal must
    // outlive the control).
    void makeTabs(HWND tabs);

private:
    void render(HWND top);
    void release();
    HBRUSH controlBrush(HDC dc, HWND child);

    HWND top_ = nullptr;
    HDC mem_ = nullptr;
    HBITMAP bmp_ = nullptr, old_ = nullptr;
    HBRUSH brush_ = nullptr;
    int w_ = 0, h_ = 0;
};

// Makes a push button (created with BS_OWNERDRAW) a 3D button: hover, pressed, disabled and focus states; its parent
// window's Metal::handle draws it.
void makeButton(HWND button);
// A drop-down list (CBS_DROPDOWNLIST) with its field in light grey instead of white; the list itself is grey through
// WM_CTLCOLORLISTBOX.
void makeCombo(HWND combo);
// A toggle button (Bypass): checked = pushed in and lit green.
void setChecked(HWND button, bool on);
bool isChecked(HWND button);

} // namespace ap::theme
