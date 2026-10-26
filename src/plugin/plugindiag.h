// Diagnostics of the plug-in in a host (2026-10-26: in one DJ program the plug-in processed - its meters moved - but
// its output was not heard): what the host does with each instance, in the window's status line and in a log file,
// %TEMP%\DJAP-plugin.log - the host's calls (initialize, setup, buses, activation) when they happen, and every 2 s a
// line per live instance with what its audio thread saw. The audio thread only stores numbers (atomics).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace ap::plugin {

// What process() saw (written by the audio thread, read by the window and the logger).
struct ProcessStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> changes{0};   // parameter changes the host delivered
    std::atomic<uint64_t> updates{0};   // whole states taken over from the controller (the window, setState)
    std::atomic<int32_t> frames{0}, sampleSize{-1}, mode{-1};
    std::atomic<int32_t> inputs{-1}, outputs{-1}, inChannels{-1}, outChannels{-1};
    std::atomic<int32_t> inPlace{-1};   // 1: output buffers are the input buffers, 0: separate
    std::atomic<bool> bypass{false};    // as the processing has it
    std::atomic<float> inPeak{0}, outPeak{0}; // of the last block
};

class Plugin;

// "32-bit", "64-bit", or "no audio yet" for a ProcessStats::sampleSize
inline const char* sampleSizeName(int32_t size) {
    return size == 0 ? "32-bit" : size == 1 ? "64-bit" : "no audio yet";
}

void diagLog(const char* format, ...); // any thread but the audio thread
void diagRegister(Plugin* p);          // constructor / destructor: the logger runs while an instance lives
void diagUnregister(Plugin* p);
int diagLiveInstances();

} // namespace ap::plugin
