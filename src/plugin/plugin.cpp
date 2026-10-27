#include "plugin.h"

#include "pluginids.h"
#include "pluginview.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace ap::plugin {

namespace {

constexpr int32 kStateVersion = 1;
constexpr double kMaxLook = 5.0; // ms, the look-ahead parameter's range (Setup window: 0 .. 5 ms)

// The stages as the host's groups (units), in chain order; the extra parameters stay in the root.
const char* const kStages[] = {"General",      "Leveller",      "Compressor", "Density",  "Bass enhancer",
                               "Exciter",      "De-esser",      "Stereo width", "Soft clipper", "Limiter",
                               "Tone",         "Auto tone",     "Sub bass",   "Loudness"};
constexpr int kStageUnits = static_cast<int>(sizeof kStages / sizeof kStages[0]);

UnitID unitOf(int p) {
    for (int u = 0; u < kStageUnits; ++u)
        if (std::strcmp(paramInfo(p).stage, kStages[u]) == 0) return u + 1;
    return kRootUnitId;
}

void ascii(String128 out, const char* text) { UString(out, 128).fromAscii(text); }

// The program's settings file, where the user's own presets and colours are (also those saved from a plug-in). A
// portable program (DJAP.ini next to DJAP.exe) is not seen.
std::filesystem::path settingsFile() {
    PWSTR roaming = nullptr;
    std::filesystem::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
        p = std::filesystem::path(roaming) / L"DJAP" / L"DJAP.ini";
    CoTaskMemFree(roaming);
    return p;
}

// One chain value as a parameter: the program's slider positions are the host's steps (params.h), so a value set
// in the host is one the program could set too.
class ChainParameter : public Parameter {
public:
    ChainParameter(int p, UnitID unit) : p_(p) {
        const ParamInfo& in = paramInfo(p);
        char title[128];
        std::snprintf(title, sizeof title, "%s: %s", in.stage, in.name);
        ascii(info.title, title);
        ascii(info.shortTitle, in.name);
        ascii(info.units, in.unit);
        info.id = paramId(p);
        info.unitId = unit;
        info.stepCount = paramPositions(p);
        info.flags = ParameterInfo::kCanAutomate;
        info.defaultNormalizedValue = Plugin::normalizedOf(p, in.def);
        setNormalized(info.defaultNormalizedValue);
    }
    ParamValue toPlain(ParamValue v) const override {
        return paramValueAt(p_, static_cast<int>(std::lround(v * paramPositions(p_))));
    }
    ParamValue toNormalized(ParamValue plain) const override { return Plugin::normalizedOf(p_, plain); }
    void toString(ParamValue v, String128 out) const override {
        const ParamInfo& in = paramInfo(p_);
        const double x = toPlain(v);
        char text[64];
        if (in.isSwitch) std::snprintf(text, sizeof text, "%s", x >= 0.5 ? "On" : "Off");
        else if (p_ == pCompMakeup && x < 0) std::snprintf(text, sizeof text, "auto");
        else if (p_ == pCompSideHpf && x <= 0) std::snprintf(text, sizeof text, "off");
        else std::snprintf(text, sizeof text, "%.*f", in.decimals, x);
        ascii(out, text);
    }
    bool fromString(const TChar* string, ParamValue& v) const override {
        const std::string s = StringConvert::convert(string);
        if (p_ == pCompMakeup && s.find("auto") != std::string::npos) return v = 0.0, true;
        if (p_ == pCompSideHpf && s.find("off") != std::string::npos) return v = 0.0, true;
        if (paramInfo(p_).isSwitch && (s == "On" || s == "on")) return v = 1.0, true;
        if (paramInfo(p_).isSwitch && (s == "Off" || s == "off")) return v = 0.0, true;
        std::string t = s;
        std::replace(t.begin(), t.end(), ',', '.');
        char* end = nullptr;
        const double x = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        v = toNormalized(x);
        return true;
    }

private:
    int p_;
};

// The look-ahead in ms, 0 .. 5 in steps of 0.1 (the SDK's RangeParameter counts whole steps from its minimum).
class LookParameter : public Parameter {
public:
    explicit LookParameter(double ms) {
        ascii(info.title, "Look-ahead");
        ascii(info.shortTitle, "Look-ahead");
        ascii(info.units, "ms");
        info.id = kLookId;
        info.unitId = kRootUnitId;
        info.stepCount = static_cast<int32>(kMaxLook * 10);
        info.flags = ParameterInfo::kCanAutomate;
        info.defaultNormalizedValue = 1.5 / kMaxLook;
        setNormalized(toNormalized(ms));
    }
    ParamValue toPlain(ParamValue v) const override { return std::round(v * kMaxLook * 10.0) / 10.0; }
    ParamValue toNormalized(ParamValue ms) const override { return std::min(kMaxLook, std::max(0.0, ms)) / kMaxLook; }
    void toString(ParamValue v, String128 out) const override {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f", toPlain(v));
        ascii(out, text);
    }
    bool fromString(const TChar* string, ParamValue& v) const override {
        std::string t = StringConvert::convert(string);
        std::replace(t.begin(), t.end(), ',', '.');
        char* end = nullptr;
        const double x = std::strtod(t.c_str(), &end);
        if (end == t.c_str()) return false;
        v = toNormalized(x);
        return true;
    }
};

// The preset (or colour) list: the names of the plug-in's lists - which change when the preset window saves,
// renames or deletes one - and "(other values)" after them.
class NameListParameter : public Parameter {
public:
    NameListParameter(const char* title, ParamID id, const std::vector<Preset>* items) : items_(items) {
        ascii(info.title, title);
        ascii(info.shortTitle, title);
        info.id = id;
        info.unitId = kRootUnitId;
        info.flags = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        follow();
    }
    void follow() { info.stepCount = std::max<int32>(1, static_cast<int32>(items_->size())); }
    ParamValue toPlain(ParamValue v) const override { return std::round(v * info.stepCount); }
    ParamValue toNormalized(ParamValue index) const override { return index / info.stepCount; }
    void toString(ParamValue v, String128 out) const override {
        const auto i = static_cast<size_t>(std::lround(v * info.stepCount));
        if (i < items_->size()) StringConvert::convert((*items_)[i].name, out);
        else ascii(out, "(other values)");
    }
    bool fromString(const TChar* string, ParamValue& v) const override {
        const std::string s = StringConvert::convert(string);
        for (size_t i = 0; i < items_->size(); ++i)
            if ((*items_)[i].name == s) return v = toNormalized(static_cast<double>(i)), true;
        return false;
    }

private:
    const std::vector<Preset>* items_;
};

} // namespace

Plugin::Plugin() = default;
Plugin::~Plugin() = default;

ParamValue Plugin::normalizedOf(int p, double plain) {
    return static_cast<double>(paramPositionOf(p, plain)) / paramPositions(p);
}

double Plugin::plainFromNormalized(int p, ParamValue v) const {
    return paramValueAt(p, static_cast<int>(std::lround(v * paramPositions(p))));
}

void Plugin::loadLists() {
    ini_ = settingsFile();
    std::error_code ec;
    if (!ini_.empty() && std::filesystem::exists(ini_, ec)) settings_.load(ini_); // else the shipped lists
    base_ = settings_;
    // one deck; the start: Medium with Neutral, as in the program
    settings_.decks = 1;
    settings_.linkPresets = false;
    settings_.deck[0] = DeckSettings();
    values_ = settings_.deckValues(0);
    exact_ = values_;
    applied_ = values_;
}

void Plugin::addParameters() {
    for (int u = 0; u < kStageUnits; ++u) {
        String128 name;
        ascii(name, kStages[u]);
        addUnit(new Unit(name, u + 1, kRootUnitId));
    }
    // the lists: the presets' names and one more for values that match none of them (a project from elsewhere)
    for (const bool colour : {false, true}) {
        auto* param = new NameListParameter(colour ? "Colour" : "Loudness", colour ? kColourId : kPresetId,
                                            colour ? &settings_.colours : &settings_.presets);
        const ParamValue norm = static_cast<double>(colour ? colourIndex() : presetIndex()) / param->getInfo().stepCount;
        param->getInfo().defaultNormalizedValue = norm;
        param->setNormalized(norm);
        parameters.addParameter(param);
    }
    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                            kBypassId);
    parameters.addParameter(new LookParameter(lookMs_));
    for (int p = 0; p < kParamCount; ++p) {
        auto* param = new ChainParameter(p, unitOf(p));
        param->setNormalized(normalizedOf(p, values_[static_cast<size_t>(p)]));
        parameters.addParameter(param);
    }
}

tresult PLUGIN_API Plugin::initialize(FUnknown* context) {
    const tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    loadLists();
    addParameters();
    setLatencyFrom(lookMs_);
    return kResultOk;
}

tresult PLUGIN_API Plugin::terminate() {
    closeEditors();
    // a host that ends the plug-in before its window: the window goes now and never calls the plug-in again
    if (view_) view_->detach();
    view_ = nullptr;
    return SingleComponentEffect::terminate();
}

// Processing starts again after a pause of the host: the deck fades in (its delay line and filters still hold the
// audio from before the pause - outside review), at the next process() on the audio thread.
tresult PLUGIN_API Plugin::setProcessing(TBool state) {
    if (state) restart_.store(true, std::memory_order_release);
    return kResultOk;
}

tresult PLUGIN_API Plugin::setupProcessing(ProcessSetup& setup) {
    fs_ = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
    setLatencyFrom(plainOf(kLookId));
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API Plugin::setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs,
                                              int32 numOuts) {
    // stereo in, stereo out only (a deck)
    if (numIns == 1 && numOuts == 1 && inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo)
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    return kResultFalse;
}

tresult PLUGIN_API Plugin::canProcessSampleSize(int32 symbolicSampleSize) {
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

uint32 PLUGIN_API Plugin::getLatencySamples() { return latency_.load(); }

void Plugin::setLatencyFrom(double lookMs) {
    const auto frames = static_cast<uint32>(lookFrames(lookMs, fs_)); // the deck's own rule
    if (latency_.exchange(frames) != frames && componentHandler) componentHandler->restartComponent(kLatencyChanged);
}

tresult PLUGIN_API Plugin::setActive(TBool state) {
    if (state) {
        Snapshot s;
        if (incoming_.take(s)) values_ = s.values, lookMs_ = s.lookMs, bypass_ = s.bypass;
        deck_.prepare(fs_); // allocates: never while processing (the host stops it first)
        publishConfig();
        deck_.startFadeIn(); // a stream may start in the middle of the music
        active_ = true;
    } else {
        active_ = false;
    }
    return SingleComponentEffect::setActive(state);
}

void Plugin::publishConfig() {
    DeckControl ctl;
    ctl.lookMs = lookMs_;
    ctl.processing = !bypass_;
    // the coefficients are designed here, on the audio thread, when the host changed a value: no allocation, a few
    // microseconds (the program designs them on its GUI thread)
    deck_.publish(makeDeckConfig(values_, ctl, fs_));
    dirty_ = false;
}

tresult PLUGIN_API Plugin::process(ProcessData& data) {
    Snapshot s;
    if (incoming_.take(s)) {
        values_ = s.values;
        lookMs_ = s.lookMs;
        bypass_ = s.bypass;
        dirty_ = true;
    }
    if (IParameterChanges* changes = data.inputParameterChanges) {
        const int32 count = changes->getParameterCount();
        if (count > 0 && !hostRoutes_.load(std::memory_order_relaxed)) hostRoutes_.store(true, std::memory_order_relaxed);
        for (int32 i = 0; i < count; ++i) {
            IParamValueQueue* q = changes->getParameterData(i);
            if (!q || q->getPointCount() <= 0) continue;
            int32 offset = 0;
            ParamValue v = 0;
            if (q->getPoint(q->getPointCount() - 1, offset, v) != kResultTrue) continue; // the block's last value
            const ParamID id = q->getParameterId();
            if (id == kPresetId || id == kColourId) {
                continue; // a preset's values come from the controller as a whole (publishToProcessor)
            } else if (id == kBypassId) {
                bypass_ = v >= 0.5;
            } else if (id == kLookId) {
                lookMs_ = snapLook(v * kMaxLook); // the window's choices (deck.h)
            } else {
                const int p = paramIndex(id);
                if (p < 0) continue;
                values_[static_cast<size_t>(p)] = plainFromNormalized(p, v);
            }
            dirty_ = true;
        }
    }
    if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
    if (data.symbolicSampleSize != kSample32) return kResultOk; // never 64-bit buffers read as 32-bit ones
    AudioBusBuffers& in = data.inputs[0];
    AudioBusBuffers& out = data.outputs[0];
    if (in.numChannels < 2 || out.numChannels < 2 || !in.channelBuffers32 || !out.channelBuffers32) return kResultOk;
    float* l = out.channelBuffers32[0];
    float* r = out.channelBuffers32[1];
    const int n = data.numSamples;
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);
    // an input the host marks silent may hold anything: it is read as silence (outside review)
    const bool silentIn = (in.silenceFlags & 3) == 3;
    if (silentIn) {
        std::memset(l, 0, bytes);
        std::memset(r, 0, bytes);
    } else {
        if (l != in.channelBuffers32[0]) std::memcpy(l, in.channelBuffers32[0], bytes);
        if (r != in.channelBuffers32[1]) std::memcpy(r, in.channelBuffers32[1], bytes);
    }
    out.silenceFlags = 0; // the deck's tail (look-ahead, release) may still sound
    if (!active_) return kResultOk; // not prepared: the input passes (never the host's leftovers)
    if (dirty_) publishConfig();
    if (restart_.exchange(false, std::memory_order_acq_rel)) deck_.startFadeIn();
    deck_.process(l, r, n);
    return kResultOk;
}

// ---- state: the values themselves, by ini key (a project keeps its sound without the program's presets) ----

tresult PLUGIN_API Plugin::getState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    s.writeInt32(kStateVersion);
    s.writeStr8(settings_.deck[0].preset.c_str()); // "" = values that match no preset
    s.writeStr8(settings_.deck[0].colour.c_str());
    s.writeDouble(plainOf(kLookId));
    s.writeInt8(plainOf(kBypassId) >= 0.5 ? 1 : 0);
    s.writeInt32(kParamCount);
    const PresetValues v = currentValues();
    for (int p = 0; p < kParamCount; ++p) {
        s.writeStr8(paramInfo(p).key);
        s.writeDouble(v[static_cast<size_t>(p)]);
    }
    return kResultOk;
}

tresult PLUGIN_API Plugin::setState(IBStream* state) {
    IBStreamer s(state, kLittleEndian);
    int32 version = 0, count = 0;
    if (!s.readInt32(version) || version < 1) return kResultFalse;
    // writeStr8's format (length with the NUL, then the bytes), read with its length: the SDK's readStr8 leaves the
    // text without a NUL when a project file is cut off or damaged, and a string made from it read past its end
    auto readText = [&s]() {
        int32 length = 0;
        if (!s.readInt32(length) || length <= 0 || length > 262144) return std::string();
        std::string t(static_cast<size_t>(length), '\0');
        const TSize got = s.readRaw(t.data(), length);
        t.resize(got > 0 ? static_cast<size_t>(got) : 0);
        const size_t end = t.find('\0');
        if (end != std::string::npos) t.resize(end);
        return t;
    };
    const std::string preset = readText(), colour = readText();
    double look = 1.5;
    int8 bypass = 0;
    if (!s.readDouble(look) || !s.readInt8(bypass) || !s.readInt32(count)) return kResultFalse;
    PresetValues v = currentValues();
    for (int32 i = 0; i < count; ++i) {
        const std::string key = readText();
        double x = 0;
        if (!s.readDouble(x)) return kResultFalse;
        for (int p = 0; p < kParamCount; ++p)
            if (key == paramInfo(p).key) v[static_cast<size_t>(p)] = x; // unknown keys (a newer version): skipped
    }
    normaliseValues(v);

    // the controller
    exact_ = v;
    settings_.deck[0].preset = settings_.find(preset) ? preset : std::string(); // unknown here: "(other values)"
    settings_.deck[0].colour = settings_.find(colour, PresetKind::Colour) ? colour : std::string();
    loading_ = true;
    SingleComponentEffect::setParamNormalized(kPresetId, static_cast<double>(presetIndex()) / settings_.presets.size());
    SingleComponentEffect::setParamNormalized(kColourId, static_cast<double>(colourIndex()) / settings_.colours.size());
    SingleComponentEffect::setParamNormalized(kBypassId, bypass ? 1.0 : 0.0);
    SingleComponentEffect::setParamNormalized(kLookId, std::min(kMaxLook, std::max(0.0, look)) / kMaxLook);
    for (int p = 0; p < kParamCount; ++p)
        SingleComponentEffect::setParamNormalized(paramId(p), normalizedOf(p, v[static_cast<size_t>(p)]));
    loading_ = false;
    applied_ = chosenValues();
    setLatencyFrom(look);

    // the processing (exactly the stored values, not their positions)
    incoming_.publish(Snapshot{v, std::min(kMaxLook, std::max(0.0, look)), bypass != 0});
    if (view_) view_->refresh();
    return kResultOk;
}

// ---- controller ----

int Plugin::presetIndex() const {
    for (size_t i = 0; i < settings_.presets.size(); ++i)
        if (settings_.presets[i].name == settings_.deck[0].preset) return static_cast<int>(i);
    return static_cast<int>(settings_.presets.size());
}

int Plugin::colourIndex() const {
    for (size_t i = 0; i < settings_.colours.size(); ++i)
        if (settings_.colours[i].name == settings_.deck[0].colour) return static_cast<int>(i);
    return static_cast<int>(settings_.colours.size());
}

double Plugin::plainOf(ParamID id) const {
    const ParamValue v = const_cast<Plugin*>(this)->getParamNormalized(id);
    if (id == kLookId) return snapLook(v * kMaxLook); // what runs (deck.h)
    if (id == kBypassId) return v >= 0.5 ? 1.0 : 0.0;
    const int p = paramIndex(id);
    return p >= 0 ? exact_[static_cast<size_t>(p)] : v;
}

PresetValues Plugin::currentValues() const { return exact_; }

void Plugin::loadPresetIntoController(bool colour) {
    const Preset* chosen = colour ? settings_.find(settings_.deck[0].colour, PresetKind::Colour)
                                  : settings_.find(settings_.deck[0].preset);
    if (!chosen) return; // "(other values)"
    loading_ = true;
    const PresetValues& v = chosen->values;
    for (int p = colour ? kFirstColourParam : 0; p < (colour ? kParamCount : kFirstColourParam); ++p) {
        exact_[static_cast<size_t>(p)] = v[static_cast<size_t>(p)];
        SingleComponentEffect::setParamNormalized(paramId(p), normalizedOf(p, v[static_cast<size_t>(p)]));
    }
    loading_ = false;
    applied_ = chosenValues();
    // the host reads the new values; the processing gets them as a whole (publishToProcessor)
    if (componentHandler) componentHandler->restartComponent(kParamValuesChanged);
}

void Plugin::listsChanged() {
    for (const ParamID id : {kPresetId, kColourId})
        if (auto* p = dynamic_cast<NameListParameter*>(getParameterObject(id))) p->follow();
    loading_ = true;
    SingleComponentEffect::setParamNormalized(kPresetId, static_cast<double>(presetIndex()) / settings_.presets.size());
    SingleComponentEffect::setParamNormalized(kColourId, static_cast<double>(colourIndex()) / settings_.colours.size());
    loading_ = false;
    if (componentHandler) componentHandler->restartComponent(kParamTitlesChanged | kParamValuesChanged);
    if (view_) view_->listsChanged();
}

bool Plugin::saveSettings() {
    if (ini_.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(ini_.parent_path(), ec); // a plug-in without the program: the folder
    auto names = [this] {
        std::vector<std::string> n;
        for (const Preset& p : settings_.presets) n.push_back(p.name);
        for (const Preset& p : settings_.colours) n.push_back("\x01" + p.name);
        return n;
    };
    const std::vector<std::string> before = names();
    // as a guest: only the plug-in's preset and colour changes; the program's other settings stay as they are
    const bool ok = settings_.saveMerged(ini_, base_, Settings::SaveRole::Guest);
    if (names() != before) listsChanged(); // another plug-in's or the program's presets came in
    return ok;
}

PresetValues Plugin::chosenValues() const {
    const Preset* p = settings_.find(settings_.deck[0].preset);
    const Preset* c = settings_.find(settings_.deck[0].colour, PresetKind::Colour);
    return withColour(p ? p->values : exact_, c ? c->values : exact_);
}

void Plugin::edited(bool listChanged, bool save) {
    // the preset window changed values of the chosen preset (or colour), or "Save as" moved the plug-in to its copy:
    // those values, and only those - a bass slider or the host's automation of another value stays as it is
    const PresetValues now = chosenValues();
    bool changed = false;
    loading_ = true;
    for (int i = 0; i < kParamCount; ++i) {
        const auto k = static_cast<size_t>(i);
        if (now[k] == applied_[k] || now[k] == exact_[k]) continue;
        exact_[k] = now[k];
        SingleComponentEffect::setParamNormalized(paramId(i), normalizedOf(i, now[k]));
        changed = true;
    }
    loading_ = false;
    applied_ = now;
    if (changed) {
        if (componentHandler) componentHandler->restartComponent(kParamValuesChanged);
        publishToProcessor();
    }
    if (listChanged) listsChanged();
    if (save) saveSettings();
    if (view_) view_->refresh();
}

void Plugin::openEditor(bool colour, HWND owner, const std::string& tab) {
    saveSettings(); // the lists as they are in the file now (another plug-in or the program may have added some)
    std::unique_ptr<EditWindow>& e = colour ? colourEditor_ : editor_;
    if (!e) {
        e = std::make_unique<EditWindow>(settings_, [this](bool listChanged, bool save) { edited(listChanged, save); },
                                         colour ? PresetKind::Colour : PresetKind::Dynamics);
        e->setDeckName(L"this plug-in");
    }
    const std::string& chosen = colour ? settings_.deck[0].colour : settings_.deck[0].preset;
    e->show(owner, chosen.empty() ? std::string(colour ? "Neutral" : "Medium") : chosen, 0, tab);
}

void Plugin::closeEditors() {
    editor_.reset();
    colourEditor_.reset();
}

tresult PLUGIN_API Plugin::setParamNormalized(ParamID id, ParamValue value) {
    const ParamValue before = getParamNormalized(id);
    const tresult r = SingleComponentEffect::setParamNormalized(id, value);
    const int p = paramIndex(id);
    if (p >= 0 && !loading_ && value != normalizedOf(p, exact_[static_cast<size_t>(p)]))
        exact_[static_cast<size_t>(p)] = plainFromNormalized(p, value); // moved to another step
    if (!loading_ && before != value) {
        if (id == kPresetId || id == kColourId) { // a choice of the host or the window: the name, then its values
            const bool colour = id == kColourId;
            const std::vector<Preset>& items = colour ? settings_.colours : settings_.presets;
            const auto i = static_cast<size_t>(std::lround(value * static_cast<double>(items.size())));
            (colour ? settings_.deck[0].colour : settings_.deck[0].preset) = i < items.size() ? items[i].name : "";
            loadPresetIntoController(colour);
            publishToProcessor(); // a choice is not automation: always, also when the host delivers changes itself
        }
        if (id == kLookId) setLatencyFrom(plainOf(kLookId));
    }
    // a value the host gave the controller: to the processing too, unless the host delivers changes there itself
    // (then its automation stays sample-accurate and is not overtaken by the controller's later copies)
    if (!loading_ && !hostRoutes_.load()) publishToProcessor();
    if (view_) view_->refresh();
    return r;
}

void Plugin::publishToProcessor() {
    incoming_.publish(Snapshot{exact_, plainOf(kLookId), plainOf(kBypassId) >= 0.5});
}

void Plugin::edit(ParamID id, ParamValue normalized) {
    beginEdit(id);
    performEdit(id, normalized); // a host passes it to process() - not every host does: also directly, below
    endEdit(id);
    setParamNormalized(id, normalized);
    publishToProcessor();
}

void Plugin::editPlain(ParamID id, double plain) {
    const int p = paramIndex(id);
    edit(id, p >= 0 ? normalizedOf(p, plain) : plain);
}

void Plugin::choose(bool colour, int index) {
    const std::vector<Preset>& items = colour ? settings_.colours : settings_.presets;
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    const ParamID id = colour ? kColourId : kPresetId;
    if (index != (colour ? colourIndex() : presetIndex())) {
        edit(id, static_cast<double>(index) / static_cast<double>(items.size()));
        return;
    }
    // already chosen: its values again (after changes), exactly, to the controller and the processing
    loadPresetIntoController(colour);
    publishToProcessor();
}

IPlugView* PLUGIN_API Plugin::createView(FIDString name) {
    if (!name || std::strcmp(name, ViewType::kEditor) != 0) return nullptr;
    view_ = new PluginView(this);
    return view_;
}

void Plugin::viewClosed(PluginView* view) {
    if (view_ == view) view_ = nullptr;
}

} // namespace ap::plugin
