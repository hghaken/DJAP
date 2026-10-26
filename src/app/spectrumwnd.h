// Spectrum window (client, 2026-10-26: "all together in a separate, big spectrum graph"; a click on a deck's spectrum
// in the meter panel opens it): one deck's input and output spectrum, the density stage's bands with their gains now -
// a strip, a shade or lines, the user's choice (also in the meter panels) - and the deck's colour as an EQ curve: the
// total (the auto tone's correction now included), each tone section thin, the fixed tone dashed while the auto tone
// moves it, the sub bass's range. A point per tone section (bass, low-mid, presence, treble) can be dragged: left-right
// its frequency, up-down its gain, in the parameters' own steps; a double-click sets its gain back to 0 dB. That
// changes the deck's colour preset (every deck using it), heard at once and saved when the mouse is let go. A handle
// per density band (its fixed gain, the preset's Band N gain) is dragged up and down the same way; "Snap to bands"
// makes a dragged colour point sit on a band split when it comes near one (client, 2026-10-26). Reset preset / Reset
// colour set the deck's preset or colour back as the preset window's button does (asks first). A second row holds
// the main window's four bass sliders for the deck (bassctl.h; client, 2026-10-26).
// Custom-drawn on the dark meter palette, refreshed by the main window's timer. One modeless window, owned by the
// main window.
#pragma once

#include "audioio.h"
#include "bassctl.h"
#include "settings.h"
#include "spectrumdraw.h"
#include "tooltips.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ap {

class SpectrumWindow {
public:
    // A colour value or the density view changed; save = false while a point is dragged.
    using Changed = std::function<void(bool save)>;
    SpectrumWindow(Settings& settings, const AudioIO& audio, Changed onChange);
    ~SpectrumWindow();
    void show(HWND owner, int deck);
    void refresh(); // from the main window's timer
    HWND hwnd() const { return hwnd_; }

private:
    enum Button { bDeckA, bDeckB, bSnap, bResetPreset, bResetColour, kButtons };
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC target);
    void setFonts();
    spectrum::Data data() const;
    Preset* colour(); // the deck's colour preset (nullptr: none)
    Preset* preset(); // the deck's preset (its density band gains)
    void drag(POINT pt);
    void dragBand(POINT pt);
    void endDrag();
    void reset(bool colour); // the deck's preset or colour back to its shipped values (own: the defaults), asks first
    void layoutBar();        // the Loudness and Colour lists' places in the bar (after a size, DPI or deck change)
    void fillBoxes();        // their entries and choices, when the lists or the deck's choice changed
    void choose(bool colour); // a list's choice: the deck's loudness preset or colour (both decks when linked)
    void fillSliders();       // the bass sliders' positions from the deck's values (not while one is dragged)
    void hover(POINT pt);
    int buttonAt(POINT pt) const;
    void setTitle();
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }

    Settings& settings_;
    const AudioIO& audio_;
    Changed onChange_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, small_ = nullptr;
    UINT dpi_ = 96;
    Tooltips tips_;
    int deck_ = 0;
    spectrum::Geometry geo_;
    RECT button_[kButtons] = {};
    int hot_ = -1, dragging_ = -1; // the tone point under the mouse, the one being dragged
    int hotBand_ = -1, draggingBand_ = -1; // the same for the density band handles
    int snapSplit_ = -1;           // the split the dragged point sits on
    POINT mouse_{-1, -1};          // the mouse in the graph (for the read-out), -1: outside
    RECT tipRect_[5] = {};         // where the tooltips are (graph, deck buttons, snap, the two resets)
    std::wstring title_;           // the title shown
    // the Loudness and Colour lists (client, 2026-10-26: "the dropdowns for preset and colour in the big spectrum
    // window too"): real combo boxes in the custom-drawn bar
    HWND boxes_[2] = {};           // 0 loudness, 1 colour
    int boxLabelX_[2] = {}, buttonsX_ = 0, decksRight_ = 0, layoutDecks_ = 0;
    std::vector<std::string> boxNames_[2];
    // the bass sliders (a second row of the bar): their trackbars, where their labels and values are drawn
    HWND bass_[bass::kSliders] = {};
    int bassLabelX_[bass::kSliders] = {}, bassValueX_[bass::kSliders] = {}, bassRowY_ = 0;
    HBRUSH backBrush_ = nullptr; // the trackbars' background (WM_CTLCOLORSTATIC)
    bool filling_ = false;
};

} // namespace ap
