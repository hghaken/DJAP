// SPDX-License-Identifier: GPL-3.0-or-later
// The plug-in's window inside the host: one deck strip as in the program - DJAP and the version (with a newer
// version's note from the update check: one per DLL, asked once per load when the program's daily check is on),
// Graphs (the program's analysis window, one deck), Bypass, preset and colour with their Edit buttons (the program's
// preset window) and the meter panel (LED bars, the gain of each stage, the spectrum; a click on a row opens its tab,
// on the spectrum the program's spectrum window with the bass sliders) - on the brushed metal; a right-click: Setup
// (the plug-in's own, pluginsetup.h: look-ahead, oversampling, tooltips, updates) and About, as in the program. The
// bass sliders, oversampling, look-ahead and About button left the window on 2026-10-29 (client: "they are in other
// windows"). Every value is also a parameter the host shows in its own list.
#pragma once

#include "aboutwnd.h"
#include "analysiswnd.h"
#include "monitor.h"
#include "pluginsetup.h"
#include "spectrumwnd.h"
#include "theme.h"
#include "tooltips.h"

#include "public.sdk/source/common/pluginview.h"

#include <windows.h>

#include <atomic>

namespace ap::plugin {

class Plugin;

class PluginView : public Steinberg::CPluginView {
public:
    explicit PluginView(Plugin* plugin);
    ~PluginView() override;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;

    // A parameter changed (the host, a preset): the controls follow at the next timer tick (any thread).
    void refresh() { dirty_.store(true, std::memory_order_relaxed); }
    // The plug-in ends before this window: the window goes now and the plug-in is never called again.
    void detach();
    // Presets were added, renamed or deleted: the lists are filled again.
    void listsChanged();

    // The module's window classes and GDI+ (pluginentry.cpp: at load / unload of the DLL).
    static void registerClasses(HINSTANCE instance);
    static void unregisterClasses();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void layout();
    void fillLists(); // the preset and colour lists
    void fill();     // the controls from the parameters
    void meters();   // the meter panel from the deck
    void command(int id, int code);
    void updateNote(); // the version line: a newer version when the check found one
    int px(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    static Steinberg::ViewRect sizeFor(UINT dpi);

    Plugin* plugin_;
    HWND hwnd_ = nullptr, title_ = nullptr, version_ = nullptr, presetLabel_ = nullptr, preset_ = nullptr,
         colourLabel_ = nullptr, colour_ = nullptr, bypass_ = nullptr, meter_ = nullptr, edit_ = nullptr,
         editColour_ = nullptr, graphs_ = nullptr;
    AboutWindow aboutWnd_; // the About window with the update check (client, 2026-10-28)
    // the Graphs button's analysis window (client, 2026-10-29: "I miss the Graphs button in the plug-in"): this deck
    class Graphs : public AnalysisSource {
    public:
        explicit Graphs(const PluginView& view) : view_(view) {}
        int decks() const override { return 1; }
        bool audioActive() const override;
        const DeckMeters& meters(int) const override { return view_.monitor_.meters(); }
        const DeckAnalysis& analysis(int) const override { return view_.monitor_.analysis(); }
        PresetValues values(int) const override;
        std::wstring title(int) const override;
        bool processing(int) const override;

    private:
        const PluginView& view_;
    };
    // the spectrum window of this channel (client, 2026-10-29: "a click on the spectrum does not give the big one, nor
    // the bass settings"): the program's window, its values the plug-in's parameters - the project keeps them, the
    // presets in the lists stay as they are
    class Spectrum : public SpectrumSource {
    public:
        explicit Spectrum(PluginView& view) : view_(view) {}
        int decks() const override { return 1; }
        bool audioActive() const override { return view_.graphsSource_.audioActive(); }
        const DeckMeters& meters(int) const override { return view_.monitor_.meters(); }
        const DeckAnalysis& analysis(int) const override { return view_.monitor_.analysis(); }
        double sampleRate() const override;
        PresetValues values(int d) const override { return view_.graphsSource_.values(d); }
        bool processing(int d) const override { return view_.graphsSource_.processing(d); }
        std::wstring deckName(int) const override { return L"DJAP"; }
        std::vector<std::string> names(bool colour) const override;
        int chosen(int d, bool colour) const override;
        std::string chosenName(int d, bool colour) const override;
        void choose(int d, bool colour, int index) override;
        bool editable(int, bool) const override { return view_.plugin_ != nullptr; }
        void set(int d, const std::vector<std::pair<int, double>>& values, bool save) override;
        void setBass(int d, int slider, int pos, bool save) override;
        void reset(int d, bool colour, HWND owner) override;
        const wchar_t* resetTip(bool colour) const override;
        std::wstring hint(int d) const override;
        bool snapToBands() const override { return snap_; }
        void setSnapToBands(bool on) override { snap_ = on; }
        bool snap_ = false; // from the settings file when the window opens; not saved by the plug-in

    private:
        PluginView& view_;
    };
    void contextMenu(int x, int y);
    int ticks_ = 0;
    HFONT font_ = nullptr, bold_ = nullptr;
    // counted in g_views (attached() succeeded so far): removed() and detach() share one teardown, so a host that
    // calls removed() twice, or never after terminate(), cannot unbalance the count (outside review)
    bool attached_ = false;
    void teardown();
    UINT dpi_ = 96;
    std::atomic<bool> dirty_{true};
    theme::Metal metal_;
    Tooltips tips_;
    DeckMonitor monitor_;
    Graphs graphsSource_{*this};
    AnalysisWindow graphsWnd_{graphsSource_};
    // (set in the constructor: a default member initializer sees *this as const here)
    Spectrum spectrumSource_;
    SpectrumWindow spectrumWnd_;
    SetupWindow setupWnd_;
};

} // namespace ap::plugin
