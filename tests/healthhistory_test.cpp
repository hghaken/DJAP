// SPDX-License-Identifier: GPL-3.0-or-later
// healthhistory_test: the Health window's history - counts per second, a reset or a new stream leaves no spike, the
// average load, the drift buffer's values per deck, one hour kept.
#include "check.h"
#include "healthhistory.h"

using namespace ap;

int main() {
    {
        HealthHistory h;
        CHECK(h.size() == 0);
        HealthReading r;
        r.running = true;
        r.late = 5; // counted before this stream's first reading: not of this second
        r.busySeconds = 10.0;
        r.peakPercent = 12.5;
        h.add(r, 1.0);
        CHECK(h.size() == 1 && h.point(0).running);
        CHECK(h.point(0).late == 0.0f && h.point(0).averagePercent == 0.0f && h.point(0).peakPercent == 12.5f);

        r.late = 7;
        r.overHalf = 3;
        r.refused = 1;
        r.resyncs = 2;
        r.overloads = 4;
        r.busySeconds = 10.25;
        h.add(r, 1.0);
        const HealthPoint& p = h.point(0);
        CHECK(p.late == 2.0f && p.overHalf == 3.0f && p.refused == 1.0f && p.resyncs == 2.0f && p.overloads == 4.0f);
        CHECK(p.averagePercent == 25.0f);
        CHECK(h.point(1).late == 0.0f); // the older point stays

        // Reset counters: the counters step back - no negative count, no spike
        r.late = 0;
        r.overHalf = 0;
        h.add(r, 1.0);
        CHECK(h.point(0).late == 0.0f && h.point(0).overHalf == 0.0f);
        r.late = 1;
        h.add(r, 2.0); // a late timer: the average over the time that really passed
        CHECK(h.point(0).late == 1.0f && h.point(0).averagePercent == 0.0f);

        // stopped: a gap, and the next stream starts without a spike from its counters
        HealthReading off;
        h.add(off, 1.0);
        CHECK(!h.point(0).running);
        r.late = 50;
        h.add(r, 1.0);
        CHECK(h.point(0).running && h.point(0).late == 0.0f);
    }
    {
        // the drift buffer's values per deck; underruns and overruns of both decks together
        HealthHistory h;
        HealthReading r;
        r.running = true;
        r.input[1].on = true;
        r.input[1].driftPpm = -29.2;
        r.input[1].fillMs = 13.0;
        r.input[1].targetMs = 13.2;
        r.input[1].underruns = 4; // before the first reading
        h.add(r, 1.0);
        CHECK(!h.point(0).inputOn[0] && h.point(0).inputOn[1] && h.point(0).underruns == 0.0f);
        CHECK(h.point(0).driftPpm[1] == static_cast<float>(-29.2) && h.point(0).targetMs[1] == static_cast<float>(13.2));
        r.input[0].on = true;
        r.input[0].underruns = 9; // this deck's input just came on: not of this second
        r.input[1].underruns = 6;
        r.input[1].overruns = 1;
        h.add(r, 1.0);
        CHECK(h.point(0).underruns == 2.0f && h.point(0).overruns == 1.0f);
        r.input[0].underruns = 10;
        r.input[1].underruns = 7;
        h.add(r, 1.0);
        CHECK(h.point(0).underruns == 2.0f);
    }
    {
        // one hour kept, the newest first
        HealthHistory h;
        HealthReading r;
        r.running = true;
        for (int i = 0; i < HealthHistory::kSeconds + 10; ++i) {
            r.peakPercent = i;
            h.add(r, 1.0);
        }
        CHECK(h.size() == HealthHistory::kSeconds);
        CHECK(h.added() == HealthHistory::kSeconds + 10);
        CHECK(h.point(0).peakPercent == static_cast<float>(HealthHistory::kSeconds + 9));
        CHECK(h.point(HealthHistory::kSeconds - 1).peakPercent == 10.0f);
    }
    return test::finish("healthhistory_test");
}
