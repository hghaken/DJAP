// Audio through the Windows APIs other than ASIO - WASAPI (shared or exclusive), DirectSound, MME - with miniaudio
// (third_party/miniaudio). Each deck is one duplex stream: its input device -> the deck -> its output device, on the
// stream's own thread; a deck without devices does not run. Devices are opened and closed on a helper thread (a
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
    double latencyMs = 0;         // estimate: input period + output buffer (capture-only: input period)
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
    bool start(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames, bool exclusive,
               AudioEngine& engine, const std::function<void(double)>& beforeStart, std::string& error);
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AudioApi api_ = AudioApi::Wasapi;
    bool capturing_ = false;
};

} // namespace ap
