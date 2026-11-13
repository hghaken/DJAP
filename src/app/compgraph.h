// SPDX-License-Identifier: GPL-3.0-or-later
// The graphs of the loudness window's Compressor tab (client, 2026-11-13: "turn these sliders into a graphical view,
// so it is clearer what they do to the audio - maybe the live audio in the background"; "as compact as possible",
// "sizable"): five panels in one strip that takes whatever size it is given -
//   the curve (in -> out) with the threshold, the knee, the ratio and the make-up, the live working point on it and
//     where the music's level was in the last seconds;
//   the attack: the first 150 ms of a hit 10 dB over the threshold - what still gets through;
//   the release: the gain on the rest of the music after that hit;
//   the level and the reduction of the last 4 s (live);
//   the side chain: the music's spectrum and what of it the compressor listens to (its high-pass).
// The round marks can be dragged: threshold, ratio, make-up, attack, release, side-chain high-pass - the sliders
// follow. The attack and the release have no slider on the tab (client, 2026-11-13: "the attack and release slider
// can go, make a small reset icon in the graph"): their graphs are where they are set - the mark, the mouse wheel
// for one step - and a small reset icon in the graph's corner takes the place of the row's reset button.
// One child window, custom-drawn (GDI+ for the shapes, GDI for the texts), refreshed by its owner's timer.
// Compiled into the plug-in too (the loudness window is the same there).
#pragma once

#include "analysiswnd.h"
#include "params.h"

#include <windows.h>

#include <functional>
#include <vector>

namespace Gdiplus {
class Graphics;
}

namespace ap {

class CompressorGraphs {
public:
    static constexpr int kPanels = 5;
    static constexpr int kResets = 2; // the graphs with a reset icon: attack, release (they have no row on the tab)
    // The strip's tooltip rectangles: the panels (0 .. kPanels - 1; of a panel with a reset icon the part left of
    // the icon), the rest of such a panel under its icon (kPanels + k), and the reset icons (kPanels + kResets + k).
    // They do not overlap.
    static constexpr int kTips = kPanels + 2 * kResets;
    // A mark was dragged, the wheel turned or a reset icon clicked: the parameter and its new value (on one of its
    // slider positions); save = false while the mouse button is down on a mark.
    using Edit = std::function<void(int param, double value, bool save)>;

    ~CompressorGraphs();
    void create(HWND parent, HINSTANCE module, UINT dpi);
    HWND hwnd() const { return hwnd_; }
    void setDpi(UINT dpi);
    // The loudness preset shown: its compressor values and the level its threshold is relative to; `reset`: the
    // values the reset icons go back to (an icon is lit when its value differs).
    void setValues(const PresetValues& values, const PresetValues& reset);
    // Whose audio is drawn behind the graphs (null: none - a preset no deck plays).
    void setLive(const AnalysisSource* source, int deck);
    void onEdit(Edit f) { edit_ = std::move(f); }
    // From the owner's timer: draws again when it is visible.
    void refresh();
    // Tooltip rectangle i in the window's client coordinates (as its size is now), and its text - null for a reset
    // icon: the owner's, which knows the reset value's text.
    RECT tipRect(int i);
    static const wchar_t* tipText(int i);
    static int resetParam(int k); // the parameter of reset icon k
    // The smallest height the strip is useful at (at this DPI).
    static int minHeight(UINT dpi) { return static_cast<int>(132.0 * dpi / 96.0 + 0.5); }

    static void registerClass(HINSTANCE module);
    static void unregisterClass(HINSTANCE module);

private:
    enum Mark { mNone = -1, mThreshold, mRatio, mMakeup, mAttack, mRelease, mSideHpf, kMarks };
    // A panel's plot area and its scales (value -> pixel and back).
    struct Plot {
        double l = 0, t = 0, r = 1, b = 1; // pixels
        double x0 = 0, x1 = 1, y0 = 0, y1 = 1; // values at left / right, bottom / top
        bool logX = false;
        double X(double v) const;
        double Y(double v) const;
        double fromX(double px) const;
        double fromY(double py) const;
    };
    struct Label {
        int x, y;
        std::wstring text;
        COLORREF colour;
        UINT align;
    };

    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC target);
    void renderStill(HDC dc, int w, int h, int th); // what does not move with the audio
    void paintLive(HDC dc, int th);                 // what does
    void drawMark(Gdiplus::Graphics& g, int id, double x, double y);
    void layoutPanels(int w, int h);
    int markAt(POINT pt) const;
    int resetAt(POINT pt) const; // the reset icon under the point that has something to reset, or -1
    bool resetLit(int k) const;  // its value differs from the reset value
    void change(int param, double value, bool save); // a new value from the graph: shown, then told to the owner
    void drag(POINT pt);
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }
    // the compressor's numbers from values_
    double aim() const;
    double threshold() const; // LU to the aim
    double makeup() const;    // dB (the automatic one worked out)
    double out(double in) const; // LU to the aim -> LU to the aim, through the curve and the make-up
    double hitReduction() const; // dB the example hit (10 dB over the threshold) is turned down when settled

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, symbol_ = nullptr; // symbol_: the reset icons' arrow
    UINT dpi_ = 96;
    PresetValues values_ = defaultValues(), resetValues_ = defaultValues();
    RECT reset_[kResets] = {}; // the reset icons (layoutPanels)
    int press_ = -1;           // the reset icon the mouse button went down on
    int wheel_ = 0;            // wheel movement not yet a whole step
    const AnalysisSource* source_ = nullptr;
    int deck_ = 0;
    Edit edit_;
    RECT panel_[kPanels] = {};
    Plot plot_[kPanels];
    POINT mark_[kMarks] = {};
    int drag_ = mNone;
    HBITMAP still_ = nullptr, frame_ = nullptr; // the still picture, and the frame being drawn
    int bitmapW_ = 0, bitmapH_ = 0;
    bool stillDirty_ = true;
};

} // namespace ap
