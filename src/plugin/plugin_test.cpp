// SPDX-License-Identifier: GPL-3.0-or-later
// plugin_test: loads the built DJAP.vst3 as a host does (the SDK's hosting classes) and checks that it sounds
// exactly like the program's deck: bit-exact against a DeckProcessor with the same values, at odd block sizes,
// after a parameter change, after a project's state is stored and loaded into a new instance, in Bypass; and that
// it tells the host its latency. No audio device: everything runs in memory.
// Usage: plugin_test [path to DJAP.vst3]   (default: the bundle of the same build)
#include "check.h"
#include "deck.h"
#include "params.h"
#include "pluginids.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr double kRate = 48000.0;
constexpr int kMaxBlock = 1024;

// A test signal: a tone and noise that swell, so leveller, compressor and limiter all work.
std::vector<float> signal(int frames, int channel) {
    std::vector<float> x(static_cast<size_t>(frames));
    uint32_t seed = 12345u + static_cast<uint32_t>(channel) * 777u;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<int32_t>(seed) / 2147483648.0);
        const double t = i / kRate;
        const double swell = 0.2 + 0.8 * (0.5 + 0.5 * std::sin(2 * 3.14159265358979 * t / 3.0));
        x[static_cast<size_t>(i)] =
            static_cast<float>(swell * (0.4 * std::sin(2 * 3.14159265358979 * 110.0 * t) + 0.15 * noise));
    }
    return x;
}

// The values of a stored state (the plug-in's own format: plugin.cpp getState).
struct State {
    std::string preset, colour;
    double look = 1.5;
    bool bypass = false;
    ap::PresetValues values{};
};

bool parseState(MemoryStream& m, State& st) {
    m.seek(0, IBStream::kIBSeekSet, nullptr);
    IBStreamer s(&m, kLittleEndian);
    int32 version = 0, count = 0;
    if (!s.readInt32(version)) return false;
    auto text = [&s]() {
        std::unique_ptr<char8[]> t(s.readStr8());
        return std::string(t ? t.get() : "");
    };
    st.preset = text();
    st.colour = text();
    int8 bypass = 0;
    if (!s.readDouble(st.look) || !s.readInt8(bypass) || !s.readInt32(count)) return false;
    st.bypass = bypass != 0;
    st.values = ap::defaultValues();
    for (int32 i = 0; i < count; ++i) {
        const std::string key = text();
        double x = 0;
        if (!s.readDouble(x)) return false;
        for (int p = 0; p < ap::kParamCount; ++p)
            if (key == ap::paramInfo(p).key) st.values[static_cast<size_t>(p)] = x;
    }
    return true;
}

struct Instance {
    IPtr<IComponent> component;
    FUnknownPtr<IAudioProcessor> processor;
    FUnknownPtr<IEditController> controller;
    HostProcessData data;

    bool open(const VST3::Hosting::Module::Ptr& module, FUnknown* host) {
        for (const auto& info : module->getFactory().classInfos())
            if (info.category() == kVstAudioEffectClass) {
                component = module->getFactory().createInstance<IComponent>(info.ID());
                break;
            }
        if (!component || component->initialize(host) != kResultOk) return false;
        processor = FUnknownPtr<IAudioProcessor>(component);
        controller = FUnknownPtr<IEditController>(component);
        if (!processor || !controller) return false;
        SpeakerArrangement st = SpeakerArr::kStereo;
        if (processor->setBusArrangements(&st, 1, &st, 1) != kResultTrue) return false;
        ProcessSetup setup{kRealtime, kSample32, kMaxBlock, kRate};
        if (processor->setupProcessing(setup) != kResultOk) return false;
        component->activateBus(kAudio, kInput, 0, true);
        component->activateBus(kAudio, kOutput, 0, true);
        if (!data.prepare(*component, kMaxBlock, kSample32)) return false;
        return true;
    }
    void start() {
        component->setActive(true);
        processor->setProcessing(true);
    }
    // Processes `frames` frames of in into out (the plug-in's own buffers), with optional parameter changes.
    void run(const float* inL, const float* inR, float* outL, float* outR, int frames, IParameterChanges* changes) {
        data.numSamples = frames;
        std::memcpy(data.inputs[0].channelBuffers32[0], inL, sizeof(float) * static_cast<size_t>(frames));
        std::memcpy(data.inputs[0].channelBuffers32[1], inR, sizeof(float) * static_cast<size_t>(frames));
        data.inputParameterChanges = changes;
        processor->process(data);
        std::memcpy(outL, data.outputs[0].channelBuffers32[0], sizeof(float) * static_cast<size_t>(frames));
        std::memcpy(outR, data.outputs[0].channelBuffers32[1], sizeof(float) * static_cast<size_t>(frames));
        data.inputParameterChanges = nullptr;
    }
    void close() {
        if (!component) return;
        processor->setProcessing(false);
        component->setActive(false);
        data.unprepare();
        component->terminate();
    }
    State state() {
        MemoryStream m;
        component->getState(&m);
        State st;
        CHECK(parseState(m, st));
        return st;
    }
};

// The reference: the program's deck with the values of a state.
void configure(ap::DeckProcessor& deck, const State& st) {
    ap::DeckControl ctl;
    ctl.lookMs = st.look;
    ctl.processing = !st.bypass;
    deck.publish(ap::makeDeckConfig(st.values, ctl, kRate));
}

int differences(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0) {
    int n = 0;
    for (size_t i = from; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}

} // namespace

int main(int argc, char** argv) {
    // the bundle the build made next to this program (tools\run_tests.ps1 runs it without arguments)
    std::string path;
    if (argc >= 2) {
        path = argv[1];
    } else {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const std::string self = exe;
        path = self.substr(0, self.find_last_of('\\')) + "\\VST3\\Release\\DJAP.vst3";
    }
    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) {
        std::printf("FAIL: the plug-in did not load: %s\n", error.c_str());
        return 1;
    }
    HostApplication host;

    // 0. the parameter ids: unique, apart from the four extra ones (a host keys automation and projects on them)
    {
        int clashes = 0;
        for (int p = 0; p < ap::kParamCount; ++p) {
            const ParamID id = ap::plugin::paramId(p);
            clashes += id < 1000;
            for (int q = p + 1; q < ap::kParamCount; ++q) clashes += id == ap::plugin::paramId(q);
            clashes += ap::plugin::paramIndex(id) != p;
        }
        std::printf("-- %d parameter ids, %d clashes\n", ap::kParamCount, clashes);
        CHECK(clashes == 0);
    }
    const int frames = static_cast<int>(kRate * 8);
    const std::vector<float> inL = signal(frames, 0), inR = signal(frames, 1);
    const int blocks[] = {512, 333, 1024, 64, 1};

    // 1. bit-exact with the program's deck, block sizes of every kind, then a parameter change mid-way
    {
        Instance a;
        CHECK(a.open(module, &host));
        const State st = a.state();
        std::printf("-- start: preset %s, colour %s, look-ahead %.1f ms\n", st.preset.c_str(), st.colour.c_str(), st.look);
        CHECK(a.processor->getLatencySamples() == static_cast<uint32>(std::lround(st.look * kRate / 1000.0)));
        a.start();
        ap::DeckProcessor ref;
        ref.prepare(kRate);
        configure(ref, st);
        ref.startFadeIn();
        std::vector<float> pl(frames), pr(frames), rl(inL), rr(inR);
        const ParamID target = ap::plugin::paramId(ap::pTarget);
        int pos = 0, b = 0;
        bool changed = false;
        while (pos < frames) {
            const int n = std::min(blocks[b++ % 5], frames - pos);
            ParameterChanges changes(1);
            IParameterChanges* withChange = nullptr;
            State now = st;
            if (!changed && pos >= frames / 2) { // the target from -14 to -10 LUFS, as a host's automation
                changed = true;
                int32 qi = 0, pi = 0;
                IParamValueQueue* q = changes.addParameterData(target, qi);
                const double norm = static_cast<double>(ap::paramPositionOf(ap::pTarget, -10.0)) /
                                    ap::paramPositions(ap::pTarget);
                q->addPoint(0, norm, pi);
                withChange = &changes;
                now.values[ap::pTarget] = -10.0;
                configure(ref, now);
            }
            a.run(&inL[static_cast<size_t>(pos)], &inR[static_cast<size_t>(pos)], &pl[static_cast<size_t>(pos)],
                  &pr[static_cast<size_t>(pos)], n, withChange);
            ref.process(&rl[static_cast<size_t>(pos)], &rr[static_cast<size_t>(pos)], n);
            pos += n;
        }
        const int diff = differences(pl, rl) + differences(pr, rr);
        std::printf("-- the program's deck and the plug-in, 8 s at blocks 512 / 333 / 1024 / 64 / 1, target changed "
                    "half-way: %d samples differ\n", diff);
        CHECK(diff == 0);
        a.close();
    }

    // 2. a project: values changed (a preset from the list, a colour value), stored, loaded into a new instance
    {
        Instance a;
        CHECK(a.open(module, &host));
        a.start();
        ParameterChanges changes(2);
        int32 qi = 0, pi = 0;
        // the preset list's last-but-one entry, Very Loud among the shipped ones, and the bass level +3 dB
        ParameterInfo info{};
        a.controller->getParameterInfo(0, info); // the preset list (id 1) comes first
        CHECK(info.id == ap::plugin::kPresetId);
        const int items = info.stepCount; // presets in the list (the last step is "(other values)")
        changes.addParameterData(ap::plugin::kPresetId, qi)->addPoint(0, (items - 1.0) / items, pi);
        changes.addParameterData(ap::plugin::paramId(ap::pToneBass), qi)
            ->addPoint(0, static_cast<double>(ap::paramPositionOf(ap::pToneBass, 3.0)) / ap::paramPositions(ap::pToneBass), pi);
        std::vector<float> l(512), r(512);
        a.run(inL.data(), inR.data(), l.data(), r.data(), 512, &changes);
        // the host also tells the controller (as for any automation)
        a.controller->setParamNormalized(ap::plugin::kPresetId, (items - 1.0) / items);
        a.controller->setParamNormalized(ap::plugin::paramId(ap::pToneBass),
                                         static_cast<double>(ap::paramPositionOf(ap::pToneBass, 3.0)) /
                                             ap::paramPositions(ap::pToneBass));
        MemoryStream stored;
        a.component->getState(&stored);
        State st;
        CHECK(parseState(stored, st));
        std::printf("-- stored: preset %s, bass level %+.1f dB, target %.1f LUFS\n", st.preset.c_str(),
                    st.values[ap::pToneBass], st.values[ap::pTarget]);
        CHECK(st.values[ap::pToneBass] == 3.0);
        a.close();

        Instance b;
        CHECK(b.open(module, &host));
        stored.seek(0, IBStream::kIBSeekSet, nullptr);
        CHECK(b.component->setState(&stored) == kResultOk);
        const State back = b.state();
        CHECK(back.preset == st.preset);
        int same = 0;
        for (int p = 0; p < ap::kParamCount; ++p) same += back.values[static_cast<size_t>(p)] == st.values[static_cast<size_t>(p)];
        std::printf("-- loaded into a new instance: %d of %d values the same\n", same, ap::kParamCount);
        CHECK(same == ap::kParamCount);
        b.start();
        ap::DeckProcessor ref;
        ref.prepare(kRate);
        configure(ref, st);
        ref.startFadeIn();
        std::vector<float> pl(frames), pr(frames), rl(inL), rr(inR);
        for (int pos = 0; pos < frames; pos += 512) {
            const int n = std::min(512, frames - pos);
            b.run(&inL[static_cast<size_t>(pos)], &inR[static_cast<size_t>(pos)], &pl[static_cast<size_t>(pos)],
                  &pr[static_cast<size_t>(pos)], n, nullptr);
            ref.process(&rl[static_cast<size_t>(pos)], &rr[static_cast<size_t>(pos)], n);
        }
        const int diff = differences(pl, rl) + differences(pr, rr);
        std::printf("-- the loaded project against the program's deck with the stored values: %d samples differ\n", diff);
        CHECK(diff == 0);
        b.close();
    }

    // 3. Bypass: the input, delayed by the look-ahead, bit for bit (after the 5 ms start fade)
    {
        Instance a;
        CHECK(a.open(module, &host));
        a.start();
        ParameterChanges changes(1);
        int32 qi = 0, pi = 0;
        changes.addParameterData(ap::plugin::kBypassId, qi)->addPoint(0, 1.0, pi);
        std::vector<float> pl(frames), pr(frames);
        for (int pos = 0; pos < frames; pos += 512) {
            const int n = std::min(512, frames - pos);
            a.run(&inL[static_cast<size_t>(pos)], &inR[static_cast<size_t>(pos)], &pl[static_cast<size_t>(pos)],
                  &pr[static_cast<size_t>(pos)], n, pos == 0 ? &changes : nullptr);
        }
        const int look = static_cast<int>(a.processor->getLatencySamples());
        int diff = 0;
        for (int i = static_cast<int>(kRate * 0.05); i < frames; ++i)
            diff += pl[static_cast<size_t>(i)] != inL[static_cast<size_t>(i - look)] ||
                    pr[static_cast<size_t>(i)] != inR[static_cast<size_t>(i - look)];
        std::printf("-- Bypass: the input delayed by %d samples; %d samples differ\n", look, diff);
        CHECK(diff == 0);
        a.close();
    }

    // 3b. a host that never passes parameter changes to process() (one DJ program does not, for a plug-in whose
    // controller and processor are one object): the processing follows the controller itself - a target change
    // half-way, bit-exact with the deck, then Bypass
    {
        Instance a;
        CHECK(a.open(module, &host));
        const State st = a.state();
        a.start();
        ap::DeckProcessor ref;
        ref.prepare(kRate);
        configure(ref, st);
        ref.startFadeIn();
        std::vector<float> pl(frames), pr(frames), rl(inL), rr(inR);
        const int half = frames / 2 / 441 * 441;
        for (int pos = 0; pos < frames; pos += 441) {
            const int n = std::min(441, frames - pos);
            if (pos == half) { // as the window or the host's own slider would: the controller only
                a.controller->setParamNormalized(ap::plugin::paramId(ap::pTarget),
                                                 static_cast<double>(ap::paramPositionOf(ap::pTarget, -10.0)) /
                                                     ap::paramPositions(ap::pTarget));
                State now = st;
                now.values[ap::pTarget] = -10.0;
                configure(ref, now);
            }
            a.run(&inL[static_cast<size_t>(pos)], &inR[static_cast<size_t>(pos)], &pl[static_cast<size_t>(pos)],
                  &pr[static_cast<size_t>(pos)], n, nullptr);
            ref.process(&rl[static_cast<size_t>(pos)], &rr[static_cast<size_t>(pos)], n);
        }
        const int diff = differences(pl, rl) + differences(pr, rr);
        std::printf("-- a host that passes no changes to the processing: the target changed through the controller "
                    "only: %d samples differ\n", diff);
        CHECK(diff == 0);

        a.controller->setParamNormalized(ap::plugin::kBypassId, 1.0);
        const int second = static_cast<int>(kRate);
        std::vector<float> bl(static_cast<size_t>(second)), br(static_cast<size_t>(second));
        for (int pos = 0; pos < second; pos += 441)
            a.run(&inL[static_cast<size_t>(pos)], &inR[static_cast<size_t>(pos)], &bl[static_cast<size_t>(pos)],
                  &br[static_cast<size_t>(pos)], std::min(441, second - pos), nullptr);
        const int look = static_cast<int>(a.processor->getLatencySamples());
        int same = 0;
        for (int i = static_cast<int>(kRate / 2); i < static_cast<int>(kRate); ++i)
            same += bl[static_cast<size_t>(i)] == inL[static_cast<size_t>(i - look)] &&
                    br[static_cast<size_t>(i)] == inR[static_cast<size_t>(i - look)];
        std::printf("-- ... and Bypass through the controller only: %d of %d samples are the delayed input\n", same,
                    static_cast<int>(kRate / 2));
        CHECK(same == static_cast<int>(kRate / 2));
        a.close();
    }

    // 4. the look-ahead parameter changes the latency the host is told
    {
        Instance a;
        CHECK(a.open(module, &host));
        // a list of the window's choices 0, 1.5, 2, 3, 5 ms: 3 ms is the fourth (0.75)
        ParameterInfo pi{};
        for (int32 i = 0; i < a.controller->getParameterCount(); ++i)
            if (a.controller->getParameterInfo(i, pi) == kResultOk && pi.id == ap::plugin::kLookId) break;
        CHECK(pi.id == ap::plugin::kLookId && pi.stepCount == 4 && (pi.flags & ParameterInfo::kIsList));
        a.controller->setParamNormalized(ap::plugin::kLookId, 0.75);
        std::printf("-- look-ahead 3.0 ms: latency %u samples\n", a.processor->getLatencySamples());
        CHECK(a.processor->getLatencySamples() == 144);
        String128 shown{};
        a.controller->getParamStringByValue(ap::plugin::kLookId, 0.75, shown);
        CHECK(shown[0] == '3' && shown[1] == '.' && shown[2] == '0' && shown[3] == 0);
        // a value between the list's points (a host's automation) runs - and is reported - as the nearest point
        a.controller->setParamNormalized(ap::plugin::kLookId, 0.3);
        CHECK(a.processor->getLatencySamples() == static_cast<uint32>(ap::lookFrames(1.5, kRate)));
        a.close();
    }

    // 4a. an old project's look-ahead under 1.5 ms (0.5 and 1.0 were choices once) runs, shows and is reported as
    // 1.5 ms from the start - not first 66 frames and 72 after the next edit (outside review)
    {
        Instance a;
        CHECK(a.open(module, &host));
        MemoryStream m;
        {
            IBStreamer s(&m, kLittleEndian);
            s.writeInt32(2);
            s.writeStr8("");
            s.writeStr8("");
            s.writeDouble(1.0);
            s.writeInt8(0);
            s.writeInt32(0); // no values: the defaults
            s.writeInt32(4);
        }
        m.seek(0, IBStream::kIBSeekSet, nullptr);
        CHECK(a.component->setState(&m) == kResultOk);
        const uint32 before = a.processor->getLatencySamples();
        CHECK(before == static_cast<uint32>(ap::lookFrames(1.5, kRate)));
        CHECK(a.controller->getParamNormalized(ap::plugin::kLookId) == 0.25); // 1.5 ms in the list
        a.controller->setParamNormalized(ap::plugin::kBypassId, 1.0); // another edit
        CHECK(a.processor->getLatencySamples() == before);
        std::printf("-- an old project's 1.0 ms: %u samples (1.5 ms) from the start\n", before);
        a.close();
    }

    // 4b. the oversampling (2x, 4x, 8x; 4x by default) is a parameter and goes with the project
    {
        Instance a, b;
        CHECK(a.open(module, &host));
        CHECK(a.controller->getParamNormalized(ap::plugin::kOversamplingId) == 0.5); // 4x
        a.controller->setParamNormalized(ap::plugin::kOversamplingId, 1.0);       // 8x
        MemoryStream m;
        CHECK(a.component->getState(&m) == kResultOk);
        m.seek(0, IBStream::kIBSeekSet, nullptr);
        CHECK(b.open(module, &host));
        CHECK(b.component->setState(&m) == kResultOk);
        const double loaded = b.controller->getParamNormalized(ap::plugin::kOversamplingId);
        std::printf("-- oversampling 8x stored and loaded: %s\n", loaded == 1.0 ? "8x" : "NOT 8x");
        CHECK(loaded == 1.0);
        a.close();
        b.close();
    }

    // 4c. mono (off by default) is a parameter and goes with the project (state version 3)
    {
        Instance a, b;
        CHECK(a.open(module, &host));
        CHECK(a.controller->getParamNormalized(ap::plugin::kMonoId) == 0.0);
        a.controller->setParamNormalized(ap::plugin::kMonoId, 1.0);
        MemoryStream m;
        CHECK(a.component->getState(&m) == kResultOk);
        m.seek(0, IBStream::kIBSeekSet, nullptr);
        CHECK(b.open(module, &host));
        CHECK(b.component->setState(&m) == kResultOk);
        const double loaded = b.controller->getParamNormalized(ap::plugin::kMonoId);
        std::printf("-- mono stored and loaded: %s\n", loaded == 1.0 ? "mono" : "NOT mono");
        CHECK(loaded == 1.0);
        a.close();
        b.close();
    }

    // 4d. a project loaded after its preset changed in the settings file (client, 2026-10-29: "noise and stereo not
    // taken over after the start"): the values the project left as the preset had them take the preset's values now,
    // a value changed in the project stays. On a scratch settings file (DJAP_PLUGIN_INI), never the user's.
    {
        const std::filesystem::path ini = std::filesystem::temp_directory_path() / "djap_plugin_test.ini";
        std::error_code ec;
        std::filesystem::remove(ini, ec);
        SetEnvironmentVariableW(L"DJAP_PLUGIN_INI", ini.wstring().c_str());
        Instance a, b;
        CHECK(a.open(module, &host)); // no file yet: the shipped lists, Medium with Neutral
        const double ceiling = static_cast<double>(ap::paramPositionOf(ap::pLimCeiling, -2.0)) /
                               ap::paramPositions(ap::pLimCeiling);
        a.controller->setParamNormalized(ap::plugin::paramId(ap::pLimCeiling), ceiling); // this project's own change
        MemoryStream m;
        CHECK(a.component->getState(&m) == kResultOk);
        {
            std::ofstream f(ini);
            f << "[Preset Medium]\nWidthOn=1\nWidth=200\nBassWidth=0\nNoiseOn=1\n"; // the preset changed meanwhile
        }
        CHECK(b.open(module, &host));
        m.seek(0, IBStream::kIBSeekSet, nullptr);
        CHECK(b.component->setState(&m) == kResultOk);
        const State back = b.state();
        std::printf("-- the preset changed after the project was saved: width %s %.0f %%, bass width %.0f %%, noise %s, "
                    "the project's own ceiling %.1f dBTP\n",
                    back.values[ap::pWidthOn] >= 0.5 ? "on" : "OFF", back.values[ap::pWidth], back.values[ap::pBassWidth],
                    back.values[ap::pNoiseOn] >= 0.5 ? "on" : "OFF", back.values[ap::pLimCeiling]);
        CHECK(back.preset == "Medium");
        CHECK(back.values[ap::pWidthOn] == 1.0 && back.values[ap::pWidth] == 200.0 && back.values[ap::pBassWidth] == 0.0);
        CHECK(back.values[ap::pNoiseOn] == 1.0);
        CHECK(back.values[ap::pLimCeiling] == -2.0);
        // a project of a version before the base (state version 3, as 0.1.245 wrote it): the named preset's values now
        {
            MemoryStream old;
            IBStreamer w(&old, kLittleEndian);
            w.writeInt32(3);
            w.writeStr8("Medium");
            w.writeStr8("Neutral");
            w.writeDouble(1.5);
            w.writeInt8(0);
            w.writeInt32(2);
            w.writeStr8(ap::paramInfo(ap::pWidthOn).key);
            w.writeDouble(0.0);
            w.writeStr8(ap::paramInfo(ap::pLimCeiling).key);
            w.writeDouble(-2.0);
            w.writeInt32(4); // oversampling
            w.writeInt8(0);  // mono
            Instance c;
            CHECK(c.open(module, &host));
            old.seek(0, IBStream::kIBSeekSet, nullptr);
            CHECK(c.component->setState(&old) == kResultOk);
            const State o = c.state();
            std::printf("-- a version-3 project: width %s, ceiling %.1f dBTP (Medium's now)\n",
                        o.values[ap::pWidthOn] >= 0.5 ? "on" : "OFF", o.values[ap::pLimCeiling]);
            CHECK(o.values[ap::pWidthOn] == 1.0 && o.values[ap::pLimCeiling] == -1.0);
            c.close();
        }
        // 4e. a host that keeps a set's parameter values and restores them one by one after initialize - only the
        // ones that differ - and never calls setState (the client's DJ program, seen in a log 2026-10-29)
        {
            using Stored = std::vector<std::pair<ParamID, ParamValue>>;
            auto keep = [](Instance& i) { // what such a host keeps for its set
                Stored s;
                for (int32 k = 0; k < i.controller->getParameterCount(); ++k) {
                    ParameterInfo info{};
                    i.controller->getParameterInfo(k, info);
                    s.push_back({info.id, i.controller->getParamNormalized(info.id)});
                }
                return s;
            };
            auto restore = [](Instance& i, const Stored& s) {
                for (const auto& [id, v] : s)
                    if (i.controller->getParamNormalized(id) != v) i.controller->setParamNormalized(id, v);
            };
            auto writeIni = [&ini](bool noise) {
                std::ofstream f(ini);
                f << "[Preset Medium]\nWidthOn=1\nWidth=200\nBassWidth=0\nNoiseOn=" << (noise ? 1 : 0) << "\n";
            };
            const double ceiling2 = static_cast<double>(ap::paramPositionOf(ap::pLimCeiling, -2.0)) /
                                    ap::paramPositions(ap::pLimCeiling);
            // a set saved with Medium (Noise off) and a ceiling of its own; then Noise switched on in the preset
            writeIni(false);
            Instance s1;
            CHECK(s1.open(module, &host));
            s1.start(); // a running plug-in: its stamp is set
            s1.controller->setParamNormalized(ap::plugin::paramId(ap::pLimCeiling), ceiling2);
            const Stored set1 = keep(s1);
            s1.close();
            // a set kept before 0.1.253: no stamp in it - and the preset changed since (Noise on): the preset's values
            {
                Stored old;
                for (const auto& e : set1)
                    if (e.first != ap::plugin::kStampId) old.push_back(e);
                writeIni(true);
                Instance o;
                CHECK(o.open(module, &host));
                restore(o, old);
                o.start();
                const State r = o.state();
                std::printf("-- a host restoring a set of before the stamp: noise %s (the preset's now)\n",
                            r.values[ap::pNoiseOn] >= 0.5 ? "on" : "OFF");
                CHECK(r.values[ap::pNoiseOn] == 1.0 && r.values[ap::pWidthOn] == 1.0);
                o.close();
            }
            writeIni(true);
            Instance s2;
            CHECK(s2.open(module, &host));
            restore(s2, set1);
            s2.start();
            const State r2 = s2.state();
            std::printf("-- a host restoring parameters, the preset changed since: noise %s, width %s (the preset's now), "
                        "ceiling %.1f\n",
                        r2.values[ap::pNoiseOn] >= 0.5 ? "on" : "OFF", r2.values[ap::pWidthOn] >= 0.5 ? "on" : "OFF",
                        r2.values[ap::pLimCeiling]);
            CHECK(r2.values[ap::pNoiseOn] == 1.0 && r2.values[ap::pWidthOn] == 1.0);
            // the same set saved again (now with the new preset) and a ceiling of its own: the preset unchanged, the
            // set's own value stays
            s2.controller->setParamNormalized(ap::plugin::paramId(ap::pLimCeiling), ceiling2);
            const Stored set2 = keep(s2);
            s2.close();
            Instance s3;
            CHECK(s3.open(module, &host));
            restore(s3, set2);
            s3.start();
            const State r3 = s3.state();
            std::printf("-- the preset unchanged: noise %s, the set's own ceiling %.1f dBTP\n",
                        r3.values[ap::pNoiseOn] >= 0.5 ? "on" : "OFF", r3.values[ap::pLimCeiling]);
            CHECK(r3.values[ap::pNoiseOn] == 1.0 && r3.values[ap::pLimCeiling] == -2.0);
            s3.close();
        }
        SetEnvironmentVariableW(L"DJAP_PLUGIN_INI", nullptr);
        std::filesystem::remove(ini, ec);
        a.close();
        b.close();
    }

    // 4f. a host that keeps no plug-in state and sets every parameter back to its default at its start (the client's
    // DJ program, a load log of 2026-10-30): each instance keeps its own choices in the settings file - the first one
    // open as 1, the second as 2 - and takes them at its next start. The choices are made in the plug-in's window.
    {
        const std::filesystem::path ini = std::filesystem::temp_directory_path() / "djap_plugin_test_slots.ini";
        std::error_code ec;
        std::filesystem::remove(ini, ec);
        SetEnvironmentVariableW(L"DJAP_PLUGIN_INI", ini.wstring().c_str());
        SetEnvironmentVariableW(L"DJAP_NO_UPDATE_CHECK", L"1");
        // an item of the window's list chosen as a click chooses it (CBN_SELENDOK to the window)
        auto pick = [](Instance& i, int listId, const wchar_t* name) {
            HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, nullptr, nullptr);
            IPtr<IPlugView> view = owned(i.controller->createView(ViewType::kEditor));
            bool ok = false;
            if (parent && view && view->attached(parent, kPlatformTypeHWND) == kResultOk) {
                HWND w = FindWindowExW(parent, nullptr, nullptr, nullptr);
                HWND list = w ? GetDlgItem(w, listId) : nullptr;
                const LRESULT k = list ? SendMessageW(list, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                      reinterpret_cast<LPARAM>(name))
                                       : CB_ERR;
                if (k != CB_ERR) {
                    SendMessageW(list, CB_SETCURSEL, static_cast<WPARAM>(k), 0);
                    SendMessageW(w, WM_COMMAND, MAKEWPARAM(listId, CBN_SELENDOK), reinterpret_cast<LPARAM>(list));
                    ok = true;
                }
                view->removed();
            }
            if (parent) DestroyWindow(parent);
            return ok;
        };
        auto fileText = [&ini] {
            std::ifstream f(ini);
            std::stringstream t;
            t << f.rdbuf();
            return t.str();
        };
        {
            Instance a, b; // instances 1 and 2 (deck 1 and deck 2)
            CHECK(a.open(module, &host) && b.open(module, &host));
            CHECK(pick(a, 1, L"Loud"));
            CHECK(pick(b, 2, L"Warm"));
            a.close();
            b.close();
        }
        const std::string text = fileText();
        CHECK(text.find("Loudness1=Loud\n") != std::string::npos && text.find("Colour2=Warm\n") != std::string::npos);
        {
            Instance a, b;
            CHECK(a.open(module, &host) && b.open(module, &host));
            const State sa = a.state(), sb = b.state();
            std::printf("-- the next start: instance 1 %s with %s, instance 2 %s with %s\n", sa.preset.c_str(),
                        sa.colour.c_str(), sb.preset.c_str(), sb.colour.c_str());
            CHECK(sa.preset == "Loud" && sa.colour == "Neutral");
            CHECK(sb.preset == "Medium" && sb.colour == "Warm");
            // the host's reset: every parameter to its default, then activated - the kept preset's values again
            int reset = 0;
            for (int32 k = 0; k < a.controller->getParameterCount(); ++k) {
                ParameterInfo info{};
                a.controller->getParameterInfo(k, info);
                if (a.controller->getParamNormalized(info.id) == info.defaultNormalizedValue) continue;
                a.controller->setParamNormalized(info.id, info.defaultNormalizedValue);
                ++reset;
            }
            a.start();
            const State r = a.state();
            std::printf("-- the host set %d parameters back to their defaults: %s with %s, %s\n", reset,
                        r.preset.c_str(), r.colour.c_str(), r.values == sa.values ? "Loud's values" : "OTHER values");
            CHECK(reset > 0 && r.preset == "Loud" && r.values == sa.values);
            a.close();
            b.close();
        }
        // look-ahead, oversampling and mono (the plug-in's Setup window) kept the same way - as those parameters'
        // defaults too, so the host's reset leaves them
        {
            std::ofstream f(ini);
            f << "[Plugin]\nLoudness1=Very Loud\nColour1=Bright\nLookAhead1=3\nOversampling1=8\nMono1=1\n";
        }
        {
            Instance a;
            CHECK(a.open(module, &host));
            const State sa = a.state();
            ParameterInfo mono{};
            for (int32 k = 0; k < a.controller->getParameterCount(); ++k) {
                ParameterInfo info{};
                a.controller->getParameterInfo(k, info);
                if (info.id == ap::plugin::kMonoId) mono = info;
            }
            std::printf("-- kept: %s with %s, look-ahead %.1f ms, oversampling %.0f, mono %.0f (default %.0f)\n",
                        sa.preset.c_str(), sa.colour.c_str(), sa.look,
                        a.controller->getParamNormalized(ap::plugin::kOversamplingId) * 2,
                        a.controller->getParamNormalized(ap::plugin::kMonoId), mono.defaultNormalizedValue);
            CHECK(sa.preset == "Very Loud" && sa.colour == "Bright" && sa.look == 3.0);
            CHECK(a.controller->getParamNormalized(ap::plugin::kOversamplingId) == 1.0);
            CHECK(a.controller->getParamNormalized(ap::plugin::kMonoId) == 1.0 && mono.defaultNormalizedValue == 1.0);
            CHECK(a.processor->getLatencySamples() == static_cast<uint32>(std::lround(3.0 * kRate / 1000.0)));
            a.close();
        }
        SetEnvironmentVariableW(L"DJAP_NO_UPDATE_CHECK", nullptr);
        SetEnvironmentVariableW(L"DJAP_PLUGIN_INI", nullptr);
        std::filesystem::remove(ini, ec);
    }

    // 5. an input the host marks silent may hold anything: it is processed as silence
    {
        Instance a;
        CHECK(a.open(module, &host));
        a.start();
        const int n = 4800;
        std::vector<float> junk(n, 1e30f), ol(n), orr(n);
        junk[7] = std::numeric_limits<float>::quiet_NaN();
        for (int pos = 0; pos < n; pos += kMaxBlock) { // the host's blocks are at most kMaxBlock
            a.data.inputs[0].silenceFlags = 3;
            a.run(&junk[static_cast<size_t>(pos)], &junk[static_cast<size_t>(pos)], &ol[static_cast<size_t>(pos)],
                  &orr[static_cast<size_t>(pos)], std::min(kMaxBlock, n - pos), nullptr);
        }
        a.data.inputs[0].silenceFlags = 0;
        int loud = 0;
        for (int i = 0; i < n; ++i) loud += !(ol[static_cast<size_t>(i)] == 0.0f && orr[static_cast<size_t>(i)] == 0.0f);
        std::printf("-- an input marked silent (holding junk): %d of %d output samples not silent\n", loud, n);
        CHECK(loud == 0);
        a.close();
    }

    // 6. several plug-ins at once (one per deck in the DJ program, next to the program itself, which runs once): four
    // instances of the one loaded module, each with a target of its own, processed on four threads at the same time -
    // each bit-exact with its own deck, none hears another (no shared state between instances)
    {
        constexpr int kInstances = 4;
        const double targets[kInstances] = {-18.0, -14.0, -11.0, -8.0};
        Instance inst[kInstances];
        State st[kInstances];
        for (int i = 0; i < kInstances; ++i) {
            CHECK(inst[i].open(module, &host));
            st[i] = inst[i].state();
            st[i].values[ap::pTarget] = targets[i];
            inst[i].start();
        }
        const int n = static_cast<int>(kRate * 4);
        std::vector<float> outL[kInstances], outR[kInstances], refL[kInstances], refR[kInstances];
        std::vector<std::thread> threads;
        for (int i = 0; i < kInstances; ++i) {
            outL[i].assign(static_cast<size_t>(n), 0.0f);
            outR[i].assign(static_cast<size_t>(n), 0.0f);
            refL[i].assign(inL.begin(), inL.begin() + n);
            refR[i].assign(inR.begin(), inR.begin() + n);
            threads.emplace_back([&, i] {
                ap::DeckProcessor ref;
                ref.prepare(kRate);
                configure(ref, st[i]);
                ref.startFadeIn();
                for (int pos = 0, b = 0; pos < n;) {
                    const int len = std::min(blocks[(b++ + i) % 5], n - pos); // block sizes differ per instance
                    ParameterChanges changes(1);
                    IParameterChanges* withChange = nullptr;
                    if (pos == 0) { // the instance's own target, as the host's automation at the start
                        int32 qi = 0, pi = 0;
                        IParamValueQueue* q = changes.addParameterData(ap::plugin::paramId(ap::pTarget), qi);
                        q->addPoint(0, static_cast<double>(ap::paramPositionOf(ap::pTarget, targets[i])) /
                                           ap::paramPositions(ap::pTarget), pi);
                        withChange = &changes;
                    }
                    inst[i].run(&inL[static_cast<size_t>(pos)], &inR[static_cast<size_t>(pos)],
                                &outL[i][static_cast<size_t>(pos)], &outR[i][static_cast<size_t>(pos)], len, withChange);
                    ref.process(&refL[i][static_cast<size_t>(pos)], &refR[i][static_cast<size_t>(pos)], len);
                    pos += len;
                }
            });
        }
        for (std::thread& t : threads) t.join();
        int diff = 0, alike = 0;
        for (int i = 0; i < kInstances; ++i) {
            diff += differences(outL[i], refL[i]) + differences(outR[i], refR[i]);
            if (i > 0) alike += differences(outL[i], outL[0]) == 0; // a target of its own is heard
        }
        std::printf("-- %d plug-ins at once on %d threads, targets -18 / -14 / -11 / -8 LUFS: %d samples differ from "
                    "their own decks, %d outputs equal to another's\n", kInstances, kInstances, diff, alike);
        CHECK(diff == 0);
        CHECK(alike == 0);
        for (Instance& x : inst) x.close();
    }
    return test::finish("plugin_test");
}
