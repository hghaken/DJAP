// SPDX-License-Identifier: GPL-3.0-or-later
// ASIO host (docs/design.md section 6, handbook 12). The driver is a COM object, loaded, set up and called on the
// GUI thread (which has joined a single-threaded COM apartment); only the buffer switch runs on the driver's thread
// and calls AudioEngine::process. One driver at a time: the ASIO SDK keeps it in a global, so there is one AsioHost.
#pragma once

#include "engine.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace ap {

// First channel (1-based) of each deck's input and output pair. An input of 0: the deck's input does not come from
// the driver (no buffers are made for it; the engine reads it from an AsyncInput). An output of 0: the deck is not
// used (one-channel use: deck B's input and output are 0, it is not processed).
struct AsioChannels {
    int inputA = 1, inputB = 3, outputA = 1, outputB = 3;
};

struct AsioDriverEntry {
    std::string name;  // as in the registry (ANSI code page, converted to UTF-8)
    std::wstring path; // driver DLL
};

struct AsioDeviceState {
    std::string driver;
    long inputs = 0, outputs = 0;
    long minBuffer = 0, maxBuffer = 0, preferredBuffer = 0, granularity = 0;
    std::vector<double> rates;                     // of 44100 / 48000 / 88200 / 96000, the ones the driver accepts
    std::vector<std::string> inputNames, outputNames;
    // while running
    double sampleRate = 0;
    long bufferSize = 0;
    long inputLatency = 0, outputLatency = 0;      // frames, as the driver reports them
    SampleFormat formats[2 * AudioEngine::kChannels] = {}; // inputs A L, A R, B L, B R, then the outputs
};

enum class AsioEvent {
    None,
    Reset,             // the driver asked for a reset (or a new buffer size / sample rate): close and open again
    LatenciesChanged,  // read state() again
};

class AsioHost {
public:
    AsioHost();
    ~AsioHost(); // closes the driver
    AsioHost(const AsioHost&) = delete;
    AsioHost& operator=(const AsioHost&) = delete;

    // Installed drivers, from the registry (HKLM\SOFTWARE\ASIO). Opens nothing.
    static std::vector<AsioDriverEntry> drivers();

    // GUI thread. Loads and initialises the driver: this opens the device (nothing plays). `window` = main window.
    bool open(const std::string& driver, void* window, std::string& error);
    bool isOpen() const { return open_; }
    bool running() const { return running_; }
    const AsioDeviceState& state() const { return state_; }

    // GUI thread. Sets the sample rate (when the driver accepts it, else keeps its rate), creates the buffers of the
    // eight channels, prepares the engine at the rate in use, calls `beforeStart(rate)` (publish the decks'
    // configurations made for that rate), then starts. bufferSize 0 = the driver's preferred size.
    bool start(AudioEngine& engine, double sampleRate, long bufferSize, const AsioChannels& channels,
               const std::function<void(double)>& beforeStart, std::string& error);
    void stop();  // stops and disposes the buffers
    void close(); // stop, exit and unload the driver
    void controlPanel();

    // GUI thread, from a timer: requests of the driver since the last call. A reset is reported once the requests
    // have stopped for kResetQuietMs (drivers often send several).
    AsioEvent poll();
    static constexpr int kResetQuietMs = 500;
    long long resyncs() const { return resyncs_.load(std::memory_order_relaxed); }
    long long overloads() const { return overloads_.load(std::memory_order_relaxed); }

    // driver thread (called through the SDK's plain callbacks)
    void bufferSwitch(long index);
    long message(long selector, long value);
    void rateChanged() { resetRequest_.store(true, std::memory_order_release); }

private:
    bool open_ = false, running_ = false, buffers_ = false, postOutput_ = false;
    AsioDeviceState state_;
    AudioEngine* engine_ = nullptr;
    void* bufs_[2][2 * AudioEngine::kChannels] = {}; // [half][channel]
    std::atomic<bool> resetRequest_{false}, latencyChange_{false};
    std::atomic<long long> resyncs_{0}, overloads_{0};
    bool resetPending_ = false;
    std::chrono::steady_clock::time_point lastReset_{};
};

} // namespace ap
