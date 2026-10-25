#include "limiter.h"

namespace ap {

void Limiter::prepare(int maxLook, int tpFactor, int tpTapsPerPhase, double kaiserBeta) {
    maxLook_ = std::max(0, maxLook);
    cap_ = maxLook_ + 2;
    dqVal_.assign(static_cast<size_t>(cap_), 1.0);
    dqIdx_.assign(static_cast<size_t>(cap_), 0);
    avgRing_.assign(static_cast<size_t>(cap_), 1.0);
    delayL_.assign(static_cast<size_t>(cap_), 0.0);
    delayR_.assign(static_cast<size_t>(cap_), 0.0);
    tpL_.configure(tpFactor, tpTapsPerPhase, kaiserBeta);
    tpR_.configure(tpFactor, tpTapsPerPhase, kaiserBeta);
    setLook(look_);
}

void Limiter::setLook(int look) {
    look_ = std::min(std::max(0, look), maxLook_);
    useTp_ = tpL_.factor() > 1 && look_ >= tpL_.delay();
    window_ = useTp_ ? look_ - tpL_.delay() + 1 : look_ + 1;
    setParams(ceilingSet_, release_);
    reset();
}

void Limiter::reset() {
    std::fill(delayL_.begin(), delayL_.end(), 0.0);
    std::fill(delayR_.begin(), delayR_.end(), 0.0);
    std::fill(avgRing_.begin(), avgRing_.end(), 1.0);
    tpL_.reset();
    tpR_.reset();
    lastReport_ = 0.0;
    sum_ = static_cast<double>(window_);
    gain_ = 1.0;
    minGain_ = 1.0;
    n_ = 0;
    resum_ = 0;
    dqHead_ = dqCount_ = 0;
    avgPos_ = 0;
    delayPos_ = 0;
}

} // namespace ap
