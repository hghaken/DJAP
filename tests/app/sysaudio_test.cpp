// The streams of WASAPI / DirectSound / MME (sysaudio.*) on miniaudio's null backend: devices that make no sound and
// run on a timer, so nothing is heard and no real device is opened. Checks the route a deck takes - one duplex
// stream, or with drift correction a capture stream into the deck's AsyncInput and a playback stream that reads it -
// starts, runs the deck, fills the buffer, counts its periods and stops cleanly.
#include "check.h"
#include "settingslock.h"
#include "sysaudio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ap;

namespace {

void run(bool drift, const SysRoute& route) {
    std::printf("-- %s\n", drift ? "drift correction: capture stream -> AsyncInput -> playback stream" : "one duplex stream");
    AudioEngine engine;
    AsyncInput async;
    async.reset(48000.0);
    AsyncInput* targets[2] = {drift ? &async : nullptr, nullptr};
    const SysRoute routes[2] = {route, SysRoute()};
    SysAudio sys;
    std::string err;
    bool published = false;
    const bool ok = sys.start(AudioApi::Wasapi, routes, 48000.0, 480, false, targets, true, engine,
                              [&](double) { published = true; }, err);
    CHECK(ok);
    CHECK(published);
    if (!ok) {
        std::printf("   start failed: %s\n", err.c_str());
        return;
    }
    const SysStreamInfo info = sys.info(0);
    CHECK(info.running && info.driftCorrected == drift);
    CHECK(!sys.info(1).running); // a deck without devices does not run
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const EngineHealth h = sys.health();
    const uint64_t frames = engine.deck(0).processedFrames();
    const AsyncInputState a = async.state();
    std::printf("   %lld periods, deck A processed %llu frames (%.1f s), deck B %llu; device %u x %u frames, %.1f ms\n",
                h.blocks, static_cast<unsigned long long>(frames), frames / 48000.0,
                static_cast<unsigned long long>(engine.deck(1).processedFrames()), info.periodFrames, info.periods,
                info.latencyMs);
    CHECK(h.blocks > 0);
    CHECK(frames > 48000); // more than one of the three seconds (the buffer fills first)
    CHECK(engine.deck(1).processedFrames() == 0);
    CHECK(!sys.failed());
    if (drift) {
        std::printf("   AsyncInput: %s, buffer %.1f ms (fill %.1f), drift %+.1f ppm, %lld input blocks, underruns %lld, "
                    "overruns %lld\n",
                    a.active ? "reading" : "filling up", a.targetMs, a.fillMs, a.driftPpm, a.writes, a.underruns,
                    a.overruns);
        CHECK(a.active);
        CHECK(a.writes > 0);
        CHECK(a.overruns == 0);
        CHECK(a.targetMs > 0.0 && a.targetMs < 60.0);
    } else {
        CHECK(a.writes == 0); // the AsyncInput is not used
    }
    sys.stop();
    CHECK(!sys.running());
    const uint64_t after = engine.deck(0).processedFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(engine.deck(0).processedFrames() == after); // stopped: no callback any more
}

// A start that cannot open a device fails with a reason, leaves nothing running (a stream opened before the failing
// one is closed again) and the next start works (fourth outside review: these paths were untested).
void failures(const SysRoute& good) {
    std::printf("-- a device that is not there: the start fails, nothing runs, the next start works\n");
    for (bool drift : {false, true})
        for (int which = 0; which < 2; ++which) {
            AudioEngine engine;
            AsyncInput async;
            async.reset(48000.0);
            AsyncInput* targets[2] = {drift ? &async : nullptr, nullptr};
            SysRoute bad = good;
            (which == 0 ? bad.input : bad.output) = "no such device";
            const SysRoute routes[2] = {good, bad}; // deck A opens first, deck B fails
            SysAudio sys;
            std::string err;
            const bool ok = sys.start(AudioApi::Wasapi, routes, 48000.0, 480, false, targets, true, engine, nullptr, err);
            std::printf("   %s, %s missing: %s\n", drift ? "drift correction" : "duplex", which == 0 ? "input" : "output",
                        err.c_str());
            CHECK(!ok && !err.empty());
            CHECK(!sys.running() && !sys.info(0).running && !sys.info(1).running);
            const SysRoute fine[2] = {good, SysRoute()};
            CHECK(sys.start(AudioApi::Wasapi, fine, 48000.0, 480, false, targets, true, engine, nullptr, err));
            CHECK(sys.running() && sys.info(0).running);
            sys.stop();
            CHECK(!sys.running());
        }
}

// Drift correction "auto" leaves an input and output of one device one duplex stream; "always" corrects them too
// (sixth outside review: which way the choice went was untested - the null devices never count as one device).
void oneDevice(const SysRoute& route) {
    std::printf("-- one device: auto -> one duplex stream, always -> drift-corrected\n");
    SysAudio::assumeOneDevice(true);
    for (const bool autoMode : {true, false}) {
        AudioEngine engine;
        AsyncInput async;
        async.reset(48000.0);
        AsyncInput* targets[2] = {&async, nullptr};
        const SysRoute routes[2] = {route, SysRoute()};
        SysAudio sys;
        std::string err;
        CHECK(sys.start(AudioApi::Wasapi, routes, 48000.0, 480, false, targets, autoMode, engine, nullptr, err));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const SysStreamInfo info = sys.info(0);
        const long long writes = async.state().writes;
        std::printf("   %s: %s, AsyncInput blocks %lld\n", autoMode ? "auto" : "always",
                    info.driftCorrected ? "drift-corrected" : (info.sameDevice ? "one duplex stream (one device)" : "duplex"),
                    writes);
        CHECK(info.running);
        CHECK(info.driftCorrected == !autoMode && info.sameDevice == autoMode);
        CHECK(autoMode ? writes == 0 : writes > 0);
        sys.stop();
    }
    SysAudio::assumeOneDevice(false);
}

// Capture streams only (the decks' inputs when the output is ASIO): the period asked for reaches the device - the
// default, or a short one (Setup's input period, seventh outside review) - and the blocks reach the AsyncInput.
void captureOnly(const std::string& input) {
    std::printf("-- capture only (ASIO output): the input period asked for\n");
    for (const unsigned asked : {0u, 64u}) {
        AsyncInput async;
        async.reset(48000.0);
        AsyncInput* targets[2] = {&async, nullptr};
        const std::string devices[2] = {input, std::string()};
        SysAudio sys;
        std::string err;
        const bool ok = sys.startCapture(AudioApi::Wasapi, devices, 48000.0, asked, false, targets, err);
        CHECK(ok);
        if (!ok) {
            std::printf("   start failed: %s\n", err.c_str());
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const SysStreamInfo info = sys.info(0);
        std::printf("   asked %u frames: the device runs %u frames (%.2f ms), %lld blocks in the AsyncInput\n", asked,
                    info.periodFrames, info.latencyMs, async.state().writes);
        CHECK(info.running && info.askedFrames == asked);
        CHECK(async.state().writes > 0);
        if (asked) CHECK(info.periodFrames == asked); // the null device takes any size
        else CHECK(info.periodFrames >= 240);         // the default: about 10 ms
        sys.stop();
        CHECK(!sys.running());
    }
}

// A requested input period put on the device's step (ninth outside review: untested - five mutants survived).
void onStep() {
    std::printf("-- a requested period on the device's step\n");
    SysAudio::EnginePeriods vac; // a virtual cable: 1.02 .. 10 ms in steps of 1 frame
    vac.known = true;
    vac.rate = 44100;
    vac.defaultFrames = 441;
    vac.stepFrames = 1;
    vac.minFrames = 45;
    vac.maxFrames = 441;
    CHECK(SysAudio::onStep(vac, 176, 44100.0) == 176); // 4 ms: as asked
    CHECK(SysAudio::onStep(vac, 30, 44100.0) == 45);   // under the shortest: the shortest
    CHECK(SysAudio::onStep(vac, 441, 44100.0) == 441);
    CHECK(SysAudio::onStep(vac, 500, 44100.0) == 0);   // over the longest: Windows' default
    CHECK(SysAudio::onStep(vac, 176, 48000.0) == 0);   // another rate in Windows: no low-latency mode
    CHECK(SysAudio::onStep(vac, 0, 44100.0) == 0);
    SysAudio::EnginePeriods hd = vac; // a step of 128 frames at 48 kHz, 128 .. 480
    hd.rate = 48000;
    hd.defaultFrames = 480;
    hd.stepFrames = 128;
    hd.minFrames = 128;
    hd.maxFrames = 480;
    CHECK(SysAudio::onStep(hd, 192, 48000.0) == 256); // 4 ms: rounded up onto the step, never under the request
    CHECK(SysAudio::onStep(hd, 384, 48000.0) == 384); // 8 ms: on the step already
    CHECK(SysAudio::onStep(hd, 100, 48000.0) == 128);
    SysAudio::EnginePeriods wide = hd; // a step of 256 at 44.1 kHz, longest over the default
    wide.rate = 44100;
    wide.defaultFrames = 441;
    wide.stepFrames = 256;
    wide.minFrames = 256;
    wide.maxFrames = 768;
    CHECK(SysAudio::onStep(wide, 353, 44100.0) == 0); // 8 ms would be 512 = 11.6 ms: longer than the default
    CHECK(SysAudio::onStep(wide, 176, 44100.0) == 256);
    SysAudio::EnginePeriods none = vac;
    none.known = false;
    CHECK(SysAudio::onStep(none, 176, 44100.0) == 0);
    none = vac;
    none.stepFrames = 0;
    CHECK(SysAudio::onStep(none, 176, 44100.0) == 0);
}

// The settings lock: a second taker waits until the first lets go (the program and a plug-in saving at once).
void settingsLock() {
    std::printf("-- the settings lock: one writer at a time\n");
    std::atomic<bool> held{false};
    std::thread first([&] {
        const SettingsLock lock;
        CHECK(lock.owned());
        held = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });
    while (!held) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto t0 = std::chrono::steady_clock::now();
    {
        const SettingsLock lock;
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   the second taker waited %.0f ms (the first held it 300 ms)\n", waited * 1000.0);
        CHECK(lock.owned());
        CHECK(waited > 0.2 && waited < 1.5);
    }
    first.join();
    const HANDLE a = createUserMutex(L"Local\\DJAP.test.lock");
    CHECK(a && GetLastError() != ERROR_ALREADY_EXISTS);
    const HANDLE b = createUserMutex(L"Local\\DJAP.test.lock");
    CHECK(b && GetLastError() == ERROR_ALREADY_EXISTS);
    if (a) CloseHandle(a);
    if (b) CloseHandle(b);
}

} // namespace

int main() {
    SysAudio::useNullBackend(true);
    std::vector<SysDevice> inputs, outputs;
    std::string err;
    CHECK(SysAudio::devices(AudioApi::Wasapi, inputs, outputs, err));
    if (inputs.empty() || outputs.empty()) {
        std::printf("FAIL no null devices: %s\n", err.c_str());
        return test::finish("sysaudio_test") + 1;
    }
    std::printf("null devices: \"%s\" -> \"%s\"\n", inputs[0].name.c_str(), outputs[0].name.c_str());
    const SysRoute route{inputs[0].name, outputs[0].name};
    run(false, route);
    run(true, route);
    failures(route);
    oneDevice(route);
    captureOnly(inputs[0].name);
    onStep();
    settingsLock();
    return test::finish("sysaudio_test");
}
