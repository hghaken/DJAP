// SPDX-License-Identifier: GPL-3.0-or-later
// How fast Windows wakes a waiting thread (client, 2026-11-05: "a latency monitor, without endangering the performance /
// latency itself"): a thread of its own sets a high-resolution timer 1000 times a second and measures how much later
// than asked it wakes - what a DPC / ISR latency monitor's main number tells. It runs below the audio threads (above
// normal, never time-critical or MMCSS), so it can never push the audio aside; it can only wake later itself, and then
// shows that. About 1000 short wake-ups a second: well under 0.1 % of one core. It cannot say which driver is the cause.
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace ap {

class LatencyProbe {
public:
    LatencyProbe() = default;
    ~LatencyProbe() { stop(); }
    LatencyProbe(const LatencyProbe&) = delete;
    LatencyProbe& operator=(const LatencyProbe&) = delete;

    void start();
    void stop();
    bool running() const { return thread_.joinable() && available_.load(std::memory_order_acquire); }
    // The most a wake-up came late since the last call, in ms (GUI thread, once a second).
    double takePeakMs() { return peakUs_.exchange(0, std::memory_order_relaxed) / 1000.0; }

private:
    void loop();
    std::thread thread_;
    std::atomic<bool> stop_{false}, available_{true};
    std::atomic<uint32_t> peakUs_{0};
};

} // namespace ap
