// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery.h"

#include <algorithm>

namespace ap {

void DeviceRecovery::reset() {
    trying_ = gaveUp_ = false;
    since_ = noReturnBefore_ = 0.0;
    since2_ = -1.0;
}

DeviceRecovery::Action DeviceRecovery::tick(double now, const Facts& f) {
    if (!f.wanted || (f.running && f.onFallback == 0)) {
        reset();
        return Action::None;
    }
    if (f.running) { // on a fallback; the times stay: if it stops too, they are over already
        if (now < noReturnBefore_) return Action::None;
        if (f.chosenOutputsThere) return Action::ReturnToChosen;
        if (f.onFallback == 2 && f.fallbackUsable[0]) return Action::StepUp;
        return Action::None;
    }
    if (gaveUp_) return Action::None;
    if (!trying_) {
        trying_ = true;
        since_ = now;
        since2_ = -1.0;
    }
    if (f.atStart) { // no time to try first: the first fallback that can play
        if (f.fallbackUsable[0]) return Action::StartFallback1;
        if (f.fallbackUsable[1]) return Action::StartFallback2;
    }
    if (now - since_ < f.retrySeconds[0]) return Action::Retry;
    if (f.fallbackUsable[0]) {
        since2_ = -1.0; // (if it goes too, fallback 2's time counts from then)
        return Action::StartFallback1;
    }
    if (f.secondFallback) {
        if (since2_ < 0.0) since2_ = now;
        if (now - since2_ < f.retrySeconds[1]) return Action::Retry;
        if (f.fallbackUsable[1]) return Action::StartFallback2;
    }
    if (f.keepTrying) return Action::Retry;
    gaveUp_ = true;
    return Action::GiveUp;
}

double DeviceRecovery::secondsLeft(double now, const double retrySeconds[2], int* fallback) const {
    if (fallback) *fallback = 0;
    if (!trying_) return 0.0;
    const bool second = since2_ >= 0.0;
    if (fallback) *fallback = second ? 2 : 1;
    return second ? std::max(0.0, retrySeconds[1] - (now - since2_)) : std::max(0.0, retrySeconds[0] - (now - since_));
}

} // namespace ap
