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
#include <string>

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
    double blockMs = 0;        // the largest input block (burst) lately, as measured: what the buffer must hold
    double steadyMs = 0;       // the median finished input burst lately: the input's period as it really runs
    long long repeats = 0;     // input blocks read twice (up to 4 times) bit for bit: a device read faster than it is
                               // filled (a virtual cable's timer slower than the period chosen); a long run of equal
                               // blocks (a test tone's period) and a flat block (DC, a clipped plateau) never count
    long long recentRepeats = 0; // of them, those of the last kBurstWindowS .. 2 x kBurstWindowS seconds
};

// What Health says about an input of another API under the ASIO output (tenth outside review: a pure function, so a
// table test can pin its rules). Empty: nothing to say; else "; ..." parts to append to the deck's line.
struct InputNoteFacts {
    double steadyMs = 0, blockMs = 0;
    long long recentRepeats = 0;
    bool lowLatency = false;   // shared WASAPI: Windows' low-latency periods apply
    double requestedMs = 0;    // Setup's Input period (0: Windows' default)
    double onStepMs = 0;       // the request on the device's step (0: none fits - Windows' default)
    double defaultMs = 0;      // the device's default period in Windows (0: not known)
    bool rateDiffers = false;  // the device runs at another rate in Windows than the decks
    double sampleRate = 48000; // the decks' rate (for the message)
};
std::string inputNotes(const InputNoteFacts& f);
// Health shows repeated blocks from this many in the recent window on (one is a coincidence or the stream's start).
constexpr long long kRepeatsShown = 3;

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
    // outside review). The output's callbacks before its first real gap are not merged: a stream fills its buffer at
    // the start with callbacks straight after each other (sixth review: +20 ms for 20-40 s after every start).
    static constexpr double kBurstGap = 0.5;
    static constexpr double kBurstWindowS = 20.0;
    static constexpr double kBurstCapMs = 50.0;
    // The target goes up at once but down at most this fast: a burst forgotten dropped it by 40 ms in one step, and
    // the read speed jumped to its limit (+2000 ppm, 3.5 cents - a pitch step; sixth outside review). 0.3 ms/s is a
    // read speed of 300 ppm (0.5 cent).
    static constexpr double kTargetFallMsPerS = 0.3;
    // An output whose callbacks never leave a gap starts reading after this anyway (eighth outside review).
    static constexpr double kPrimeTimeoutS = 1.0;

    AsyncInput();
    AsyncInput(const AsyncInput&) = delete;
    AsyncInput& operator=(const AsyncInput&) = delete;

    // Neither thread running: forget everything, sample rate of both sides (the capture stream converts its device's
    // rate to this one; only the clocks' drift is left).
    void reset(double fs);
    // Test aid: 0 = the loop steers the read speed (normal); > 0 = this fixed read speed.
    void setFixedRatio(double ratio) { fixedRatio_ = ratio; }
    // Any thread, after reset (the ASIO + capture route sets it while the output already reads - seventh outside
    // review): the devices' own period in frames at this object's rate - a burst may be 1.5 of it, also over
    // kBurstCapMs (a 3072-frame packet in callbacks of 512 underran at every period with the cap alone).
    void setDevicePeriod(int frames) { devicePeriod_.store(frames > 0 ? frames : 0, std::memory_order_relaxed); }

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

    // The largest burst (see kBurstWindowS) of one side: blocks closer than kBurstGap of the last one's length.
    struct Bursts {
        // the finished bursts of a window by length, in bins of 0.25 ms (the last bin: everything longer): how many
        // and their sum in frames - the median's bin and the mean length in it (exact for a steady stream)
        static constexpr int kBins = 96;
        struct Histogram {
            int count[kBins] = {};
            int64_t sum[kBins] = {};
        };
        int burst = 0, lastFrames = 0, now = 0, before = 0;
        Histogram histNow, histBefore;
        double lastTime = -1.0, windowStart = -1.0;
        // count false: only the time and size are noted (for the gap test), the window keeps what it had
        int add(int frames, double time, double fs, int cap, bool merge = true, bool count = true);
        // the median finished burst of the two windows (0: none finished yet) - unlike the largest, a stall's
        // catch-up does not move it (ninth outside review), and unlike the smallest (the ninth round's rule) one
        // short burst does not either (tenth review: 16 frames showed 0.33 ms for up to 40 s)
        int steady() const;
    };
    int burstCap(int frames) const;

    double fs_ = 48000.0;
    double fixedRatio_ = 0.0;
    std::unique_ptr<float[]> ring_;                    // interleaved; (kCapacity + 2 * kHalfTaps) frames, mirrored
    // writer -> reader
    std::atomic<int64_t> written_{0};
    std::atomic<double> writeTime_{0.0};
    std::atomic<int> maxBlock_{0}, steadyBlock_{0};
    std::atomic<long long> overruns_{0}, writes_{0}, repeats_{0}, recentRepeats_{0};
    Bursts writeBursts_; // writer only
    // writer only: the last block's length, and how many blocks in a row repeated the one before bit for bit (a short
    // run counts when a new block ends it; a periodic signal repeats block for block, endlessly)
    int lastWriteFrames_ = 0, repeatRun_ = 0;
    static constexpr int kRepeatRunMax = 3; // a block read up to 4 times counts; longer runs are a signal's period
    long long repeatsNow_ = 0, repeatsBefore_ = 0; // writer only: repeats per window of kBurstWindowS
    double repeatWindow_ = -1.0;
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
    std::atomic<int> devicePeriod_{0};   // setDevicePeriod
    double fade_ = 1.0;     // fade-in position 0..1 (1: done)
    bool active_ = false, periodOk_ = false, haveAvg_ = false;
    int playGaps_ = 0; // the output's real gaps since reset, up to 2: merge from the first, read from the second
    double playStart_ = -1.0; // the output's first callback since reset (kPrimeTimeoutS)
    // reader -> display
    std::atomic<bool> activeShown_{false};
    std::atomic<double> targetShown_{0}, fillShown_{0}, driftShown_{0}, corrShown_{0};
    std::atomic<long long> underruns_{0}, resyncs_{0};
};

} // namespace ap
