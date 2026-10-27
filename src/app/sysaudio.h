// Audio through the Windows APIs other than ASIO - WASAPI (shared or exclusive), DirectSound, MME - with miniaudio
// (third_party/miniaudio). Each deck is one duplex stream: its input device -> the deck -> its output device, on the
// stream's own thread; a deck without devices does not run. With drift correction (the default; the input and the
// output device may run on different clocks) a deck is a capture stream into its AsyncInput and a playback stream
// that reads it, as with an ASIO output (docs/design.md 6.1): a buffer of about 10-20 ms instead of a glitch whenever
// the two clocks have drifted a period apart. Devices are opened and closed on a helper thread (a
// WASAPI open can fail on a thread in a single-threaded COM apartment, like the GUI thread; handbook 12).
// miniaudio converts the sample rate and channel count when a device differs; the decks run at the rate asked for.
// With an ASIO output (startCapture), a deck's input stream only captures: it writes into the deck's AsyncInput,
// which the ASIO buffer switch reads with drift correction (docs/design.md section 6.1).
#pragma once

#include "engine.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ap {

enum class AudioApi { Asio, Wasapi, DirectSound, Mme };
const char* apiName(AudioApi api);         // "ASIO", "WASAPI", "DirectSound", "MME" (as stored in the settings)
const char* apiLabel(AudioApi api);        // shown in the Audio window
bool apiFromName(const std::string& name, AudioApi& api);

struct SysDevice {
    std::string name; // as the API reports it (UTF-8)
    bool isDefault = false;
};

// One deck's devices (names; empty input and output = the deck does not run).
struct SysRoute {
    std::string input, output;
};

struct SysStreamInfo {
    bool running = false;
    std::string input, output;    // device names in use (output empty: a capture-only stream)
    double deviceRate = 0;        // the output device's own rate (capture-only: the input device's)
    unsigned periodFrames = 0;    // output device period (at its own rate; capture-only: the input's)
    unsigned periods = 0;
    double latencyMs = 0;         // estimate: input period + output buffer (capture-only: input period); with
                                  // drift correction the AsyncInput's buffer comes on top
    bool driftCorrected = false;  // a capture and a playback stream with the deck's AsyncInput between them
    bool sameDevice = false;      // drift correction asked for, but input and output are one device (one clock):
                                  // one duplex stream (WASAPI only can tell)
};

class SysAudio {
public:
    SysAudio();
    ~SysAudio();
    SysAudio(const SysAudio&) = delete;
    SysAudio& operator=(const SysAudio&) = delete;

    // The input and output devices of an API (opens nothing).
    static bool devices(AudioApi api, std::vector<SysDevice>& inputs, std::vector<SysDevice>& outputs,
                        std::string& error);

    // GUI thread. Prepares the engine at `sampleRate`, calls beforeStart(rate) (publish the decks' configurations),
    // opens and starts one duplex stream per deck that has devices. periodFrames 0 = the API's low-latency default.
    // drift[d] (may be null; reset by the caller): deck d runs drift-corrected through it instead.
    bool start(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames, bool exclusive,
               AsyncInput* const drift[2], AudioEngine& engine, const std::function<void(double)>& beforeStart,
               std::string& error);
    // GUI thread, with the ASIO stream already running at `sampleRate`: one capture-only stream per deck that has a
    // device name, writing into targets[d] (reset by the caller; the deck reads it in the ASIO buffer switch).
    bool startCapture(AudioApi api, const std::string devices[2], double sampleRate, unsigned periodFrames,
                      bool exclusive, AsyncInput* const targets[2], std::string& error);
    void stop();
    bool running() const;
    bool capturing() const { return capturing_; } // the streams running are capture-only (startCapture)
    AudioApi api() const { return api_; }
    SysStreamInfo info(int deck) const;
    // Both decks' periods together (blocks summed, heaviest = the larger).
    EngineHealth health() const;
    void resetHealth();
    // A stream stopped by itself (device unplugged, taken by another program in exclusive mode). Cleared by stop().
    bool failed() const;
    // Test aid (sysaudio_test): every API is miniaudio's null backend - devices that make no sound, on a timer.
    static void useNullBackend(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AudioApi api_ = AudioApi::Wasapi;
    bool capturing_ = false;
};

} // namespace ap
