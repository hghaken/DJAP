// The plug-in for DJ software (docs/design.md section 10): one deck - stereo in, stereo out - with the same chain,
// presets and colours as the program. Processor and controller in one object (SingleComponentEffect), so the
// window reads the deck's meters directly. Every chain value is a parameter the host can show and automate; the
// preset and colour lists come from the program's settings file (read only); a project stores the values
// themselves, so it plays the same on a computer without that preset.
#pragma once

#include "deck.h"
#include "plugindiag.h"
#include "params.h"
#include "realtime.h"
#include "settings.h"

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <atomic>
#include <string>
#include <vector>

namespace ap::plugin {

class PluginView;

class Plugin : public Steinberg::Vst::SingleComponentEffect {
public:
    using tresult = Steinberg::tresult;
    using ParamID = Steinberg::Vst::ParamID;
    using ParamValue = Steinberg::Vst::ParamValue;

    Plugin();
    ~Plugin() override;
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new Plugin);
    }

    // IComponent / IAudioProcessor
    tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    tresult PLUGIN_API terminate() override;
    tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs,
                                          Steinberg::int32 numOuts) override;
    tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    tresult PLUGIN_API setProcessing(Steinberg::TBool state) override;
    Steinberg::uint32 PLUGIN_API getLatencySamples() override;
    tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

    // IEditController
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // For the window (GUI thread).
    DeckProcessor& deck() { return deck_; }
    double sampleRate() const { return fs_; }
    bool active() const { return active_.load(); }
    const std::vector<Preset>& presets() const { return presets_; }
    const std::vector<Preset>& colours() const { return colours_; }
    int presetIndex() const;
    int colourIndex() const;
    PresetValues currentValues() const; // the chain values as the controller holds them
    double plainOf(ParamID id) const;   // a parameter's value in its unit
    // An edit from the window: tells the host (which passes it to the processing) and updates the controller.
    void edit(ParamID id, ParamValue normalized);
    void editPlain(ParamID id, double plain);
    // The window chose a preset or colour (index into its list): the list parameter, or - when it is already the
    // chosen one - every value of it again (back to the preset after changes).
    void choose(bool colour, int index);
    void viewClosed(PluginView* view);
    // diagnostics (plugindiag.h)
    const ProcessStats& stats() const { return stats_; }
    int id() const { return id_; }
    bool hasView() const { return view_ != nullptr; }
    const std::string& hostName() const { return hostName_; }
    static ParamValue normalizedOf(int p, double plain);

private:
    struct Snapshot { // a whole state for the audio thread (setState, a preset loaded by the window)
        PresetValues values;
        double lookMs;
        bool bypass;
    };

    void addParameters();
    void loadLists();
    void loadPresetIntoController(int index, bool colour);
    void setLatencyFrom(double lookMs);
    void publishConfig(); // audio thread (or while inactive)
    // The controller's whole state to the processing (GUI thread). A host that never delivers parameter changes to
    // the processing (one DJ program does not, for a plug-in whose controller and processor are one object) would
    // otherwise never hear the window's edits.
    void publishToProcessor();
    double plainFromNormalized(int p, ParamValue v) const;

    // lists from DJAP.ini (or the shipped ones), fixed after initialize()
    std::vector<Preset> presets_, colours_;

    // audio side
    DeckProcessor deck_;
    double fs_ = 48000.0;
    std::atomic<bool> active_{false};
    PresetValues values_{};
    double lookMs_ = 1.5;
    bool bypass_ = false, dirty_ = true;
    SeqLockSlot<Snapshot> incoming_;
    std::atomic<Steinberg::uint32> latency_{0};
    std::atomic<bool> hostRoutes_{false}; // the host has delivered parameter changes to process()

    // controller side
    bool loading_ = false; // setState / a preset load in progress: no preset reloads from setParamNormalized
    // the chain values exactly (a preset's 800 Hz split, not the 810 Hz of the nearest host step); a host or the
    // window moving a parameter to another step replaces the value with that step's
    PresetValues exact_{};
    PluginView* view_ = nullptr;

    // diagnostics
    ProcessStats stats_;
    int id_ = 0;
    std::string hostName_;
};

} // namespace ap::plugin
