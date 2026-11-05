// SPDX-License-Identifier: GPL-3.0-or-later
#include "healthhistory.h"

#include <algorithm>

namespace ap {

namespace {

float step(long long now, long long before) { return static_cast<float>(std::max(0LL, now - before)); }

} // namespace

HealthHistory::HealthHistory() : points_(kSeconds) {}

void HealthHistory::add(const HealthReading& r, double seconds) {
    HealthPoint p;
    p.running = r.running;
    p.probeOn = r.probeOn;
    p.probeMs = static_cast<float>(r.probeMs);
    if (r.running) {
        p.lateMs = static_cast<float>(r.lateMs);
        p.periodMs = static_cast<float>(r.periodMs);
        // the first reading of a stream has nothing to compare with: its counts are not of this second
        const HealthReading& b = previous_.running ? previous_ : r;
        p.peakPercent = static_cast<float>(r.peakPercent);
        if (seconds > 0.0) p.averagePercent = static_cast<float>(std::max(0.0, r.busySeconds - b.busySeconds) / seconds * 100.0);
        p.overHalf = step(r.overHalf, b.overHalf);
        p.late = step(r.late, b.late);
        p.refused = step(r.refused, b.refused);
        p.resyncs = step(r.resyncs, b.resyncs);
        p.overloads = step(r.overloads, b.overloads);
        for (int d = 0; d < 2; ++d) {
            const HealthReading::Input& in = r.input[d];
            if (!in.on) continue;
            p.inputOn[d] = true;
            p.driftPpm[d] = static_cast<float>(in.driftPpm);
            p.fillMs[d] = static_cast<float>(in.fillMs);
            p.targetMs[d] = static_cast<float>(in.targetMs);
            if (b.input[d].on) {
                p.underruns += step(in.underruns, b.input[d].underruns);
                p.overruns += step(in.overruns, b.input[d].overruns);
            }
        }
    }
    points_[static_cast<size_t>(next_)] = p;
    next_ = (next_ + 1) % kSeconds;
    count_ = std::min(count_ + 1, kSeconds);
    ++added_;
    previous_ = r;
}

const HealthPoint& HealthHistory::point(int age) const {
    return points_[static_cast<size_t>((next_ - 1 - age + 2 * kSeconds) % kSeconds)];
}

} // namespace ap
