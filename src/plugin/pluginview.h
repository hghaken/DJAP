// The plug-in's window inside the host: one deck strip as in the program - preset and colour with their Edit
// buttons (the program's preset window), look-ahead, Bypass, the four bass sliders and the meter panel (LED bars, the
// gain of each stage, the spectrum; a click on a row opens its tab) - on the brushed metal. Every value is also a
// parameter the host shows in its own list.
#pragma once

#include "monitor.h"
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
    void slider(HWND bar, int code);
    void command(int id, int code);
    int px(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    static Steinberg::ViewRect sizeFor(UINT dpi);

    Plugin* plugin_;
    HWND hwnd_ = nullptr, title_ = nullptr, version_ = nullptr, presetLabel_ = nullptr, preset_ = nullptr,
         colourLabel_ = nullptr, colour_ = nullptr, lookLabel_ = nullptr, look_ = nullptr, bypass_ = nullptr, oversampling_ = nullptr,
         meter_ = nullptr, edit_ = nullptr, editColour_ = nullptr;
    HWND sliderLabel_[4] = {}, slider_[4] = {}, sliderValue_[4] = {};
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
};

} // namespace ap::plugin
