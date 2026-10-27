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
#include <limits>
#include <memory>
#include <string>
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
    return test::finish("plugin_test");
}
