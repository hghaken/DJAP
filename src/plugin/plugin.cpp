// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin.h"

#include "pluginids.h"
#include "pluginview.h"
#include "settingslock.h"

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
#include <mutex>
#include <map>
#include <utility>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace ap::plugin {

namespace {

// 2 (0.1.145): the oversampling after the values; a version 1 state (without it) loads with the default
// 2: the oversampling (0.1.145), 3: mono (0.1.245), 4: the preset and colour values the project's values were based on
// (0.1.251)
constexpr int32 kStateVersion = 4;
// The look-ahead parameter: a list of the Setup window's choices (ms; deck.h snapLook) - the host shows what runs
// (outside review: a 0..5 ms range in steps of 0.1 showed 0.5 or 1.0 while 1.5 ran).
constexpr int kLookSteps = kLookChoiceCount - 1;
double lookOfNormalized(double v) {
    const double c = v >= 0.0 ? (v <= 1.0 ? v : 1.0) : 0.0; // NaN: 0
    return kLookChoicesMs[std::lround(c * kLookSteps)];
}
double normalizedOfLook(double ms) {
    const double s = snapLook(ms);
    for (int i = 0; i <= kLookSteps; ++i)
        if (kLookChoicesMs[i] == s) return static_cast<double>(i) / kLookSteps;
    return 1.0 / kLookSteps; // (not reached: snapLook gives one of the choices)
}

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
    // a test with editorhost that writes (the windows' places, a Setup switch) points it at a scratch copy, never at
    // the user's own file
    wchar_t test[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"DJAP_PLUGIN_INI", test, MAX_PATH) > 0) return std::filesystem::path(test);
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

// The look-ahead in ms: a list of 0, 1.5, 2, 3, 5.
class LookParameter : public Parameter {
public:
    explicit LookParameter(double ms) {
        ascii(info.title, "Look-ahead");
        ascii(info.shortTitle, "Look-ahead");
        ascii(info.units, "ms");
        info.id = kLookId;
        info.unitId = kRootUnitId;
        info.stepCount = kLookSteps;
        info.flags = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        info.defaultNormalizedValue = normalizedOfLook(1.5);
        setNormalized(toNormalized(ms));
    }
    ParamValue toPlain(ParamValue v) const override { return lookOfNormalized(v); }
    ParamValue toNormalized(ParamValue ms) const override { return normalizedOfLook(ms); }
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

// The oversampling of the soft clipper and the exciter: a list of 2x, 4x, 8x.
class OversamplingParameter : public Parameter {
public:
    OversamplingParameter() {
        ascii(info.title, "Oversampling");
        ascii(info.shortTitle, "Oversampling");
        info.id = kOversamplingId;
        info.unitId = kRootUnitId;
        info.stepCount = 2;
        info.flags = ParameterInfo::kCanAutomate | ParameterInfo::kIsList;
        info.defaultNormalizedValue = 0.5; // 4x
        setNormalized(0.5);
    }
    ParamValue toPlain(ParamValue v) const override { return static_cast<double>(2 << std::lround(v * 2.0)); }
    ParamValue toNormalized(ParamValue f) const override { return f >= 6 ? 1.0 : f >= 3 ? 0.5 : 0.0; }
    void toString(ParamValue v, String128 out) const override {
        char text[8];
        std::snprintf(text, sizeof text, "%dx", static_cast<int>(toPlain(v)));
        ascii(out, text);
    }
    bool fromString(const TChar* string, ParamValue& v) const override {
        const int f = std::atoi(StringConvert::convert(string).c_str());
        if (f != 2 && f != 4 && f != 8) return false;
        v = toNormalized(f);
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

// The instance numbers in use in this process (Settings::pluginSlots): each instance takes the lowest free one at
// initialize - the first one open is 1 (deck 1 in a DJ program that opens its decks' plug-ins in order) - and frees
// it at terminate.
std::mutex g_slotLock;
std::map<int, Plugin*> g_slots; // the instances by number (the linked ones share their state through it)

int takeSlot(Plugin* p) {
    const std::lock_guard<std::mutex> lock(g_slotLock);
    int n = 1;
    while (g_slots.count(n)) ++n;
    g_slots[n] = p;
    return n;
}

void freeSlot(int n) {
    const std::lock_guard<std::mutex> lock(g_slotLock);
    g_slots.erase(n);
}

std::vector<std::pair<int, Plugin*>> liveInstances() { // by number, the first one open first
    const std::lock_guard<std::mutex> lock(g_slotLock);
    return {g_slots.begin(), g_slots.end()};
}

} // namespace

Plugin::Plugin() = default;
Plugin::~Plugin() {
    if (slot_ > 0) freeSlot(slot_); // a host that never called terminate
}

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
    // this instance's last choices (client, 2026-10-30: the DJ program keeps no plug-in state - after a restart the
    // plug-in was back on Medium and Neutral); a preset or colour no longer in the lists: Medium, Neutral
    // linked: every instance starts with the first one's choices
    const auto kept = settings_.pluginSlots.find(settings_.pluginLink ? 1 : slot_);
    if (kept != settings_.pluginSlots.end()) {
        const PluginSlot& c = kept->second;
        if (settings_.find(c.preset)) settings_.deck[0].preset = c.preset;
        if (settings_.find(c.colour, PresetKind::Colour)) settings_.deck[0].colour = c.colour;
        lookMs_ = snapLook(c.lookAheadMs);
        oversampling_ = snapOversampling(c.oversampling);
        mono_ = c.mono;
    }
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
        // "EQ": the colour's name in the windows since 2026-10-30 (client); the id stays
        auto* param = new NameListParameter(colour ? "EQ" : "Loudness", colour ? kColourId : kPresetId,
                                            colour ? &settings_.colours : &settings_.presets);
        const ParamValue norm = static_cast<double>(colour ? colourIndex() : presetIndex()) / param->getInfo().stepCount;
        param->getInfo().defaultNormalizedValue = norm;
        param->setNormalized(norm);
        parameters.addParameter(param);
    }
    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                            kBypassId);
    parameters.addParameter(new LookParameter(lookMs_));
    parameters.addParameter(new OversamplingParameter());
    parameters.addParameter(STR16("Mono"), nullptr, 1, 0.0, ParameterInfo::kCanAutomate, kMonoId); // 0 stereo, 1 mono
    // the instance's kept choices (loadLists) are also these three parameters' defaults, as the lists' are: the DJ
    // program sets every parameter back to its default at its start
    for (const auto& [id, v] : {std::pair<ParamID, ParamValue>{kLookId, normalizedOfLook(lookMs_)},
                                {kOversamplingId, oversampling_ == 8 ? 1.0 : oversampling_ == 4 ? 0.5 : 0.0},
                                {kMonoId, mono_ ? 1.0 : 0.0}})
        if (Parameter* param = parameters.getParameter(id)) {
            param->getInfo().defaultNormalizedValue = v;
            param->setNormalized(v);
        }
    // not automatable: only for a host that keeps parameter values for its set (client, 2026-10-29)
    // it starts at 1, a value no stamp has: a host that kept a stamp for its set always restores it (one that restores
    // only the values that differ would otherwise skip an equal one), and a restore without it is a set from before
    // 0.1.253; checkStamp sets the real stamp
    parameters.addParameter(STR16("Preset state"), nullptr, 0, 1.0, 0, kStampId);
    // the start's values (the instance's kept preset and colour) are also the chain values' defaults (client,
    // 2026-10-30: "the leveller starts at -12 and does not come back"): the DJ program sets every parameter that is not
    // at its default back to the default at its start, and hands that to the processing too - the window showed the
    // kept preset while the sound ran the parameters' generic defaults. With these defaults it has nothing to set.
    for (int p = 0; p < kParamCount; ++p) {
        auto* param = new ChainParameter(p, unitOf(p));
        const ParamValue start = normalizedOf(p, values_[static_cast<size_t>(p)]);
        param->getInfo().defaultNormalizedValue = start;
        param->setNormalized(start);
        parameters.addParameter(param);
    }
}

tresult PLUGIN_API Plugin::initialize(FUnknown* context) {
    const tresult r = SingleComponentEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    if (slot_ <= 0) slot_ = takeSlot(this);
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
    if (slot_ > 0) freeSlot(slot_);
    slot_ = 0;
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
        checkStamp(); // a set restored by the host whose preset has changed since: the preset's values now
        Snapshot s;
        if (incoming_.take(s))
            values_ = s.values, lookMs_ = s.lookMs, bypass_ = s.bypass, oversampling_ = s.oversampling, mono_ = s.mono;
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
    ctl.oversampling = oversampling_;
    ctl.mono = mono_;
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
        oversampling_ = s.oversampling;
        mono_ = s.mono;
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
                lookMs_ = lookOfNormalized(v); // the window's choices (deck.h)
            } else if (id == kOversamplingId) {
                oversampling_ = 2 << std::lround(std::min(1.0, std::max(0.0, v)) * 2.0);
            } else if (id == kMonoId) {
                mono_ = v >= 0.5;
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
    s.writeInt32(static_cast<int32>(plainOf(kOversamplingId))); // version 2
    s.writeInt8(plainOf(kMonoId) >= 0.5 ? 1 : 0);                // version 3
    // version 4: the chosen preset's and colour's values as this plug-in last took them over - so a load can tell a
    // value changed in this project from one the preset has changed since
    s.writeInt32(kParamCount);
    for (int p = 0; p < kParamCount; ++p) {
        s.writeStr8(paramInfo(p).key);
        s.writeDouble(applied_[static_cast<size_t>(p)]);
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
    bool haveAttack = false;
    double oldFastDown = -1.0;
    for (int32 i = 0; i < count; ++i) {
        const std::string key = readText();
        double x = 0;
        if (!s.readDouble(x)) return kResultFalse;
        for (int p = 0; p < kParamCount; ++p)
            if (key == paramInfo(p).key) v[static_cast<size_t>(p)] = x; // unknown keys (a newer version): skipped
        haveAttack = haveAttack || key == paramInfo(pLevAttack).key;
        if (key == "LevellerFastDown") oldFastDown = x;
    }
    // a project of the versions before Attack / Release: its "Fast down" (dB/s; 0 = Speed) is the Attack in seconds
    // per 10 dB (as Settings does for a saved preset)
    if (!haveAttack && oldFastDown >= 0.0) v[pLevAttack] = attackFromFastDown(oldFastDown, v[pLevSpeed]);
    normaliseValues(v);
    int32 oversampling = kOversamplingDefault;
    if (version >= 2 && s.readInt32(oversampling)) oversampling = snapOversampling(oversampling);
    else oversampling = kOversamplingDefault;
    int8 mono = 0; // a project of a version before 3: stereo
    if (version < 3 || !s.readInt8(mono)) mono = 0;
    // the preset and the colour as they are in the settings file now win over the project's copy of them (client,
    // 2026-10-29: "some settings are not taken over after the start of the plug-in - noise and stereo": a set kept
    // the values of before a change of its preset): three-way, as edited() does - a value the project left as its
    // preset had it (version 4 stores that base) takes the preset's value now, a value changed in the project (a
    // bass slider, the spectrum window, automation) stays. A project without that base (before 0.1.251) takes the
    // named preset's and colour's values now. Values of a preset or colour that is not in the file stay as stored.
    PresetValues base = v;
    bool haveBase = false;
    int32 baseCount = 0;
    if (version >= 4 && s.readInt32(baseCount)) {
        haveBase = true;
        for (int32 i = 0; i < baseCount; ++i) {
            const std::string key = readText();
            double x = 0;
            if (!s.readDouble(x)) {
                haveBase = false;
                break;
            }
            for (int p = 0; p < kParamCount; ++p)
                if (key == paramInfo(p).key) base[static_cast<size_t>(p)] = x;
        }
    }
    {
        const Preset* pp = settings_.find(preset);
        const Preset* cp = settings_.find(colour, PresetKind::Colour);
        for (int p = 0; p < kParamCount; ++p) {
            const Preset* from = isColourParam(p) ? cp : pp;
            const auto k = static_cast<size_t>(p);
            if (from && (!haveBase || v[k] == base[k])) v[k] = from->values[k];
        }
        normaliseValues(v);
    }

    // the controller
    exact_ = v;
    settings_.deck[0].preset = settings_.find(preset) ? preset : std::string(); // unknown here: "(other values)"
    settings_.deck[0].colour = settings_.find(colour, PresetKind::Colour) ? colour : std::string();
    loading_ = true;
    SingleComponentEffect::setParamNormalized(kPresetId, static_cast<double>(presetIndex()) / settings_.presets.size());
    SingleComponentEffect::setParamNormalized(kColourId, static_cast<double>(colourIndex()) / settings_.colours.size());
    SingleComponentEffect::setParamNormalized(kBypassId, bypass ? 1.0 : 0.0);
    look = snapLook(look); // an old project's 0.5 / 1.0 ms runs - and is reported - as 1.5 from the start
    SingleComponentEffect::setParamNormalized(kLookId, normalizedOfLook(look));
    SingleComponentEffect::setParamNormalized(kOversamplingId, oversampling == 8 ? 1.0 : oversampling == 4 ? 0.5 : 0.0);
    SingleComponentEffect::setParamNormalized(kMonoId, mono ? 1.0 : 0.0);
    for (int p = 0; p < kParamCount; ++p)
        SingleComponentEffect::setParamNormalized(paramId(p), normalizedOf(p, v[static_cast<size_t>(p)]));
    loading_ = false;
    applied_ = chosenValues();
    updateStamp(false);
    setLatencyFrom(look);

    // the processing (exactly the stored values, not their positions)
    incoming_.publish(Snapshot{v, look, bypass != 0, oversampling, mono != 0});
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
    if (id == kLookId) return lookOfNormalized(v); // what runs (deck.h)
    if (id == kBypassId || id == kMonoId) return v >= 0.5 ? 1.0 : 0.0;
    if (id == kOversamplingId) return static_cast<double>(2 << std::lround(v * 2.0));
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
    std::vector<ParamID> changed;
    for (int p = colour ? kFirstColourParam : 0; p < (colour ? kParamCount : kFirstColourParam); ++p) {
        exact_[static_cast<size_t>(p)] = v[static_cast<size_t>(p)];
        const ParamValue n = normalizedOf(p, v[static_cast<size_t>(p)]);
        if (getParamNormalized(paramId(p)) != n) changed.push_back(paramId(p));
        SingleComponentEffect::setParamNormalized(paramId(p), n);
    }
    loading_ = false;
    applied_ = chosenValues();
    updateStamp(fromWindow_);
    // the host reads the new values; the processing gets them as a whole (publishToProcessor)
    if (componentHandler) componentHandler->restartComponent(kParamValuesChanged);
    // a choice in the window: the host keeps each new value too (client, 2026-10-29: the DJ program restored the
    // values from before the preset was chosen - Noise and Stereo width off - at its next start)
    if (fromWindow_)
        for (const ParamID id : changed) tellHost(id);
}

double Plugin::stampOf(const PresetValues& v) {
    uint32_t h = 2166136261u; // FNV-1a over the values' bits
    for (const double x : v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &x, sizeof bits);
        for (int b = 0; b < 8; ++b) h = (h ^ static_cast<uint8_t>(bits >> (8 * b))) * 16777619u;
    }
    return static_cast<double>(h % 65536u) / 65536.0; // exact in a float, as a host may keep it
}

void Plugin::updateStamp(bool tell) {
    if (!stampChecked_) return; // before the first activation the stamp stays 1 (a restore must replace it)
    const double s = stampOf(applied_);
    if (getParamNormalized(kStampId) == s) return;
    const bool was = loading_;
    loading_ = true;
    SingleComponentEffect::setParamNormalized(kStampId, s);
    loading_ = was;
    if (tell) tellHost(kStampId);
}

void Plugin::checkStamp() {
    if (stampChecked_) return; // once: at the first activation or window (the restore came right after initialize)
    stampChecked_ = true;
    // reload when the host restored a stamp that is not the preset's now (the preset changed since the set was kept),
    // or restored values without any stamp (a set kept before 0.1.253: its values may be the preset's of before - the
    // client's sets kept Noise and Stereo width off after they had been switched on in the preset)
    const bool changed = stampRestored_ ? std::fabs(getParamNormalized(kStampId) - stampOf(chosenValues())) >= 1e-6
                                        : chainRestored_;
    stampRestored_ = chainRestored_ = false;
    if (changed) { // the preset's and colour's values now, told to the host
        fromWindow_ = true;
        loadPresetIntoController(false);
        loadPresetIntoController(true);
        fromWindow_ = false;
        publishToProcessor();
        if (view_) view_->refresh();
    }
    updateStamp(true); // the real stamp (it started at 1), kept by the host with the set
}

void Plugin::tellHost(ParamID id) {
    beginEdit(id);
    performEdit(id, getParamNormalized(id));
    endEdit(id);
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
    bool ok = false;
    {
        const SettingsLock lock; // the program or another instance saving at the same moment waits (settingslock.h)
        ok = settings_.saveMerged(ini_, base_, Settings::SaveRole::Guest);
    }
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
    std::vector<ParamID> changed;
    loading_ = true;
    for (int i = 0; i < kParamCount; ++i) {
        const auto k = static_cast<size_t>(i);
        if (now[k] == applied_[k] || now[k] == exact_[k]) continue;
        exact_[k] = now[k];
        SingleComponentEffect::setParamNormalized(paramId(i), normalizedOf(i, now[k]));
        changed.push_back(paramId(i));
    }
    loading_ = false;
    applied_ = now;
    updateStamp(true);
    const bool kept = rememberChoice(false); // "Save as" moved the plug-in to its copy, a rename renamed the choice
    if (!changed.empty()) {
        if (componentHandler) componentHandler->restartComponent(kParamValuesChanged);
        for (const ParamID id : changed) tellHost(id); // the host keeps the preset window's changes for its set too
        publishToProcessor();
    }
    if (listChanged) listsChanged();
    if (save || kept) saveSettings();
    if (!changed.empty() || listChanged || kept) shareState();
    if (view_) view_->refresh();
}

void Plugin::openEditor(bool colour, HWND owner, const std::string& tab) {
    saveSettings(); // the lists as they are in the file now (another plug-in or the program may have added some)
    std::unique_ptr<EditWindow>& e = colour ? colourEditor_ : editor_;
    if (!e) {
        e = std::make_unique<EditWindow>(settings_, [this](bool listChanged, bool save) { edited(listChanged, save); },
                                         colour ? PresetKind::Colour : PresetKind::Dynamics);
        e->setDeckName(L"this plug-in");
        const std::wstring deck = L"DJAP Deck " + std::to_wstring(slot_); // the deck, not the preset (client, 2026-10-30)
        e->setWindowTitle((colour ? L"Settings EQ: " : L"Settings Loudness: ") + deck);
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
    if (id == kStampId && !loading_) stampRestored_ = true; // only a host restoring a set sets it
    if (!loading_ && !stampChecked_ && paramIndex(id) >= 0) chainRestored_ = true;
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
    incoming_.publish(Snapshot{exact_, plainOf(kLookId), plainOf(kBypassId) >= 0.5,
                               static_cast<int>(plainOf(kOversamplingId)), plainOf(kMonoId) >= 0.5});
}

void Plugin::edit(ParamID id, ParamValue normalized) {
    beginEdit(id);
    performEdit(id, normalized); // a host passes it to process() - not every host does: also directly, below
    endEdit(id);
    setParamNormalized(id, normalized);
    publishToProcessor();
    if (id == kPresetId || id == kColourId || id == kLookId || id == kOversamplingId || id == kMonoId) rememberChoice(true);
    if (id != kBypassId) shareState(); // Bypass stays each deck's own
}

void Plugin::setLinked(bool on) {
    for (const auto& [n, p] : liveInstances()) p->settings_.pluginLink = on; // every instance's copy
    saveSettings(); // merged into the file three-way, as the two switches
    if (!on) return;
    const auto live = liveInstances();
    if (!live.empty()) live.front().second->shareState(); // the first one open leads
}

void Plugin::shareState() {
    if (!settings_.pluginLink || slot_ <= 0) return;
    for (const auto& [n, p] : liveInstances())
        if (p != this) p->takeState(*this);
}

void Plugin::takeState(Plugin& from) {
    saveSettings(); // the lists as the file has them now (the other one may just have changed a preset)
    auto own = [](const std::vector<Preset>& items, const std::string& name) {
        for (const Preset& p : items)
            if (p.name == name) return name;
        return std::string(); // not in this instance's lists: "(other values)"
    };
    settings_.deck[0].preset = own(settings_.presets, from.settings_.deck[0].preset);
    settings_.deck[0].colour = own(settings_.colours, from.settings_.deck[0].colour);
    exact_ = from.exact_;
    std::vector<ParamID> changed;
    loading_ = true;
    auto set = [&](ParamID id, ParamValue v) {
        if (getParamNormalized(id) == v) return;
        SingleComponentEffect::setParamNormalized(id, v);
        changed.push_back(id);
    };
    set(kPresetId, static_cast<double>(presetIndex()) / settings_.presets.size());
    set(kColourId, static_cast<double>(colourIndex()) / settings_.colours.size());
    for (const ParamID id : {kLookId, kOversamplingId, kMonoId}) set(id, from.getParamNormalized(id));
    for (int p = 0; p < kParamCount; ++p) set(paramId(p), normalizedOf(p, exact_[static_cast<size_t>(p)]));
    loading_ = false;
    applied_ = chosenValues();
    updateStamp(true);
    setLatencyFrom(plainOf(kLookId));
    if (componentHandler) componentHandler->restartComponent(kParamValuesChanged);
    for (const ParamID id : changed) tellHost(id); // the host keeps them, as the window's own changes
    publishToProcessor();
    rememberChoice(true);
    if (view_) view_->refresh();
}

bool Plugin::rememberChoice(bool save) {
    if (slot_ <= 0) return false;
    const auto old = settings_.pluginSlots.find(slot_);
    PluginSlot c = old != settings_.pluginSlots.end() ? old->second : PluginSlot();
    if (!settings_.deck[0].preset.empty()) c.preset = settings_.deck[0].preset; // "(other values)": the last name stays
    if (!settings_.deck[0].colour.empty()) c.colour = settings_.deck[0].colour;
    c.lookAheadMs = plainOf(kLookId);
    c.oversampling = static_cast<int>(plainOf(kOversamplingId));
    c.mono = plainOf(kMonoId) >= 0.5;
    if (old != settings_.pluginSlots.end() && old->second == c) return false;
    settings_.pluginSlots[slot_] = c;
    if (save) saveSettings();
    return true;
}

void Plugin::editPlain(ParamID id, double plain) {
    const int p = paramIndex(id);
    edit(id, p >= 0 ? normalizedOf(p, plain) : plain);
}

void Plugin::setSwitches(bool tooltips, bool checkUpdates) {
    settings_.showTooltips = tooltips;
    settings_.checkUpdates = checkUpdates;
    saveSettings(); // as a guest: these two (changed since base_) and nothing else of the program's
}

void Plugin::choose(bool colour, int index) {
    const std::vector<Preset>& items = colour ? settings_.colours : settings_.presets;
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    const ParamID id = colour ? kColourId : kPresetId;
    fromWindow_ = true; // the values the choice changes are told to the host (loadPresetIntoController)
    if (index != (colour ? colourIndex() : presetIndex())) {
        edit(id, static_cast<double>(index) / static_cast<double>(items.size()));
    } else {
        // already chosen: its values again (after changes), exactly, to the controller and the processing
        loadPresetIntoController(colour);
        publishToProcessor();
        shareState();
    }
    fromWindow_ = false;
}

IPlugView* PLUGIN_API Plugin::createView(FIDString name) {
    if (!name || std::strcmp(name, ViewType::kEditor) != 0) return nullptr;
    checkStamp(); // (a host that opens the window before it activates the plug-in)
    if (view_) view_->detach(); // one window at a time: an older one still alive goes empty, never unowned
    view_ = new PluginView(this);
    return view_;
}

void Plugin::viewClosed(PluginView* view) {
    if (view_ == view) view_ = nullptr;
}

} // namespace ap::plugin
