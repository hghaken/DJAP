// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery.h"

#include <algorithm>

namespace ap {

void DeviceRecovery::reset() {
    trying_ = gaveUp_ = false;
    since_ = noReturnBefore_ = 0.0;
}

DeviceRecovery::Action DeviceRecovery::tick(double now, const Facts& f) {
    if (!f.wanted || (f.running && !f.onFallback)) {
        reset();
        return Action::None;
    }
    if (f.running) // on the fallback; since_ stays: if it stops too, the time to try again is over already
        return f.chosenOutputsThere && now >= noReturnBefore_ ? Action::ReturnToChosen : Action::None;
    if (gaveUp_) return Action::None;
    if (!trying_) {
        trying_ = true;
        since_ = now;
    }
    if (now - since_ < f.retrySeconds) return Action::Retry;
    if (f.fallbackUsable) return Action::StartFallback;
    if (f.keepTrying) return Action::Retry;
    gaveUp_ = true;
    return Action::GiveUp;
}

double DeviceRecovery::secondsLeft(double now, double retrySeconds) const {
    return trying_ ? std::max(0.0, retrySeconds - (now - since_)) : 0.0;
}

} // namespace ap
