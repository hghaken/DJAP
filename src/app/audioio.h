// The GUI's audio side: owns the engine (both decks), keeps each deck's configuration and publishes it for the
// sample rate in use, drains the decks' meter rings into loudness meters (EBU M / S / I / LRA, true peak, gain of
// each stage), and - for tests without a device - runs a WAV file through the engine in real time with the output
// thrown away (--simulate; never makes a sound). The ASIO device is opened, started and stopped from the Audio
// window (or at program start when it was running when the program closed); driver reset requests restart it.
// With ASIO, a deck's input may come from a WASAPI / DirectSound / MME device instead (drift-corrected).
#pragma once

#if AP_HAVE_ASIO
#include "asiohost.h"
#endif
#include "analysis.h"
#include "engine.h"
#include "loudness.h"
#include "monitor.h"
#include "params.h"
#include "settings.h"
#include "sysaudio.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace ap {

#if !AP_HAVE_ASIO
struct AsioChannels { // (asiohost.h when the ASIO SDK is there)
    int inputA = 1, inputB = 3, outputA = 1, outputB = 3;
};
#endif

// Inputs of another API for decks that play through ASIO (docs/design.md section 6.1).
struct SysInputs {
    AudioApi api = AudioApi::Wasapi;
    std::string device[2]; // capture device of each deck; empty = the deck's ASIO input pair
    bool exclusive = false; // WASAPI exclusive
    unsigned periodFrames = 0; // 0 = the API's low-latency default
    int periodMs = 0;          // WASAPI: the period asked for in ms, 0 = Windows' default (Settings::inputPeriodMs)
    bool any() const { return !device[0].empty() || !device[1].empty(); }
};
// The settings' choice (AsioInputs, AsioInputDeviceA / B, Exclusive); no devices when the inputs are ASIO pairs.
// With one channel (Decks=1) deck B is never part of a route.
SysInputs sysInputsFrom(const Settings& s);
AsioChannels asioChannelsFrom(const Settings& s);   // deck B's pairs 0 (unused) with one channel
void sysRoutesFrom(const Settings& s, SysRoute routes[2]); // WASAPI / DirectSound / MME devices of each deck

class AudioIO {
public:
    AudioIO();
    ~AudioIO();

    // The configuration of a deck (GUI thread). Published at once when the engine runs, else when it starts.
    void setDeck(int d, const PresetValues& values, const DeckControl& control);
    // A volume after deck d's chain and a factor on its input (DeckProcessor::setVolume); any thread, glides.
    void setVolume(int d, double output, double input) { engine_.deck(d).setVolume(output, input); }
    double sampleRate() const { return engine_.sampleRate(); }
    AudioEngine& engine() { return engine_; }

    // GUI timer (20 Hz): drains the meter rings.
    void poll();
    const DeckMeters& meters(int d) const { return monitor_[d].meters(); }
    const DeckAnalysis& analysis(int d) const { return monitor_[d].analysis(); }
    void resetMeters(int d);

    // Test aid without a device: plays `file` (2 channels = deck A, 4 = A + B) through the engine in real time,
    // in a loop, output discarded.
    bool simulate(const std::filesystem::path& file, std::string& error);
    void stopSimulation();
    bool simulating() const { return simRunning_.load(); }
    const std::filesystem::path& simulationFile() const { return simFile_; }

    // ASIO device (GUI thread). Opening loads the driver: other programs may lose the interface.
    bool openDriver(const std::string& driver, void* window, std::string& error);
    void closeDriver();
    // Starts the open driver. A deck with a device in `inputs` takes its input from that device of another API
    // (a capture stream, read with drift correction) instead of its ASIO input pair; its output stays ASIO.
    bool startDevice(double sampleRate, long bufferSize, const AsioChannels& channels, const SysInputs& inputs,
                     std::string& error);
    void stopDevice();
    bool driverOpen() const;
    // The ASIO driver that is open (the name it was opened with); a start with another one chosen opens that first.
    const std::string& driverName() const { return driver_; }
    bool deviceRunning() const; // ASIO or another API
    bool audioActive() const { return simulating() || deviceRunning(); }
    // Deck d reads its input through its AsyncInput (while the device runs): an input of another API with an ASIO
    // output, or a WASAPI / DirectSound / MME deck with drift correction.
    bool asyncInput(int d) const {
        return deviceRunning() && (engine_.asyncInput(d) != nullptr || (systemRunning() && sys_.info(d).driftCorrected));
    }
    AsyncInputState asyncState(int d) const { return async_[d].state(); }

    // WASAPI / DirectSound / MME (GUI thread): one duplex stream per deck that has devices, or with `driftCorrection`
    // (Settings::kDrift...) a capture and a playback stream with the deck's AsyncInput between them. Ends a simulation
    // and closes an open ASIO driver first.
    bool startSystem(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames, bool exclusive,
                     int driftCorrection, std::string& error);
    bool systemRunning() const { return sys_.running() && !sys_.capturing(); }
    const SysAudio& sys() const { return sys_; }
    // Health of what runs (ASIO: the engine's counters; other APIs: both decks' streams).
    EngineHealth health() const { return systemRunning() ? sys_.health() : engine_.health(); }
    void resetHealth();
    // An automatic restart (after a driver reset request) that is still being tried, or that failed; empty when all
    // is well.
    const std::string& deviceError() const { return deviceError_; }
#if AP_HAVE_ASIO
    const AsioHost& host() const { return host_; }
    void controlPanel() { host_.controlPanel(); }
#endif

private:
    void publish(int d);
    // Before a running stream stops or starts again: the decks fade out (no click) and the silence reaches the device.
    void fadeOutDecks();
    void simLoop();

    AudioEngine engine_;
    PresetValues values_[2] = {};
    DeckControl control_[2];
    bool haveDeck_[2] = {false, false};
    DeckMonitor monitor_[2];

    std::thread sim_;
    std::atomic<bool> simRunning_{false}, simStop_{false};
    std::filesystem::path simFile_;
    std::vector<float> simData_; // interleaved
    int simChannels_ = 0;

    SysAudio sys_;
    AsyncInput async_[2]; // decks with an input of another API, or drift-corrected WASAPI / DirectSound / MME decks
#if AP_HAVE_ASIO
    AsioHost host_;
#endif
    void* window_ = nullptr;
    std::string driver_, deviceError_;
    // A driver's reset request: the driver is opened (and started, when it ran) again every second for up to
    // kResetTries - a USB interface that enumerates again, or a new rate set in its control panel, is not ready at once.
    static constexpr int kResetTries = 60;
    bool resetPending_ = false, resetWasRunning_ = false, retrying_ = false;
    int resetTries_ = 0;
    std::chrono::steady_clock::time_point resetDue_{};
    double rate_ = 0;
    long buffer_ = 0;
    AsioChannels channels_;
    SysInputs inputs_;
};

} // namespace ap
