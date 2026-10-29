// SPDX-License-Identifier: GPL-3.0-or-later
// The plug-in for DJ software (docs/design.md section 10): one deck - stereo in, stereo out - with the same chain,
// presets and colours as the program. Processor and controller in one object (SingleComponentEffect), so the
// window reads the deck's meters directly. Every chain value is a parameter the host can show and automate; the
// preset and colour lists are the program's settings file's, %APPDATA%\DJAP\DJAP.ini, which the plug-in's preset
// window also saves to (merged: Settings::saveMerged); a project stores the values themselves, so it plays the
// same on a computer without that preset.
#pragma once

#include "deck.h"
#include "editwnd.h"
#include "params.h"
#include "realtime.h"
#include "settings.h"

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <atomic>
#include <filesystem>
#include <memory>
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
    const std::vector<Preset>& presets() const { return settings_.presets; }
    bool tooltips() const { return settings_.showTooltips; } // the program's switch (read from the settings file)
    bool checkUpdates() const { return settings_.checkUpdates; } // the program's daily-check switch, likewise
    bool snapToBands() const { return settings_.snapToBands; }   // the spectrum window's start (not saved by the plug-in)
    // The plug-in's Setup window: the two switches it shares with the program, merged into the settings file.
    void setSwitches(bool tooltips, bool checkUpdates);
    const std::filesystem::path& iniFile() const { return ini_; } // (not "settingsFile": plugin.cpp's own function)
    const std::vector<Preset>& colours() const { return settings_.colours; }
    int presetIndex() const; // in its list; the list's size = "(other values)"
    int colourIndex() const;
    PresetValues currentValues() const; // the chain values as the controller holds them
    double plainOf(ParamID id) const;   // a parameter's value in its unit
    // An edit from the window: tells the host (which passes it to the processing) and updates the controller.
    void edit(ParamID id, ParamValue normalized);
    void editPlain(ParamID id, double plain);
    // The window chose a preset or colour (index into its list): the list parameter, or - when it is already the
    // chosen one - every value of it again (back to the preset after changes).
    void choose(bool colour, int index);
    // The program's preset (or colour) window for this plug-in, on `tab` (empty: as it was); while the plug-in's
    // window is open (closeEditors when it closes).
    void openEditor(bool colour, HWND owner, const std::string& tab = {});
    void closeEditors();
    void viewClosed(PluginView* view);
    static ParamValue normalizedOf(int p, double plain);

private:
    struct Snapshot { // a whole state for the audio thread (setState, a preset loaded by the window)
        PresetValues values;
        double lookMs;
        bool bypass;
        int oversampling;
        bool mono;
    };

    void addParameters();
    void loadLists();
    void loadPresetIntoController(bool colour); // the values of the chosen preset (or colour), if it exists
    void listsChanged();                        // presets added, renamed or deleted: the list parameters follow
    void edited(bool listChanged, bool save);   // the preset window changed a preset or the choice
    PresetValues chosenValues() const;          // the chosen preset with the chosen colour (exact_ for "other")
    bool saveSettings();                        // the preset lists into the settings file, merged
    void setLatencyFrom(double lookMs);
    void publishConfig(); // audio thread (or while inactive)
    // The controller's whole state to the processing (GUI thread). A host that never delivers parameter changes to
    // the processing (one DJ program does not, for a plug-in whose controller and processor are one object) would
    // otherwise never hear the window's edits.
    void publishToProcessor();
    double plainFromNormalized(int p, ParamValue v) const;

    // the presets and colours (controller side, the GUI thread only: the processing never reads them); deck[0] holds
    // the chosen preset's and colour's names ("" = values that match none)
    Settings settings_, base_;
    PresetValues applied_{}; // chosenValues() when last taken over: edited() takes only what changed since
    std::filesystem::path ini_;
    std::unique_ptr<EditWindow> editor_, colourEditor_;

    // audio side
    DeckProcessor deck_;
    double fs_ = 48000.0;
    std::atomic<bool> active_{false};
    PresetValues values_{};
    double lookMs_ = 1.5;
    int oversampling_ = kOversamplingDefault;
    bool mono_ = false;
    bool bypass_ = false, dirty_ = true;
    SeqLockSlot<Snapshot> incoming_;
    std::atomic<Steinberg::uint32> latency_{0};
    std::atomic<bool> hostRoutes_{false}; // the host has delivered parameter changes to process()

    // controller side
    bool loading_ = false; // setState / a preset load in progress: no preset reloads from setParamNormalized
    // A preset or colour chosen in the window: the values it changes are told to the host too (tellHost)
    bool fromWindow_ = false;
    // Tells the host a value the controller changed by itself (beginEdit / performEdit / endEdit), so a host that keeps
    // the parameters' values for its set (and restores them one by one at the next start, never calling setState: the
    // client's DJ program) keeps the new one - not the value before a preset was chosen or changed in the window.
    void tellHost(ParamID id);
    // The stamp (kStampId) of the preset's and colour's values as last taken over (applied_): a host that restores a
    // set's parameters one by one restores it too; a stamp that is not the one of the preset in the settings file now
    // means the preset was changed since the set was saved - its values are loaded again (checkStamp, at activation
    // and when the window opens). A value changed in the set itself stays as long as its preset did not change.
    static double stampOf(const PresetValues& v);
    void updateStamp(bool tell);
    void checkStamp();
    bool stampRestored_ = false; // the host set the stamp (a restore)
    // the chain values exactly (a preset's 800 Hz split, not the 810 Hz of the nearest host step); a host or the
    // window moving a parameter to another step replaces the value with that step's
    PresetValues exact_{};
    PluginView* view_ = nullptr;
    std::atomic<bool> restart_{false}; // setProcessing(true): the deck fades in at the next process()
};

} // namespace ap::plugin
