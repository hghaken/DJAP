// The plug-in's window inside the host: one deck strip as in the program - preset, colour, look-ahead, Bypass, the
// four bass sliders and the meter panel (LED bars, the gain of each stage, the spectrum) - on the brushed metal.
// Every other value is a parameter the host shows in its own list.
#pragma once

#include "monitor.h"
#include "theme.h"
#include "tooltips.h"

#include "public.sdk/source/common/pluginview.h"

#include <windows.h>

namespace ap::plugin {

class Plugin;

class PluginView : public Steinberg::CPluginView {
public:
    explicit PluginView(Plugin* plugin);
    ~PluginView() override;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;

    // A parameter changed (the host, a preset): the controls follow at the next timer tick.
    void refresh() { dirty_ = true; }

    // The module's window classes and GDI+ (pluginentry.cpp: at load / unload of the DLL).
    static void registerClasses(HINSTANCE instance);
    static void unregisterClasses();

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void layout();
    void fill();     // the controls from the parameters
    void meters();   // the meter panel from the deck
    void status();   // the diagnostics line: what the host does with this instance (plugindiag.h)
    void slider(HWND bar, int code);
    void command(int id, int code);
    int px(int v) const { return MulDiv(v, static_cast<int>(dpi_), 96); }
    static Steinberg::ViewRect sizeFor(UINT dpi);

    Plugin* plugin_;
    HWND hwnd_ = nullptr, title_ = nullptr, version_ = nullptr, presetLabel_ = nullptr, preset_ = nullptr,
         colourLabel_ = nullptr, colour_ = nullptr, lookLabel_ = nullptr, look_ = nullptr, bypass_ = nullptr,
         meter_ = nullptr, status_ = nullptr;
    HWND sliderLabel_[4] = {}, slider_[4] = {}, sliderValue_[4] = {};
    HFONT font_ = nullptr, bold_ = nullptr;
    UINT dpi_ = 96;
    bool dirty_ = true;
    uint64_t lastCalls_ = 0;
    int ticks_ = 0;
    theme::Metal metal_;
    Tooltips tips_;
    DeckMonitor monitor_;
};

} // namespace ap::plugin
