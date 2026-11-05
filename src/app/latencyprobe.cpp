// SPDX-License-Identifier: GPL-3.0-or-later
#include "latencyprobe.h"

#include <windows.h>

#include <algorithm>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace ap {

void LatencyProbe::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    available_.store(true);
    thread_ = std::thread([this] { loop(); });
}

void LatencyProbe::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void LatencyProbe::loop() {
    // a high-resolution timer (Windows 10 1803 and later) wakes on time without raising the whole system's timer
    // resolution (timeBeginPeriod would change what is measured, and cost power)
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        available_.store(false, std::memory_order_release);
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // below the audio threads
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    const double usPerTick = 1e6 / static_cast<double>(freq.QuadPart);
    while (!stop_.load(std::memory_order_acquire)) {
        LARGE_INTEGER due;
        due.QuadPart = -10000; // 1 ms from now (100 ns units)
        QueryPerformanceCounter(&t0);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) break;
        if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0) continue;
        QueryPerformanceCounter(&t1);
        const double lateUs = static_cast<double>(t1.QuadPart - t0.QuadPart) * usPerTick - 1000.0;
        if (lateUs > peakUs_.load(std::memory_order_relaxed))
            peakUs_.store(static_cast<uint32_t>(std::min(lateUs, 1e9)), std::memory_order_relaxed);
    }
    CloseHandle(timer);
}

} // namespace ap
