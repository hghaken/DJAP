// A deck input that runs on another clock than the output (docs/design.md section 6.1): a capture stream of a
// Windows audio API (WASAPI / DirectSound / MME) writes into a ring; the ASIO buffer switch reads it at its own rate
// through a windowed-sinc interpolator whose read speed follows the two clocks' drift. The read speed is steered by
// the ring's fill (a PI loop on the fill estimated at the moment of reading, so the capture blocks' sawtooth does not
// modulate it), by at most kMaxCorrection. At a read speed of exactly 1 the output is the input, delayed, bit-exact.
// One writer thread and one reader thread, lock-free; no allocation after construction.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace ap {

struct AsyncInputState {
    bool active = false;       // reading (false: filling up after the start or an underrun; the deck gets silence)
    double targetMs = 0;       // the fill the loop holds: the latency this buffer adds
    double fillMs = 0;         // measured (smoothed)
    double driftPpm = 0;       // the input clock against the output clock (+ = the input runs faster)
    double correctionPpm = 0;  // the read speed's offset from 1 now
    long long underruns = 0;   // periods answered with silence because the input fell behind
    long long overruns = 0;    // input blocks dropped because the ring was full
    long long resyncs = 0;     // read position moved to the target (the fill was far too high)
    long long writes = 0;      // input blocks received
};

class AsyncInput {
public:
    static constexpr int kCapacity = 32768;          // frames (power of two): 0.68 s at 48 kHz
    static constexpr int kHalfTaps = 16;             // interpolator: 32 taps
    static constexpr int kPhases = 256;              // tabulated fractional positions (linear in between)
    static constexpr double kMaxCorrection = 0.002;  // +-0.2 % read speed (3.5 cents)
    static constexpr double kMarginMs = 3.0;         // fill held above the input block + output period
    static constexpr double kMarginStepMs = 2.0;     // added after each underrun ...
    static constexpr double kMarginMaxMs = 20.0;     // ... up to this
    // ... and taken back by this after each such stretch without an underrun, down to kMarginMs: one stall no longer
    // keeps the delay up until the stream starts again (the read speed glides to the smaller fill, no jump)
    static constexpr double kMarginDecayMs = 0.5;
    static constexpr double kMarginCalmS = 60.0;
    static constexpr double kFadeInMs = 10.0;        // reading starts (or starts again after an underrun) with a
                                                     // raised-cosine fade-in: else silence -> music is a step (a click)
    static constexpr double kFadeOutMs = 3.0;        // an underrun ends with a fade over this: what is still there,
                                                     // then its last frame held (else the music stops with a step)
    // Blocks (or output periods) that come within half the last one's length after it count as one burst - a device
    // period cut into short callbacks arrives at once, and the fill must hold all of it. The largest burst of the last
    // kBurstWindowS .. 2 x kBurstWindowS seconds counts, at most kBurstCapMs or 1.5 device periods (setDevicePeriod):
    // the catch-up after a stall is one burst too, and it must not raise the buffer for the rest of the session (fifth
    // outside review). Output callbacks before any input came are not merged: a stream fills its buffer at the start
    // with callbacks straight after each other (sixth review: +20 ms for 20-40 s after every start).
    static constexpr double kBurstGap = 0.5;
    static constexpr double kBurstWindowS = 20.0;
    static constexpr double kBurstCapMs = 50.0;
    // The target goes up at once but down at most this fast: a burst forgotten dropped it by 40 ms in one step, and
    // the read speed jumped to its limit (+2000 ppm, 3.5 cents - a pitch step; sixth outside review). 0.3 ms/s is a
    // read speed of 300 ppm (0.5 cent).
    static constexpr double kTargetFallMsPerS = 0.3;

    AsyncInput();
    AsyncInput(const AsyncInput&) = delete;
    AsyncInput& operator=(const AsyncInput&) = delete;

    // Neither thread running: forget everything, sample rate of both sides (the capture stream converts its device's
    // rate to this one; only the clocks' drift is left).
    void reset(double fs);
    // Test aid: 0 = the loop steers the read speed (normal); > 0 = this fixed read speed.
    void setFixedRatio(double ratio) { fixedRatio_ = ratio; }
    // Neither thread running (after reset): the devices' own period in frames - a burst may be 1.5 of it, also over
    // kBurstCapMs (a 3072-frame packet in callbacks of 512 underran at every period with the cap alone).
    void setDevicePeriod(int frames) { devicePeriod_ = frames > 0 ? frames : 0; }

    // Writer (capture thread): `frames` interleaved stereo frames, received at `nowSeconds` (steady clock).
    void write(const float* interleaved, int frames, double nowSeconds);

    // Reader (output thread): once per device period of `frames` frames, at its start (`nowSeconds`, the same
    // clock), then read() calls that add up to `frames`.
    void beginPeriod(int frames, double nowSeconds);
    void read(float* left, float* right, int frames);

    // Any thread (counters and smoothed values; consistent enough for display).
    AsyncInputState state() const;

private:
    void interpolate(float* left, float* right, int frames);

    // The largest burst (see kBurstWindowS) of one side: blocks closer than a quarter of the last one's length.
    struct Bursts {
        int burst = 0, lastFrames = 0, now = 0, before = 0;
        double lastTime = -1.0, windowStart = -1.0;
        int add(int frames, double time, double fs, int cap, bool merge = true);
    };
    int burstCap(int frames) const;

    double fs_ = 48000.0;
    double fixedRatio_ = 0.0;
    std::unique_ptr<float[]> ring_;                    // interleaved; (kCapacity + 2 * kHalfTaps) frames, mirrored
    // writer -> reader
    std::atomic<int64_t> written_{0};
    std::atomic<double> writeTime_{0.0};
    std::atomic<int> maxBlock_{0};
    std::atomic<long long> overruns_{0}, writes_{0};
    Bursts writeBursts_; // writer only
    // reader -> writer
    std::atomic<int64_t> oldestNeeded_{0};
    // reader only
    int64_t pos_ = 0;       // integer read position (frame index) ...
    double frac_ = 0.0;     // ... and its fraction
    double ratio_ = 1.0;    // input frames per output frame in this period
    double avgFill_ = 0.0;  // frames
    double integ_ = 0.0;    // the loop's integral part (= the drift estimate)
    double marginMs_ = kMarginMs;
    double calmS_ = 0.0; // seconds of reading since the last underrun or margin step down
    double marginFloorMs_ = 0.0; // the decay stops here: the margin that held before a step down that failed
    bool decayed_ = false;       // a step down since the last underrun
    int period_ = 0;        // the largest output burst lately (playBursts_)
    Bursts playBursts_;
    int fadeOut_ = 0, fadeOutTotal_ = 0; // after an underrun: frames still to fade out, of so many ...
    int fadeRing_ = 0;                   // ... the first of them still in the ring, the rest the last frame held
    float lastL_ = 0.0f, lastR_ = 0.0f;  // the last frame read (before the fade-out's gain)
    double targetS_ = 0.0;               // the target, going down at most kTargetFallMsPerS (frames)
    int devicePeriod_ = 0;               // setDevicePeriod
    double fade_ = 1.0;     // fade-in position 0..1 (1: done)
    bool active_ = false, periodOk_ = false, haveAvg_ = false;
    // reader -> display
    std::atomic<bool> activeShown_{false};
    std::atomic<double> targetShown_{0}, fillShown_{0}, driftShown_{0}, corrShown_{0};
    std::atomic<long long> underruns_{0}, resyncs_{0};
};

} // namespace ap
