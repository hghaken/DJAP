// SPDX-License-Identifier: GPL-3.0-or-later
// The last hour of the audio's health, a point per second, for the Health window's graphs (client, 2026-11-05: "the
// health part in a window of its own, with line graphs of the health values"). Fed on the GUI thread, also while the
// window is closed, so it shows what happened before it was opened. No Windows code.
#pragma once

#include <vector>

namespace ap {

// What the counters say now (EngineHealth, the ASIO driver's counts, each deck's AsyncInputState).
struct HealthReading {
    bool running = false;
    double peakPercent = 0; // the heaviest period since the last reading, in % of its time
    double busySeconds = 0; // processing time so far
    long long overHalf = 0, late = 0, refused = 0, resyncs = 0, overloads = 0;
    struct Input {
        bool on = false; // the deck reads through a drift buffer
        double driftPpm = 0, fillMs = 0, targetMs = 0;
        long long underruns = 0, overruns = 0;
    } input[2];
};

// One second. The counts are what happened in it: the counters only ever go up, except at Reset counters and a new
// stream - such a step counts as nothing, so a reset leaves no spike in the graph.
struct HealthPoint {
    bool running = false;
    float peakPercent = 0, averagePercent = 0;
    float overHalf = 0, late = 0, refused = 0, resyncs = 0, overloads = 0, underruns = 0, overruns = 0;
    bool inputOn[2] = {false, false};
    float driftPpm[2] = {0, 0}, fillMs[2] = {0, 0}, targetMs[2] = {0, 0};
};

class HealthHistory {
public:
    static constexpr int kSeconds = 3600;

    HealthHistory();
    // `seconds`: the time since the last reading (the average load is the processing time's growth over it).
    void add(const HealthReading& r, double seconds);
    int size() const { return count_; }
    const HealthPoint& point(int age) const; // age 0: the newest; 0 <= age < size()
    long long added() const { return added_; } // points ever added: the graph repaints when it grows

private:
    std::vector<HealthPoint> points_;
    int next_ = 0, count_ = 0;
    long long added_ = 0;
    HealthReading previous_;
};

} // namespace ap
