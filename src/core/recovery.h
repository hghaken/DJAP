// SPDX-License-Identifier: GPL-3.0-or-later
// What to do when the audio that should run does not (client, 2026-11-05: "a default output device I can choose: first
// try again for a certain time, 5 minutes for example, then fall back on it"): try the chosen devices again; after the
// time, play on the fallback output while a chosen output is gone; back as soon as it returns. The rule only: the main
// window asks every 5 s and does what it answers. No Windows code; tested with a simulated clock.
#pragma once

namespace ap {

class DeviceRecovery {
public:
    enum class Action {
        None,
        Retry,          // start the chosen devices
        StartFallback,  // start with the fallback output in the place of the outputs that are gone
        ReturnToChosen, // running on the fallback and the chosen output is back: start the chosen devices
        GiveUp          // the time is over and there is nothing else to try: stop trying, say so
    };
    struct Facts {
        bool wanted = false;             // the audio should run (the last Start; not stopped by the user)
        bool running = false;
        bool onFallback = false;         // what runs plays on another output than the chosen one
        bool chosenOutputsThere = true;  // every chosen output is among the devices now
        bool fallbackUsable = false;     // a fallback is chosen, is there, and a chosen output is gone
        bool keepTrying = true;          // no fallback and the time is over: go on trying (false: ASIO - each try
                                         // opens a driver, which may take the interface from another program)
        double retrySeconds = 300.0;
    };
    // After this long a return that failed is tried again (a device can be listed before it can be opened).
    static constexpr double kReturnPause = 30.0;

    Action tick(double now, const Facts& f);
    // The start that ReturnToChosen asked for failed: the fallback goes on (started again by the caller).
    void returnFailed(double now) { noReturnBefore_ = now + kReturnPause; }
    // Of the time to try again: what is left (0: over, or not trying).
    double secondsLeft(double now, double retrySeconds) const;
    bool trying() const { return trying_ && !gaveUp_; } // the audio should run and does not, or runs on the fallback

private:
    void reset();
    bool trying_ = false, gaveUp_ = false;
    double since_ = 0.0, noReturnBefore_ = 0.0;
};

} // namespace ap
