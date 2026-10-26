#include "plugin.h"

#include "pluginids.h"
#include "pluginview.h"
#include "version.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
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

// The program's settings file, where the user's own presets and colours are (read only here). A portable program
// (DJAP.ini next to DJAP.exe) is not found; the shipped lists are used then.
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

} // namespace

std::atomic<int> g_nextId{0};

Plugin::Plugin() : id_(++g_nextId) {
    diagRegister(this);
    diagLog("#%d created (%d instance(s) live)", id_, diagLiveInstances());
}

Plugin::~Plugin() {
    diagUnregister(this);
    diagLog("#%d destroyed", id_);
}

ParamValue Plugin::normalizedOf(int p, double plain) {
    return static_cast<double>(paramPositionOf(p, plain)) / paramPositions(p);
}

double Plugin::plainFromNormalized(int p, ParamValue v) const {
    return paramValueAt(p, static_cast<int>(std::lround(v * paramPositions(p))));
}

void Plugin::loadLists() {
    Settings s; // the shipped presets and colours
    const std::filesystem::path file = settingsFile();
    std::error_code ec;
    if (!file.empty() && std::filesystem::exists(file, ec)) s.load(file);
    presets_ = s.presets;
    colours_ = s.colours;
    // the start: Medium with Neutral, as in the program
    values_ = withColour(s.valuesOf("Medium"), s.colourValuesOf("Neutral"));
    exact_ = values_;
}

void Plugin::addParameters() {
    for (int u = 0; u < kStageUnits; ++u) {
        String128 name;
        ascii(name, kStages[u]);
        addUnit(new Unit(name, u + 1, kRootUnitId));
    }
    // the lists: the program's names and one more for values that match none of them (a project from elsewhere)
    auto list = [&](const char* title, ParamID id, const std::vector<Preset>& items, const char* other,
                    const std::string& start) {
        String128 t;
        ascii(t, title);
        auto* param = new StringListParameter(t, id, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
        int startIndex = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            String128 n;
            StringConvert::convert(items[i].name, n);
            param->appendString(n);
            if (items[i].name == start) startIndex = static_cast<int>(i);
        }
        String128 o;
        ascii(o, other);
        param->appendString(o);
        const ParamValue norm = static_cast<double>(startIndex) / std::max<int32>(1, param->getInfo().stepCount);
        param->getInfo().defaultNormalizedValue = norm;
        param->setNormalized(norm);
        parameters.addParameter(param);
    };
    list("Preset", kPresetId, presets_, "(other values)", "Medium");
    list("Colour", kColourId, colours_, "(other values)", "Neutral");
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
    if (FUnknownPtr<IHostApplication> host = FUnknownPtr<IHostApplication>(context)) {
        String128 name{};
        if (host->getName(name) == kResultOk) hostName_ = StringConvert::convert(name);
    }
    diagLog("#%d initialize: host \"%s\", DJAP %s", id_, hostName_.c_str(), AP_VERSION_STRING);
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    loadLists();
    addParameters();
    setLatencyFrom(lookMs_);
    return kResultOk;
}

tresult PLUGIN_API Plugin::terminate() {
    diagLog("#%d terminate", id_);
    return SingleComponentEffect::terminate();
}

tresult PLUGIN_API Plugin::setProcessing(TBool state) {
    diagLog("#%d setProcessing(%d)", id_, state ? 1 : 0);
    return kResultOk;
}

tresult PLUGIN_API Plugin::setupProcessing(ProcessSetup& setup) {
    fs_ = setup.sampleRate > 0 ? setup.sampleRate : 48000.0;
    diagLog("#%d setupProcessing: %.0f Hz, at most %d frames, mode %d, %s", id_, setup.sampleRate,
            setup.maxSamplesPerBlock, setup.processMode, setup.symbolicSampleSize == kSample32 ? "32-bit" : "64-bit");
    setLatencyFrom(plainOf(kLookId));
    return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API Plugin::setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs,
                                              int32 numOuts) {
    // stereo in, stereo out only (a deck)
    const bool stereo =
        numIns == 1 && numOuts == 1 && inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo;
    diagLog("#%d setBusArrangements: %d in (0x%llx), %d out (0x%llx): %s", id_, numIns,
            numIns > 0 ? static_cast<unsigned long long>(inputs[0]) : 0ull, numOuts,
            numOuts > 0 ? static_cast<unsigned long long>(outputs[0]) : 0ull, stereo ? "accepted" : "refused");
    if (stereo) return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    return kResultFalse;
}

tresult PLUGIN_API Plugin::canProcessSampleSize(int32 symbolicSampleSize) {
    diagLog("#%d canProcessSampleSize(%s)", id_, symbolicSampleSize == kSample32 ? "32-bit" : "64-bit");
    return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

uint32 PLUGIN_API Plugin::getLatencySamples() { return latency_.load(); }

void Plugin::setLatencyFrom(double lookMs) {
    const auto frames = static_cast<uint32>(std::lround(std::min(kMaxLook, std::max(0.0, lookMs)) * fs_ / 1000.0));
    if (latency_.exchange(frames) != frames && componentHandler) componentHandler->restartComponent(kLatencyChanged);
}

tresult PLUGIN_API Plugin::setActive(TBool state) {
    diagLog("#%d setActive(%d) at %.0f Hz", id_, state ? 1 : 0, fs_);
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
        stats_.updates.fetch_add(1, std::memory_order_relaxed);
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
            stats_.changes.fetch_add(1, std::memory_order_relaxed);
            if (id == kPresetId || id == kColourId) {
                const bool colour = id == kColourId;
                const std::vector<Preset>& items = colour ? colours_ : presets_;
                const auto index = static_cast<size_t>(std::lround(v * static_cast<double>(items.size())));
                if (index < items.size()) // the last entry ("other values") changes nothing
                    for (int p = colour ? kFirstColourParam : 0; p < (colour ? kParamCount : kFirstColourParam); ++p)
                        values_[static_cast<size_t>(p)] = items[index].values[static_cast<size_t>(p)];
            } else if (id == kBypassId) {
                bypass_ = v >= 0.5;
            } else if (id == kLookId) {
                lookMs_ = std::round(v * kMaxLook * 10.0) / 10.0;
            } else {
                const int p = paramIndex(id);
                if (p < 0) continue;
                values_[static_cast<size_t>(p)] = plainFromNormalized(p, v);
            }
            dirty_ = true;
        }
    }
    // what the host hands over (diagnostics: plugindiag.h)
    stats_.calls.fetch_add(1, std::memory_order_relaxed);
    stats_.frames.store(data.numSamples, std::memory_order_relaxed);
    stats_.sampleSize.store(data.symbolicSampleSize, std::memory_order_relaxed);
    stats_.mode.store(data.processMode, std::memory_order_relaxed);
    stats_.inputs.store(data.numInputs, std::memory_order_relaxed);
    stats_.outputs.store(data.numOutputs, std::memory_order_relaxed);
    stats_.inChannels.store(data.numInputs > 0 && data.inputs ? data.inputs[0].numChannels : -1,
                            std::memory_order_relaxed);
    stats_.outChannels.store(data.numOutputs > 0 && data.outputs ? data.outputs[0].numChannels : -1,
                             std::memory_order_relaxed);
    stats_.bypass.store(bypass_, std::memory_order_relaxed);
    if (!active_) return kResultOk;
    if (dirty_) publishConfig();
    if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
    if (data.symbolicSampleSize != kSample32) return kResultOk; // never 64-bit buffers read as 32-bit ones
    AudioBusBuffers& in = data.inputs[0];
    AudioBusBuffers& out = data.outputs[0];
    if (in.numChannels < 2 || out.numChannels < 2 || !in.channelBuffers32 || !out.channelBuffers32) return kResultOk;
    float* l = out.channelBuffers32[0];
    float* r = out.channelBuffers32[1];
    const int n = data.numSamples;
    stats_.inPlace.store(l == in.channelBuffers32[0] ? 1 : 0, std::memory_order_relaxed);
    float inPeak = 0.0f;
    for (int i = 0; i < n; ++i)
        inPeak = std::max(inPeak, std::max(std::fabs(in.channelBuffers32[0][i]), std::fabs(in.channelBuffers32[1][i])));
    const size_t bytes = static_cast<size_t>(n) * sizeof(float);
    if (l != in.channelBuffers32[0]) std::memcpy(l, in.channelBuffers32[0], bytes);
    if (r != in.channelBuffers32[1]) std::memcpy(r, in.channelBuffers32[1], bytes);
    deck_.process(l, r, n);
    out.silenceFlags = 0;
    float outPeak = 0.0f;
    for (int i = 0; i < n; ++i) outPeak = std::max(outPeak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    stats_.inPeak.store(inPeak, std::memory_order_relaxed);
    stats_.outPeak.store(outPeak, std::memory_order_relaxed);
    return kResultOk;
}

// ---- state: the values themselves, by ini key (a project keeps its sound without the program's presets) ----

tresult PLUGIN_API Plugin::getState(IBStream* state) {
    diagLog("#%d getState", id_);
    IBStreamer s(state, kLittleEndian);
    const int pi = presetIndex(), ci = colourIndex();
    s.writeInt32(kStateVersion);
    s.writeStr8(pi < static_cast<int>(presets_.size()) ? presets_[static_cast<size_t>(pi)].name.c_str() : "");
    s.writeStr8(ci < static_cast<int>(colours_.size()) ? colours_[static_cast<size_t>(ci)].name.c_str() : "");
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
    diagLog("#%d setState", id_);
    IBStreamer s(state, kLittleEndian);
    int32 version = 0, count = 0;
    if (!s.readInt32(version) || version < 1) return kResultFalse;
    auto readText = [&s]() {
        std::unique_ptr<char8[]> t(s.readStr8());
        return std::string(t ? t.get() : "");
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
    loading_ = true;
    auto indexOf = [](const std::vector<Preset>& items, const std::string& name) {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].name == name) return static_cast<double>(i);
        return static_cast<double>(items.size()); // "(other values)"
    };
    SingleComponentEffect::setParamNormalized(kPresetId, indexOf(presets_, preset) / presets_.size());
    SingleComponentEffect::setParamNormalized(kColourId, indexOf(colours_, colour) / colours_.size());
    SingleComponentEffect::setParamNormalized(kBypassId, bypass ? 1.0 : 0.0);
    SingleComponentEffect::setParamNormalized(kLookId, std::min(kMaxLook, std::max(0.0, look)) / kMaxLook);
    for (int p = 0; p < kParamCount; ++p)
        SingleComponentEffect::setParamNormalized(paramId(p), normalizedOf(p, v[static_cast<size_t>(p)]));
    loading_ = false;
    setLatencyFrom(look);

    // the processing (exactly the stored values, not their positions)
    incoming_.publish(Snapshot{v, std::min(kMaxLook, std::max(0.0, look)), bypass != 0});
    if (view_) view_->refresh();
    return kResultOk;
}

// ---- controller ----

int Plugin::presetIndex() const {
    return static_cast<int>(std::lround(const_cast<Plugin*>(this)->getParamNormalized(kPresetId) * presets_.size()));
}

int Plugin::colourIndex() const {
    return static_cast<int>(std::lround(const_cast<Plugin*>(this)->getParamNormalized(kColourId) * colours_.size()));
}

double Plugin::plainOf(ParamID id) const {
    const ParamValue v = const_cast<Plugin*>(this)->getParamNormalized(id);
    if (id == kLookId) return std::round(v * kMaxLook * 10.0) / 10.0;
    if (id == kBypassId) return v >= 0.5 ? 1.0 : 0.0;
    const int p = paramIndex(id);
    return p >= 0 ? exact_[static_cast<size_t>(p)] : v;
}

PresetValues Plugin::currentValues() const { return exact_; }

void Plugin::loadPresetIntoController(int index, bool colour) {
    const std::vector<Preset>& items = colour ? colours_ : presets_;
    if (index < 0 || index >= static_cast<int>(items.size())) return; // "(other values)"
    loading_ = true;
    const PresetValues& v = items[static_cast<size_t>(index)].values;
    for (int p = colour ? kFirstColourParam : 0; p < (colour ? kParamCount : kFirstColourParam); ++p) {
        exact_[static_cast<size_t>(p)] = v[static_cast<size_t>(p)];
        SingleComponentEffect::setParamNormalized(paramId(p), normalizedOf(p, v[static_cast<size_t>(p)]));
    }
    loading_ = false;
    // the host reads the new values (the processing loads the same preset from the list parameter's change)
    if (componentHandler) componentHandler->restartComponent(kParamValuesChanged);
}

tresult PLUGIN_API Plugin::setParamNormalized(ParamID id, ParamValue value) {
    const ParamValue before = getParamNormalized(id);
    const tresult r = SingleComponentEffect::setParamNormalized(id, value);
    const int p = paramIndex(id);
    if (p >= 0 && !loading_ && value != normalizedOf(p, exact_[static_cast<size_t>(p)]))
        exact_[static_cast<size_t>(p)] = plainFromNormalized(p, value); // moved to another step
    if (!loading_ && before != value) {
        if (id == kPresetId) loadPresetIntoController(presetIndex(), false);
        if (id == kColourId) loadPresetIntoController(colourIndex(), true);
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
    const std::vector<Preset>& items = colour ? colours_ : presets_;
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    const ParamID id = colour ? kColourId : kPresetId;
    if (index != (colour ? colourIndex() : presetIndex())) {
        edit(id, static_cast<double>(index) / static_cast<double>(items.size()));
        return;
    }
    // already chosen: its values again (after changes), exactly, to the controller and the processing
    loadPresetIntoController(index, colour);
    publishToProcessor();
}

IPlugView* PLUGIN_API Plugin::createView(FIDString name) {
    diagLog("#%d createView(%s)", id_, name ? name : "");
    if (!name || std::strcmp(name, ViewType::kEditor) != 0) return nullptr;
    view_ = new PluginView(this);
    return view_;
}

void Plugin::viewClosed(PluginView* view) {
    if (view_ == view) view_ = nullptr;
}

} // namespace ap::plugin
