// The live audio path of both decks (docs/design.md section 6): one device period at a time, converted from the
// device's sample format, in chunks of at most kChunk frames through fixed buffers, deck A then deck B, converted
// back. Health counters for the Audio window (handbook 11.7). Platform-neutral: the ASIO host calls process() from
// the driver's thread; tests call it with buffers of their own.
#pragma once

#include "asyncinput.h"
#include "deck.h"
#include "sampleformat.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ap {

struct EngineHealth {
    long long blocks = 0;       // periods processed since the last reset
    long long overHalf = 0;     // periods that took more than half their time
    long long late = 0;         // periods that took longer than their time (the driver then plays old data)
    long long refused = 0;      // periods answered with silence (engine not prepared for this call)
    double heaviestPercent = 0; // longest processing time in % of its period
    double lastPercent = 0;
};

class AudioEngine {
public:
    static constexpr int kChunk = 256;
    static constexpr int kChannels = 4; // deck A left, right, deck B left, right

    AudioEngine();
    // Sets the sample rate of both decks and allocates; no deck reads an AsyncInput afterwards. Not real-time safe:
    // call while the device is stopped.
    void prepare(double fs);
    double sampleRate() const { return fs_; }
    DeckProcessor& deck(int d) { return *decks_[d]; }
    // After prepare(), while the device is stopped: deck d takes its input from `input` (another clock: a capture
    // stream of another audio API) instead of the device's input channels; nullptr = the device's channels.
    void setAsyncInput(int d, AsyncInput* input) { async_[d] = input; }
    AsyncInput* asyncInput(int d) const { return async_[d]; }

    // Audio thread: one device period of `frames` frames. in[c] / out[c] are the device buffers of channel c
    // (kChannels each) in the formats given; an input that is nullptr reads as silence, an output that is nullptr is
    // not written, and a deck with neither (one-channel use) is not processed. `nowSeconds` = the time of
    // the period on the steady clock (for an AsyncInput; < 0 = read the clock here). No allocation, no lock, no I/O.
    void process(const void* const* in, const SampleFormat* inFormat, void* const* out, const SampleFormat* outFormat,
                 int frames, double nowSeconds = -1.0);

    // GUI thread.
    EngineHealth health() const;
    void resetHealth() { resetRequest_.store(true, std::memory_order_release); }

private:
    double fs_ = 0.0;
    std::unique_ptr<DeckProcessor> decks_[2];
    AsyncInput* async_[2] = {nullptr, nullptr};
    float buf_[kChannels][kChunk];
    // written by the audio thread only (plain stores), read by the GUI
    std::atomic<long long> blocks_{0}, overHalf_{0}, late_{0}, refused_{0};
    std::atomic<uint32_t> heaviestPermille_{0}, lastPermille_{0};
    std::atomic<bool> resetRequest_{false};
};

} // namespace ap
