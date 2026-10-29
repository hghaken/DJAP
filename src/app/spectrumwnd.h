// SPDX-License-Identifier: GPL-3.0-or-later
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
// Custom-drawn on the dark meter palette, refreshed by its owner's timer. One modeless window, owned by the main
// window - or by the plug-in's window (client, 2026-10-29: "the plug-in does not follow the program: a click on the
// spectrum does not give the big one, nor the bass settings"), where it shows and changes the plug-in's one deck.
#pragma once

#include "analysiswnd.h"
#include "bassctl.h"
#include "settings.h"
#include "spectrumdraw.h"
#include "tooltips.h"

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ap {

// What the window shows and changes: the program's decks - their loudness presets and colours in the settings, every
// deck using them changes - or the plug-in's one deck, its own parameters.
class SpectrumSource {
public:
    virtual ~SpectrumSource() = default;
    virtual int decks() const = 0; // 1 or 2
    virtual bool audioActive() const = 0;
    virtual const DeckMeters& meters(int d) const = 0;
    virtual const DeckAnalysis& analysis(int d) const = 0;
    virtual double sampleRate() const = 0;          // 0: none yet
    virtual PresetValues values(int d) const = 0;   // the deck's chain values: its loudness preset with its colour
    virtual bool processing(int d) const = 0;       // false: bypassed
    virtual std::wstring deckName(int d) const = 0; // "Deck A", "Channel", "DJAP"
    // The Loudness (colour = false) and Colour lists: their entries, the deck's choice (-1: none of them), its name
    // for the title, and a choice made in the window.
    virtual std::vector<std::string> names(bool colour) const = 0;
    virtual int chosen(int d, bool colour) const = 0;
    virtual std::string chosenName(int d, bool colour) const = 0;
    virtual void choose(int d, bool colour, int index) = 0;
    // The colour's points (colour) or the density band handles (loudness) can be dragged.
    virtual bool editable(int d, bool colour) const = 0;
    // A dragged point or handle, a double-click: heard at once; save = false while the mouse is still down.
    virtual void set(int d, const std::vector<std::pair<int, double>>& values, bool save) = 0;
    virtual void setBass(int d, int slider, int pos, bool save) = 0; // a bass slider (bassctl.h)
    virtual void reset(int d, bool colour, HWND owner) = 0;          // Reset loudness / colour (asks first)
    virtual const wchar_t* resetTip(bool colour) const = 0;
    virtual std::wstring hint(int d) const = 0; // the line under the graph: what a drag changes
    virtual bool snapToBands() const = 0;
    virtual void setSnapToBands(bool on) = 0;
};

// The program's decks: the settings' presets and colours (every deck using them changes), the audio's meters through
// the analysis window's source; `changed(save)` after every change (heard at once; saved when save is true).
class SettingsSpectrum : public SpectrumSource {
public:
    using Changed = std::function<void(bool save)>;
    SettingsSpectrum(Settings& settings, const AnalysisSource& audio, std::function<double()> rate, Changed changed)
        : settings_(settings), audio_(audio), rate_(std::move(rate)), changed_(std::move(changed)) {}
    int decks() const override { return settings_.decks; }
    bool audioActive() const override { return audio_.audioActive(); }
    const DeckMeters& meters(int d) const override { return audio_.meters(d); }
    const DeckAnalysis& analysis(int d) const override { return audio_.analysis(d); }
    double sampleRate() const override { return rate_ ? rate_() : 0.0; }
    PresetValues values(int d) const override { return settings_.deckValues(d); }
    bool processing(int d) const override { return settings_.deck[d].processing; }
    std::wstring deckName(int d) const override;
    std::vector<std::string> names(bool colour) const override;
    int chosen(int d, bool colour) const override;
    std::string chosenName(int d, bool colour) const override;
    void choose(int d, bool colour, int index) override;
    bool editable(int d, bool colour) const override;
    void set(int d, const std::vector<std::pair<int, double>>& values, bool save) override;
    void setBass(int d, int slider, int pos, bool save) override;
    void reset(int d, bool colour, HWND owner) override;
    const wchar_t* resetTip(bool colour) const override;
    std::wstring hint(int d) const override;
    bool snapToBands() const override { return settings_.snapToBands; }
    void setSnapToBands(bool on) override;

private:
    Settings& settings_;
    const AnalysisSource& audio_;
    std::function<double()> rate_;
    Changed changed_;
};

class SpectrumWindow {
public:
    explicit SpectrumWindow(SpectrumSource& source);
    ~SpectrumWindow();
    void show(HWND owner, int deck);
    void close();   // the window goes (its owner goes)
    void refresh(); // from the owner's timer
    HWND hwnd() const { return hwnd_; }
    // The module the window class belongs to: the program (default) or the plug-in DLL, which removes the class
    // again before it is unloaded (as AnalysisWindow).
    static void setModule(HINSTANCE instance);
    static void unregisterClass();

private:
    enum Button { bDeckA, bDeckB, bSnap, bResetPreset, bResetColour, kButtons };
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC target);
    void setFonts();
    spectrum::Data data() const;
    void drag(POINT pt);
    void dragBand(POINT pt);
    void endDrag();
    void layoutBar();        // the Loudness and Colour lists' places in the bar (after a size, DPI or deck change)
    void fillBoxes();        // their entries and choices, when the lists or the deck's choice changed
    void choose(bool colour); // a list's choice: the deck's loudness preset or colour (both decks when linked)
    void fillSliders();       // the bass sliders' positions from the deck's values (not while one is dragged)
    void hover(POINT pt);
    int buttonAt(POINT pt) const;
    void setTitle();
    int px(double x) const { return static_cast<int>(x * dpi_ / 96.0 + 0.5); }

    SpectrumSource& source_;
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
    int boxChosen_[2] = {-2, -2}; // the choice the lists show
    // the bass sliders (a second row of the bar): their trackbars, where their labels and values are drawn
    HWND bass_[bass::kSliders] = {};
    int bassLabelX_[bass::kSliders] = {}, bassValueX_[bass::kSliders] = {}, bassRowY_ = 0;
    HBRUSH backBrush_ = nullptr; // the trackbars' background (WM_CTLCOLORSTATIC)
    bool filling_ = false;
};

} // namespace ap
