// SPDX-License-Identifier: GPL-3.0-or-later
// recovery_test: when an audio device goes away - try again for the chosen time, then the fallback output, back when the
// chosen output returns; nothing at all while the audio runs as chosen or the user stopped it. A simulated clock.
#include "check.h"
#include "recovery.h"

using namespace ap;
using Action = DeviceRecovery::Action;

int main() {
    DeviceRecovery::Facts gone;
    gone.wanted = true;
    gone.chosenOutputsThere = false;
    gone.fallbackUsable = true;
    gone.retrySeconds = 300.0;

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
        CHECK(r.secondsLeft(1000.0, 300.0) == 300.0);
        CHECK(r.tick(1299.0, gone) == Action::Retry);
        CHECK(r.secondsLeft(1299.0, 300.0) == 1.0);
        CHECK(r.tick(1300.0, gone) == Action::StartFallback);
        // the fallback did not start (a moment later it is asked again)
        CHECK(r.tick(1305.0, gone) == Action::StartFallback);

        // it runs: nothing to do while the headset stays away
        DeviceRecovery::Facts on = gone;
        on.running = true;
        on.onFallback = true;
        CHECK(r.tick(1310.0, on) == Action::None);
        CHECK(r.tick(5000.0, on) == Action::None);

        // the headset returns: back - and when that works, all is as it was
        on.chosenOutputsThere = true;
        on.fallbackUsable = false;
        CHECK(r.tick(5005.0, on) == Action::ReturnToChosen);
        DeviceRecovery::Facts fine;
        fine.wanted = true;
        fine.running = true;
        CHECK(r.tick(5010.0, fine) == Action::None);
        CHECK(!r.trying());
        // the next time it goes, the whole time to try again counts anew
        CHECK(r.tick(6000.0, gone) == Action::Retry);
        CHECK(r.tick(6299.0, gone) == Action::Retry);
        CHECK(r.tick(6300.0, gone) == Action::StartFallback);
    }
    {
        // a return that fails (listed, but cannot be opened yet): the fallback goes on, the next return after a pause
        DeviceRecovery r;
        CHECK(r.tick(0.0, gone) == Action::Retry);
        CHECK(r.tick(300.0, gone) == Action::StartFallback);
        DeviceRecovery::Facts on = gone;
        on.running = true;
        on.onFallback = true;
        on.chosenOutputsThere = true;
        on.fallbackUsable = false;
        CHECK(r.tick(400.0, on) == Action::ReturnToChosen);
        r.returnFailed(400.0);
        CHECK(r.tick(405.0, on) == Action::None);
        CHECK(r.tick(400.0 + DeviceRecovery::kReturnPause - 1.0, on) == Action::None);
        CHECK(r.tick(400.0 + DeviceRecovery::kReturnPause, on) == Action::ReturnToChosen);
    }
    {
        // the fallback's device goes too: the time is over already, so the fallback as soon as it is there
        DeviceRecovery r;
        CHECK(r.tick(0.0, gone) == Action::Retry);
        CHECK(r.tick(300.0, gone) == Action::StartFallback);
        DeviceRecovery::Facts on = gone;
        on.running = true;
        on.onFallback = true;
        CHECK(r.tick(305.0, on) == Action::None);
        CHECK(r.tick(310.0, gone) == Action::StartFallback);
        // and while it is not there either: the chosen devices are tried
        DeviceRecovery::Facts none = gone;
        none.fallbackUsable = false;
        CHECK(r.tick(315.0, none) == Action::Retry);
    }
    {
        // no fallback chosen: go on trying; the chosen device comes back after an hour
        DeviceRecovery r;
        DeviceRecovery::Facts f = gone;
        f.fallbackUsable = false;
        CHECK(r.tick(0.0, f) == Action::Retry);
        CHECK(r.tick(3600.0, f) == Action::Retry);
        CHECK(r.secondsLeft(3600.0, 300.0) == 0.0);
    }
    {
        // ASIO: after the time it stops trying (each try opens a driver), once, until the user starts it again
        DeviceRecovery r;
        DeviceRecovery::Facts f = gone;
        f.fallbackUsable = false;
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
        CHECK(r.secondsLeft(100.0, 300.0) == 0.0);
        CHECK(r.tick(200.0, gone) == Action::Retry);
        CHECK(r.tick(499.0, gone) == Action::Retry);
        CHECK(r.tick(500.0, gone) == Action::StartFallback);
    }
    return test::finish("recovery_test");
}
