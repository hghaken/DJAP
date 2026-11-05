// SPDX-License-Identifier: GPL-3.0-or-later
// latencyprobe_test: the Health window's Windows latency probe runs, measures something plausible, and stops at once.
#include "check.h"
#include "latencyprobe.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace ap;

int main() {
    LatencyProbe probe;
    CHECK(!probe.running());
    probe.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(probe.running()); // Windows 10 1803 and later: the high-resolution timer is there
    const double peak = probe.takePeakMs();
    std::printf("-- the latest wake-up of 1.5 s came %.3f ms late\n", peak);
    CHECK(peak >= 0.0 && peak < 100.0);
    const auto t0 = std::chrono::steady_clock::now();
    probe.stop();
    const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("-- stopped in %.1f ms\n", stopMs);
    CHECK(!probe.running() && stopMs < 100.0);
    probe.start(); // again
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(probe.running());
    probe.stop();
    return test::finish("latencyprobe_test");
}
