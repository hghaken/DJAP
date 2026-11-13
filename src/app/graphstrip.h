// SPDX-License-Identifier: GPL-3.0-or-later
// A strip of graphs on a tab of the loudness window in which the tab's values are set - by dragging marks and
// lines, with the mouse wheel, by small reset icons - with the deck's audio drawn in them. The base of the
// Leveller tab's graphs (levgraph.h), the Compressor tab's (compgraph.h) and the MB Compressor tab's (mbgraph.h):
// the window, the fonts, the bitmaps
// (what does not move with the audio is drawn once; a layer with transparency for still things that lie over
// moving ones), the marks, the reset icons, the steppers, the mouse and the wheel. A strip draws its panels and
// says what a dragged mark means.
// One child window per strip, custom-drawn (GDI+ for the shapes, GDI for the texts and the live areas), refreshed
// by its owner's timer. Compiled into the plug-in too (the loudness window is the same there).
#pragma once

#include "analysiswnd.h"
#include "params.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace Gdiplus {
class Graphics;
class Pen;
} // namespace Gdiplus

namespace ap {

// the meter panel's palette (meterview.cpp, analysiswnd.cpp)
namespace graph {
constexpr COLORREF kBack = RGB(28, 30, 33), kPanel = RGB(20, 22, 25), kEdge = RGB(55, 58, 64),
                   kGrid = RGB(70, 74, 80), kText = RGB(214, 218, 224), kDim = RGB(120, 126, 134),
                   kOrange = RGB(240, 130, 50), kTeal = RGB(80, 215, 195), kInput = RGB(150, 156, 164);
// `fg` at opacity `a` over `bg`: the colour of a blended area, worked out once (the live areas are plain polygons).
COLORREF blend(COLORREF fg, COLORREF bg, double a);
POINT ipt(double x, double y);
std::wstring number(double v, int decimals, const wchar_t* unit, bool sign = false);
} // namespace graph

class GraphStrip {
public:
    // A mark was dragged, the wheel turned or a stepper clicked: the parameter and its new value; save = false while
    // the mouse button is down on a mark.
    using Edit = std::function<void(int param, double value, bool save)>;
    // A reset icon was clicked: the owner sets the parameter back (it knows what belongs to that).
    using Reset = std::function<void(int param)>;
    // A tooltip rectangle (client coordinates). Its text is the strip's own (`text`), or the owner's for a
    // parameter: what the value does and its range (`nameOf`), or what its reset icon goes back to (`resetOf`).
    struct Tip {
        RECT rect;
        const wchar_t* text;
        int nameOf;
        int resetOf;
    };

    virtual ~GraphStrip();
    void create(HWND parent, HINSTANCE module, UINT dpi);
    HWND hwnd() const { return hwnd_; }
    void setDpi(UINT dpi);
    // The loudness preset shown; `reset`: the values the reset icons go back to (an icon is lit when its value
    // differs).
    void setValues(const PresetValues& values, const PresetValues& reset);
    // Whose audio is drawn in the graphs (null: none - a preset no deck plays).
    void setLive(const AnalysisSource* source, int deck);
    void onEdit(Edit f) { edit_ = std::move(f); }
    void onReset(Reset f) { resetTo_ = std::move(f); }
    // From the owner's timer: draws again when it is visible.
    void refresh();
    // The strip's tooltip rectangles as its size is now: always as many, in the same order, and not overlapping
    // (the tooltip control keeps the tool the mouse is in).
    std::vector<Tip> tips();
    // The tab it stands on: the stage name of its parameters.
    virtual const char* stage() const = 0;
    // The parameter is set in the graphs alone: it has no row on the tab.
    virtual bool owns(int param) const = 0;
    // The smallest height the strip is useful at (at this DPI).
    static int minHeight(UINT dpi) { return static_cast<int>(214.0 * dpi / 96.0 + 0.5); }

    static void unregisterClass(HINSTANCE module);

protected:
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
    // A reset icon: lit when its parameter's value differs from the reset value; a click sets it back.
    struct Icon {
        RECT rect;
        int param;
    };
    // A small button that steps a parameter (the number of bands: - and +).
    struct Stepper {
        RECT rect;
        int param;
        int steps;
        const wchar_t* sign;
    };
    // A few values in a panel's header: name, value, reset icon - on the title's line where that fits, else in
    // columns under the title (as many as fit; two by two rather than three and one, three and two rather than four
    // and one).
    struct Legend {
        static constexpr int kMax = 6;
        int count = 0;
        int param[kMax] = {};
        const wchar_t* name[kMax] = {};
        const wchar_t* widest[kMax] = {}; // the widest text its value can have
        int nameW[kMax] = {}, valueW[kMax] = {};
        RECT bounds = {}, item[kMax] = {}; // an item: its name and value (the icon beside it is in icons_)
        int headerBottom = 0;
        bool inLine = false; // on the title's line
        bool beside = false; // one column at the panel's left, the graph beside it (layoutLegendColumn)
    };

    // ---- what a strip is ----
    virtual int markCount() const = 0;
    virtual int markParam(int mark) const = 0;
    virtual bool markUpDown(int mark) const = 0; // dragged up / down (else left / right): the cursor
    virtual void measure(HDC dc) { (void)dc; }    // text widths in the small font (selected)
    virtual void layout(int w, int h) = 0;        // panels, legends, icons_, steppers_
    virtual void fillTips(std::vector<Tip>& out) = 0;
    virtual void renderStill(HDC dc, int w, int h) = 0;  // what does not move with the audio
    virtual void renderLayer(Gdiplus::Graphics& g, int w, int h) { (void)g, (void)w, (void)h; } // still, over moving
    virtual void paintLive(HDC dc) = 0;                  // what moves
    virtual int lineAt(POINT pt) const { (void)pt; return -1; } // a line that is taken like its mark
    // The value a dragged mark gets at this point (before it is put in its range and on a slider position).
    virtual bool dragValue(int mark, POINT pt, double& value) const = 0;
    virtual int wheelParam(POINT pt) const = 0;         // the parameter the wheel steps at this point, or -1
    virtual double stepped(int param, int steps) const; // its value `steps` steps on: slider positions

    // ---- what a strip uses ----
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }
    int textWidth(HDC dc, const wchar_t* s) const;
    void drawMark(Gdiplus::Graphics& g, int id, double x, double y);
    // The four panels of a tab: the first across the top (a little over half of the height) with the other three
    // equally wide under it; all four side by side, the first by far the widest, in a strip too low for two rows or
    // so wide that the top panel would be a ribbon.
    void layoutFour(int w, int h, RECT panel[4]) const;
    RECT cornerIcon(const RECT& panel) const; // a reset icon in a panel's title line, at the right
    void measureLegend(HDC dc, Legend& lg) const;
    // The legend's items and icons (appended to icons_) in `panel`'s header: between `titleW` at the left and
    // `rightW` kept free at the right.
    void layoutLegend(Legend& lg, const RECT& panel, int titleW, int rightW);
    // The legend as one column under the title at the panel's left: for a square graph that leaves room beside it.
    int legendColumnWidth(const Legend& lg) const;
    void layoutLegendColumn(Legend& lg, const RECT& panel);
    // A panel with a legend as three tooltip rectangles around the legend (which has its own): `text` each.
    static void legendTips(const Legend& lg, const RECT& panel, const wchar_t* text, std::vector<Tip>& out);
    // The panels' frames, into the still picture.
    void drawPanels(Gdiplus::Graphics& g, const RECT* panel, int count);
    // An attack graph: a hit `hitDb` over the threshold in its first `spanMs` - dashed as it comes in, the line as
    // it comes out, turned down `reduce` dB in the end - with the attack's mark on the line (its value: the mark's
    // time - kHitAtMs). And a release graph: the gain on the rest of the music after that hit, from `fromMs`
    // (negative: the hit still lasts) to `toMs`, with the release's mark (its value: the mark's time).
    static constexpr double kHitAtMs = 10.0;
    void drawAttack(Gdiplus::Graphics& g, Plot& q, int mark, double hitDb, double reduce, double attackMs,
                    double spanMs, double tickMs, const wchar_t* spanText, std::vector<Label>& labels);
    void drawRelease(Gdiplus::Graphics& g, Plot& q, int mark, double reduce, double releaseMs, double fromMs,
                     double toMs, double tickMs, double endMs, const wchar_t* endText, std::vector<Label>& labels);
    // A line from `from` to `to` - upright at x = `at`, else level at y = `at` - that gives way to the names it
    // would run through (their rectangles; of a level line the letters, not the empty top and bottom of the text's
    // row): drawn in stretches around them, with no stub left beside one.
    void lineAround(Gdiplus::Graphics& g, const Gdiplus::Pen& pen, bool upright, double at, double from, double to,
                    const std::vector<RECT>& names) const;
    void blendLayer(HDC dc, const RECT& r); // the layer's part `r` over what is there
    bool lit(int param) const;              // its value differs from the reset value
    bool live() const;                      // the deck plays
    const DeckAnalysis* analysis() const;
    const DeckMeters* meters() const;
    // GDI, for the live parts
    static void polygon(HDC dc, const std::vector<POINT>& pts, COLORREF c);
    static void polyline(HDC dc, const std::vector<POINT>& pts, COLORREF c, int width, int style = PS_SOLID);
    static void text(HDC dc, double x, double y, const std::wstring& s, COLORREF c, UINT align = TA_LEFT);
    static int clip(HDC dc, const Plot& q); // SaveDC + the plot as the clip; RestoreDC(dc, <result>) afterwards
    static void labels(HDC dc, const std::vector<Label>& all);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, symbol_ = nullptr; // the small font; the reset icons' arrow
    UINT dpi_ = 96;
    int textH_ = 13; // the small font's height
    PresetValues values_ = defaultValues(), resetValues_ = defaultValues();
    const AnalysisSource* source_ = nullptr;
    int deck_ = 0;
    std::vector<POINT> mark_;       // where each mark was drawn (drawMark)
    std::vector<Icon> icons_;       // the reset icons with a place of their own (layout)
    std::vector<Icon> moreIcons_;   // those that stand beside a value in a graph (the drawing puts them there)
    std::vector<Stepper> steppers_; // (layout)

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC target);
    void drawControls(HDC dc);
    int markAt(POINT pt) const;
    const Icon* iconAt(POINT pt) const;       // a lit reset icon under the point
    const Stepper* stepperAt(POINT pt) const;
    void drag(POINT pt);
    void change(int param, double value, bool save); // a new value from the graph: shown, then told to the owner

    Edit edit_;
    Reset resetTo_;
    int drag_ = -1;        // the mark being dragged
    int pressParam_ = -1;  // the parameter of the reset icon or stepper the mouse button went down on
    int pressSteps_ = 0;   // 0: a reset icon
    int wheel_ = 0;        // wheel movement not yet a whole step
    HBITMAP still_ = nullptr, frame_ = nullptr; // the still picture, and the frame being drawn
    HBITMAP layer_ = nullptr;                   // 32 bits, premultiplied: still things that lie over moving ones
    void* layerBits_ = nullptr;
    int bitmapW_ = 0, bitmapH_ = 0;
    bool stillDirty_ = true;
};

} // namespace ap
