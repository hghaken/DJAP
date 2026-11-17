// SPDX-License-Identifier: GPL-3.0-or-later
// recovery_test: when an audio device goes away - try again for the chosen time, then fallback 1; when that cannot play
// either, try again for the second time, then fallback 2; up to fallback 1 and back to the chosen output when they
// return; nothing at all while the audio runs as chosen or the user stopped it. A simulated clock.
#include "check.h"
#include "recovery.h"

using namespace ap;
using Action = DeviceRecovery::Action;

namespace {

DeviceRecovery::Facts goneWith(bool fb1, bool fb2, bool second) {
    DeviceRecovery::Facts f;
    f.wanted = true;
    f.chosenOutputsThere = false;
    f.fallbackUsable[0] = fb1;
    f.fallbackUsable[1] = fb2;
    f.secondFallback = second;
    f.retrySeconds[0] = 300.0;
    f.retrySeconds[1] = 60.0;
    return f;
}

DeviceRecovery::Facts runningOn(DeviceRecovery::Facts f, int fallback) {
    f.running = true;
    f.onFallback = fallback;
    return f;
}

} // namespace

int main() {
    const DeviceRecovery::Facts gone = goneWith(true, false, false); // one fallback, there
    const double times[2] = {300.0, 60.0};

    {
        // running as chosen, or stopped by the user: nothing
        DeviceRecovery r;
        DeviceRecovery::Facts f;
        f.wanted = true;
        f.running = true;
        CHECK(r.tick(0.0, f) == Action::None);
        f.wanted = false;
        f.running = false;
        CHECK(r.tick(5.0, f) == Action::None);
    }
    {
        // the headset goes: 5 minutes of tries, then the fallback; the first and the last try
        DeviceRecovery r;
        CHECK(!r.trying());
        CHECK(r.tick(1000.0, gone) == Action::Retry);
        CHECK(r.trying());
        int which = -1;
        CHECK(r.secondsLeft(1000.0, times, &which) == 300.0 && which == 1);
        CHECK(r.tick(1299.0, gone) == Action::Retry);
        CHECK(r.secondsLeft(1299.0, times) == 1.0);
        CHECK(r.tick(1300.0, gone) == Action::StartFallback1);
        // the fallback did not start (a moment later it is asked again)
        CHECK(r.tick(1305.0, gone) == Action::StartFallback1);

        // it runs: nothing to do while the headset stays away
        DeviceRecovery::Facts on = runningOn(gone, 1);
        CHECK(r.tick(1310.0, on) == Action::None);
        CHECK(r.tick(5000.0, on) == Action::None);

        // the headset returns: back - and when that works, all is as it was
        on.chosenOutputsThere = true;
        on.fallbackUsable[0] = false;
        CHECK(r.tick(5005.0, on) == Action::ReturnToChosen);
        DeviceRecovery::Facts fine;
        fine.wanted = true;
        fine.running = true;
        CHECK(r.tick(5010.0, fine) == Action::None);
        CHECK(!r.trying());
        // the next time it goes, the whole time to try again counts anew
        CHECK(r.tick(6000.0, gone) == Action::Retry);
        CHECK(r.tick(6299.0, gone) == Action::Retry);
        CHECK(r.tick(6300.0, gone) == Action::StartFallback1);
    }
    {
        // a return that fails (listed, but cannot be opened yet): the fallback goes on, the next return after a pause
        DeviceRecovery r;
        CHECK(r.tick(0.0, gone) == Action::Retry);
        CHECK(r.tick(300.0, gone) == Action::StartFallback1);
        DeviceRecovery::Facts on = runningOn(gone, 1);
        on.chosenOutputsThere = true;
        on.fallbackUsable[0] = false;
        CHECK(r.tick(400.0, on) == Action::ReturnToChosen);
        r.returnFailed(400.0);
        CHECK(r.tick(405.0, on) == Action::None);
        CHECK(r.tick(400.0 + DeviceRecovery::kReturnPause - 1.0, on) == Action::None);
        CHECK(r.tick(400.0 + DeviceRecovery::kReturnPause, on) == Action::ReturnToChosen);
    }
    {
        // the fallback's device goes too (no fallback 2): the time is over already, so the fallback as soon as it is
        // there
        DeviceRecovery r;
        CHECK(r.tick(0.0, gone) == Action::Retry);
        CHECK(r.tick(300.0, gone) == Action::StartFallback1);
        CHECK(r.tick(305.0, runningOn(gone, 1)) == Action::None);
        CHECK(r.tick(310.0, gone) == Action::StartFallback1);
        // and while it is not there either: the chosen devices are tried
        DeviceRecovery::Facts none = gone;
        none.fallbackUsable[0] = false;
        CHECK(r.tick(315.0, none) == Action::Retry);
    }
    {
        // the program's start with the chosen output not there: the fallback at once, back when it is there; without a
        // usable fallback (none, or not there yet just after logging on) the tries as ever
        DeviceRecovery r;
        DeviceRecovery::Facts start = gone;
        start.atStart = true;
        CHECK(r.tick(0.0, start) == Action::StartFallback1);
        DeviceRecovery::Facts on = runningOn(gone, 1);
        CHECK(r.tick(5.0, on) == Action::None);
        on.chosenOutputsThere = true;
        on.fallbackUsable[0] = false;
        CHECK(r.tick(60.0, on) == Action::ReturnToChosen);
        DeviceRecovery s;
        DeviceRecovery::Facts notYet = start;
        notYet.fallbackUsable[0] = false;
        CHECK(s.tick(0.0, notYet) == Action::Retry);
        // ... fallback 1 not there but fallback 2: that one at once
        DeviceRecovery t;
        DeviceRecovery::Facts second = goneWith(false, true, true);
        second.atStart = true;
        CHECK(t.tick(0.0, second) == Action::StartFallback2);
    }
    {
        // no fallback chosen: go on trying; the chosen device comes back after an hour
        DeviceRecovery r;
        const DeviceRecovery::Facts f = goneWith(false, false, false);
        CHECK(r.tick(0.0, f) == Action::Retry);
        CHECK(r.tick(3600.0, f) == Action::Retry);
        CHECK(r.secondsLeft(3600.0, times) == 0.0);
    }
    {
        // ASIO: after the time it stops trying (each try opens a driver), once, until the user starts it again
        DeviceRecovery r;
        DeviceRecovery::Facts f = goneWith(false, false, false);
        f.keepTrying = false;
        CHECK(r.tick(0.0, f) == Action::Retry);
        CHECK(r.tick(300.0, f) == Action::GiveUp);
        CHECK(r.tick(305.0, f) == Action::None);
        CHECK(r.tick(9999.0, f) == Action::None);
        DeviceRecovery::Facts stopped = f;
        stopped.wanted = false;
        CHECK(r.tick(10000.0, stopped) == Action::None);
        CHECK(r.tick(10005.0, f) == Action::Retry);
    }
    {
        // stopped by the user while trying: over at once, and a later failure has its whole time again
        DeviceRecovery r;
        CHECK(r.tick(0.0, gone) == Action::Retry);
        DeviceRecovery::Facts stopped = gone;
        stopped.wanted = false;
        CHECK(r.tick(100.0, stopped) == Action::None);
        CHECK(r.secondsLeft(100.0, times) == 0.0);
        CHECK(r.tick(200.0, gone) == Action::Retry);
        CHECK(r.tick(499.0, gone) == Action::Retry);
        CHECK(r.tick(500.0, gone) == Action::StartFallback1);
    }

    // ---- fallback 2 (client, 2026-11-17: "a second fallback output", each with its own time to try again) ----
    {
        // the headset goes and fallback 1 is not there either: the first time, then the second one, then fallback 2
        DeviceRecovery r;
        const DeviceRecovery::Facts f = goneWith(false, true, true);
        CHECK(r.tick(0.0, f) == Action::Retry);
        CHECK(r.tick(299.0, f) == Action::Retry);
        CHECK(r.tick(300.0, f) == Action::Retry); // fallback 1 cannot play: fallback 2's minute begins
        int which = 0;
        CHECK(r.secondsLeft(300.0, times, &which) == 60.0 && which == 2);
        CHECK(r.tick(359.0, f) == Action::Retry);
        CHECK(r.secondsLeft(359.0, times) == 1.0);
        CHECK(r.tick(360.0, f) == Action::StartFallback2);
        // on fallback 2: fallback 1 comes - up to it; it cannot be opened yet: fallback 2 again, the next step after a
        // pause; then it works
        DeviceRecovery::Facts on2 = runningOn(f, 2);
        CHECK(r.tick(400.0, on2) == Action::None);
        on2.fallbackUsable[0] = true;
        CHECK(r.tick(405.0, on2) == Action::StepUp);
        r.returnFailed(405.0);
        CHECK(r.tick(410.0, on2) == Action::None);
        CHECK(r.tick(405.0 + DeviceRecovery::kReturnPause, on2) == Action::StepUp);
        // on fallback 1 now: nothing to do while the headset is away; when it comes back, back to it
        DeviceRecovery::Facts on1 = runningOn(goneWith(true, true, true), 1);
        CHECK(r.tick(500.0, on1) == Action::None);
        on1.chosenOutputsThere = true;
        CHECK(r.tick(505.0, on1) == Action::ReturnToChosen);
    }
    {
        // the chosen output comes back while on fallback 2 with fallback 1 there too: the chosen output first
        DeviceRecovery r;
        CHECK(r.tick(0.0, goneWith(false, true, true)) == Action::Retry);
        DeviceRecovery::Facts on2 = runningOn(goneWith(true, true, true), 2);
        on2.chosenOutputsThere = true;
        CHECK(r.tick(10.0, on2) == Action::ReturnToChosen);
    }
    {
        // playing on fallback 1, it goes too: the second time counts from then (the first is over), then fallback 2;
        // fallback 1 back during that time: to it at once
        DeviceRecovery r;
        const DeviceRecovery::Facts both = goneWith(true, true, true);
        CHECK(r.tick(0.0, both) == Action::Retry);
        CHECK(r.tick(300.0, both) == Action::StartFallback1);
        CHECK(r.tick(305.0, runningOn(both, 1)) == Action::None);
        const DeviceRecovery::Facts lost1 = goneWith(false, true, true);
        CHECK(r.tick(1000.0, lost1) == Action::Retry);
        CHECK(r.tick(1059.0, lost1) == Action::Retry);
        CHECK(r.tick(1060.0, lost1) == Action::StartFallback2);
        CHECK(r.tick(1065.0, runningOn(lost1, 2)) == Action::None);
        // fallback 2 goes too: both times over - whichever is there first
        const DeviceRecovery::Facts none = goneWith(false, false, true);
        CHECK(r.tick(1100.0, none) == Action::Retry);
        CHECK(r.tick(1105.0, goneWith(false, true, true)) == Action::StartFallback2);
        CHECK(r.tick(1110.0, none) == Action::Retry);
        CHECK(r.tick(1115.0, both) == Action::StartFallback1);

        DeviceRecovery s;
        CHECK(s.tick(0.0, both) == Action::Retry);
        CHECK(s.tick(300.0, both) == Action::StartFallback1);
        CHECK(s.tick(305.0, runningOn(both, 1)) == Action::None);
        CHECK(s.tick(400.0, lost1) == Action::Retry);
        CHECK(s.tick(430.0, both) == Action::StartFallback1); // back within fallback 2's minute
        CHECK(s.tick(435.0, runningOn(both, 1)) == Action::None);
        CHECK(s.tick(900.0, lost1) == Action::Retry);         // lost again: a whole minute again
        CHECK(s.tick(959.0, lost1) == Action::Retry);
        CHECK(s.tick(960.0, lost1) == Action::StartFallback2);
    }
    {
        // fallback 2 chosen but not there when its time is over: go on trying (or, with ASIO, give up)
        DeviceRecovery r;
        const DeviceRecovery::Facts f = goneWith(false, false, true);
        CHECK(r.tick(0.0, f) == Action::Retry);
        CHECK(r.tick(300.0, f) == Action::Retry);
        CHECK(r.tick(360.0, f) == Action::Retry);
        CHECK(r.secondsLeft(360.0, times) == 0.0);
        CHECK(r.tick(400.0, goneWith(false, true, true)) == Action::StartFallback2);
        DeviceRecovery s;
        DeviceRecovery::Facts a = f;
        a.keepTrying = false;
        CHECK(s.tick(0.0, a) == Action::Retry);
        CHECK(s.tick(300.0, a) == Action::Retry);
        CHECK(s.tick(360.0, a) == Action::GiveUp);
    }
    {
        // only fallback 2 chosen: it comes after both times (fallback 1 cannot play from the end of the first)
        DeviceRecovery r;
        const DeviceRecovery::Facts f = goneWith(false, true, true);
        CHECK(r.tick(0.0, f) == Action::Retry);
        CHECK(r.tick(300.0, f) == Action::Retry); // (the main window asks every 5 s)
        CHECK(r.tick(359.0, f) == Action::Retry);
        CHECK(r.tick(300.0 + 60.0, f) == Action::StartFallback2);
    }
    return test::finish("recovery_test");
}
