// SPDX-License-Identifier: GPL-3.0-or-later
// What to do when the audio that should run does not (client, 2026-11-05: "a default output device I can choose: first
// try again for a certain time, 5 minutes for example, then fall back on it"; 2026-11-17: "a second fallback output",
// each with its own time to try again): try the chosen devices again; after the first time, play on fallback 1 while a
// chosen output is gone; when fallback 1 cannot play either (not there, or gone too), try again for the second time,
// then play on fallback 2; up to fallback 1 as soon as it is there, back to the chosen output as soon as that returns.
// The rule only: the main window asks every 5 s and does what it answers. No Windows code; tested with a simulated
// clock.
#pragma once

namespace ap {

class DeviceRecovery {
public:
    enum class Action {
        None,
        Retry,          // start the chosen devices
        StartFallback1, // start with fallback 1 in the place of the outputs that are gone
        StartFallback2, // ... with fallback 2
        StepUp,         // running on fallback 2 and fallback 1 is there: start with fallback 1
        ReturnToChosen, // running on a fallback and the chosen output is back: start the chosen devices
        GiveUp          // the time is over and there is nothing else to try: stop trying, say so
    };
    struct Facts {
        bool wanted = false;             // the audio should run (the last Start; not stopped by the user)
        bool running = false;
        int onFallback = 0;              // what runs plays on fallback 1 or 2 (0: on the chosen outputs, or nothing)
        bool chosenOutputsThere = true;  // every chosen output is among the devices now
        // fallback n + 1 is chosen, is there, and a chosen output is gone
        bool fallbackUsable[2] = {false, false};
        bool secondFallback = false;     // a fallback 2 is chosen (there or not): it has a time to try again of its own
        bool keepTrying = true;          // nothing to fall back on and the time is over: go on trying (false: ASIO -
                                         // each try opens a driver, which may take the interface from another program)
        // how long to try again before fallback 1, and - from the moment fallback 1 cannot play - before fallback 2
        double retrySeconds[2] = {60.0, 60.0};
        // the program's start: a chosen output that is not there goes to a fallback at once (the first that can play),
        // as a Start in the Setup window does (client, 2026-11-05) - no time to try first
        bool atStart = false;
    };
    // After this long a return or a step up that failed is tried again (a device can be listed before it can be opened).
    static constexpr double kReturnPause = 30.0;

    Action tick(double now, const Facts& f);
    // The start that ReturnToChosen or StepUp asked for failed: the fallback goes on (started again by the caller).
    void returnFailed(double now) { noReturnBefore_ = now + kReturnPause; }
    // The time left before the next fallback (0: over, or not trying) and which one that is: 1 or 2 (0: not trying).
    double secondsLeft(double now, const double retrySeconds[2], int* fallback = nullptr) const;
    bool trying() const { return trying_ && !gaveUp_; } // the audio should run and does not, or runs on a fallback

private:
    void reset();
    bool trying_ = false, gaveUp_ = false;
    double since_ = 0.0;        // the chosen devices went: the first time counts from here
    double since2_ = -1.0;      // fallback 1 could not play (the first time over): the second time counts from here
    double noReturnBefore_ = 0.0;
};

} // namespace ap
