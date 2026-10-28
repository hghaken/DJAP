#include "engine.h"

#include <algorithm>
#include <chrono>

namespace ap {

AudioEngine::AudioEngine() {
    decks_[0].reset(new DeckProcessor());
    decks_[1].reset(new DeckProcessor());
}

void AudioEngine::prepare(double fs) {
    fs_ = fs;
    decks_[0]->prepare(fs);
    decks_[1]->prepare(fs);
    decks_[0]->startFadeIn(); // a device starts in the middle of the music: no step at the start
    decks_[1]->startFadeIn();
    async_[0] = async_[1] = nullptr;
    for (auto& ch : buf_) std::fill(std::begin(ch), std::end(ch), 0.0f);
}

void AudioEngine::process(const void* const* in, const SampleFormat* inFormat, void* const* out,
                          const SampleFormat* outFormat, int frames, double nowSeconds) {
    const auto t0 = std::chrono::steady_clock::now();
    DenormalGuard denormals; // also for the conversions; the driver's own mode comes back afterwards
    if (resetRequest_.exchange(false, std::memory_order_acq_rel)) {
        blocks_.store(0, std::memory_order_relaxed);
        overHalf_.store(0, std::memory_order_relaxed);
        late_.store(0, std::memory_order_relaxed);
        refused_.store(0, std::memory_order_relaxed);
        heaviestPermille_.store(0, std::memory_order_relaxed);
    }
    if (frames <= 0) return;
    if (fs_ <= 0.0) {
        for (int c = 0; c < kChannels; ++c)
            if (out[c]) silence(outFormat[c], out[c], 0, frames);
        refused_.store(refused_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }
    if (async_[0] || async_[1]) {
        const double now =
            nowSeconds >= 0.0 ? nowSeconds : std::chrono::duration<double>(t0.time_since_epoch()).count();
        for (AsyncInput* a : async_)
            if (a) a->beginPeriod(frames, now);
    }
    // a deck with no input and no output (one-channel use: deck B) is not processed at all
    bool active[2];
    for (int d = 0; d < 2; ++d) active[d] = async_[d] || in[2 * d] || in[2 * d + 1] || out[2 * d] || out[2 * d + 1];
    for (int pos = 0; pos < frames; pos += kChunk) {
        const int n = std::min(kChunk, frames - pos);
        const std::size_t off = static_cast<std::size_t>(pos);
        for (int d = 0; d < 2; ++d) {
            if (!active[d]) continue;
            if (async_[d]) {
                async_[d]->read(buf_[2 * d], buf_[2 * d + 1], n);
                continue;
            }
            for (int c = 2 * d; c < 2 * d + 2; ++c) {
                if (in[c]) toFloat(in[c], inFormat[c], off, buf_[c], n);
                else std::fill(buf_[c], buf_[c] + n, 0.0f);
            }
        }
        for (int d = 0; d < 2; ++d)
            if (active[d]) decks_[d]->process(buf_[2 * d], buf_[2 * d + 1], n);
        for (int c = 0; c < kChannels; ++c)
            if (out[c]) fromFloat(buf_[c], outFormat[c], out[c], off, n);
    }
    const auto spent = std::chrono::steady_clock::now() - t0;
    busyNs_.store(busyNs_.load(std::memory_order_relaxed) +
                      std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count(),
                  std::memory_order_relaxed);
    const double used = std::chrono::duration<double>(spent).count();
    const double period = frames / fs_;
    const uint32_t permille = static_cast<uint32_t>(std::min(1e9, used / period * 1000.0));
    lastPermille_.store(permille, std::memory_order_relaxed);
    if (permille > heaviestPermille_.load(std::memory_order_relaxed))
        heaviestPermille_.store(permille, std::memory_order_relaxed);
    if (permille > 500) overHalf_.store(overHalf_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (permille > 1000) late_.store(late_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    blocks_.store(blocks_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

EngineHealth AudioEngine::health() const {
    EngineHealth h;
    h.blocks = blocks_.load(std::memory_order_acquire);
    h.overHalf = overHalf_.load(std::memory_order_relaxed);
    h.late = late_.load(std::memory_order_relaxed);
    h.refused = refused_.load(std::memory_order_relaxed);
    h.heaviestPercent = heaviestPermille_.load(std::memory_order_relaxed) / 10.0;
    h.lastPercent = lastPermille_.load(std::memory_order_relaxed) / 10.0;
    h.busySeconds = busyNs_.load(std::memory_order_relaxed) * 1e-9;
    h.core = core_.load(std::memory_order_relaxed);
    return h;
}

} // namespace ap
