#include "audioio.h"

#include "wavfile.h"

#include <algorithm>
#include <chrono>

namespace ap {

SysInputs sysInputsFrom(const Settings& s) {
    SysInputs in;
    if (s.asioInputs != "ASIO" && apiFromName(s.asioInputs, in.api)) {
        in.device[0] = s.asioInputDevice[0];
        in.device[1] = s.decks == 2 ? s.asioInputDevice[1] : std::string();
    }
    in.exclusive = s.exclusive;
    in.periodMs = in.api == AudioApi::Wasapi ? s.inputPeriodMs : 0; // DirectSound / MME: no such mode
    return in;
}

AsioChannels asioChannelsFrom(const Settings& s) {
    AsioChannels c{s.inputA, s.inputB, s.outputA, s.outputB};
    if (s.decks == 1) c.inputB = c.outputB = 0; // deck B not used
    return c;
}

void sysRoutesFrom(const Settings& s, SysRoute routes[2]) {
    for (int d = 0; d < 2; ++d)
        routes[d] = d == 1 && s.decks == 1 ? SysRoute() : SysRoute{s.inDevice[d], s.outDevice[d]};
}

AudioIO::AudioIO() = default;

AudioIO::~AudioIO() {
    fadeOutDecks(); // Exit, Windows ending the session, the setup closing the program
    sys_.stop();
    closeDriver();
    stopSimulation();
}

bool AudioIO::startSystem(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames,
                          bool exclusive, int driftCorrection, std::string& error) {
    fadeOutDecks(); // a running stream (another device or API) stops first
    stopSimulation(); // one source at a time; the engine is prepared again
    closeDriver();
    sys_.stop(); // the streams that may read the AsyncInputs end before they are reset
    AsyncInput* drift[2] = {nullptr, nullptr};
    for (int d = 0; d < 2; ++d)
        if (driftCorrection != Settings::kDriftOff && !routes[d].input.empty()) {
            async_[d].reset(sampleRate); // the capture stream converts its device's rate to the decks' rate
            drift[d] = &async_[d];
        }
    const bool ok = sys_.start(api, routes, sampleRate, periodFrames, exclusive, drift,
                               driftCorrection == Settings::kDriftAuto, engine_,
                               [this](double) {
                                   publish(0); // the decks' configurations for the rate in use
                                   publish(1);
                               },
                               error);
    if (ok) {
        resetMeters(0);
        resetMeters(1);
        deviceError_.clear();
    }
    return ok;
}

void AudioIO::resetHealth() {
    engine_.resetHealth();
    sys_.resetHealth();
}

bool AudioIO::openDriver(const std::string& driver, void* window, std::string& error) {
#if AP_HAVE_ASIO
    window_ = window;
    resetPending_ = false; // the user's own choice ends an automatic reopening
    if (!host_.open(driver, window, error)) return false;
    driver_ = driver;
    deviceError_.clear();
    return true;
#else
    (void)driver;
    (void)window;
    error = "built without the ASIO SDK";
    return false;
#endif
}

void AudioIO::closeDriver() {
#if AP_HAVE_ASIO
    resetPending_ = false;
    if (host_.running()) fadeOutDecks();
    host_.close();
#endif
}

bool AudioIO::startDevice(double sampleRate, long bufferSize, const AsioChannels& channels, const SysInputs& inputs,
                          std::string& error) {
#if AP_HAVE_ASIO
    // a start of the user's own ends an automatic reopening: else its next try closed the stream just started
    if (!retrying_) resetPending_ = false;
    fadeOutDecks(); // a running stream (a restart with new choices) stops first
    stopSimulation(); // one source at a time; the engine is prepared again for the device's rate
    sys_.stop();
    rate_ = sampleRate;
    buffer_ = bufferSize;
    channels_ = channels;
    inputs_ = inputs;
    AsioChannels ch = channels;
    if (!inputs_.device[0].empty()) ch.inputA = 0; // no ASIO input for a deck that captures elsewhere
    if (!inputs_.device[1].empty() || ch.outputB == 0) ch.inputB = 0;
    bool ok = host_.start(engine_, sampleRate, bufferSize, ch,
                          [this](double rate) {
                              publish(0); // the decks' configurations for the rate in use
                              publish(1);
                              for (int d = 0; d < 2; ++d)
                                  if (!inputs_.device[d].empty()) {
                                      async_[d].reset(rate);
                                      engine_.setAsyncInput(d, &async_[d]);
                                  }
                          },
                          error);
    if (ok && inputs_.any()) {
        // the capture streams at the rate the driver runs (silence on those decks until they have filled up)
        AsyncInput* const targets[2] = {inputs_.device[0].empty() ? nullptr : &async_[0],
                                        inputs_.device[1].empty() ? nullptr : &async_[1]};
        // a shorter input period (seventh outside review: the 10 ms default is the largest part of the latency
        // left): the chosen ms in frames; SysAudio puts a shared request on the device's own step (eighth review)
        const double rate = host_.state().sampleRate;
        const unsigned capFrames = inputs_.periodMs > 0 ? static_cast<unsigned>(std::lround(rate * inputs_.periodMs / 1000.0))
                                                        : inputs_.periodFrames;
        if (!sys_.startCapture(inputs_.api, inputs_.device, host_.state().sampleRate, capFrames, inputs_.exclusive,
                               targets, error)) {
            host_.stop();
            ok = false;
        }
    }
    if (ok) {
        resetMeters(0);
        resetMeters(1);
        engine_.resetHealth();
        deviceError_.clear();
    }
    return ok;
#else
    (void)sampleRate;
    (void)bufferSize;
    (void)channels;
    (void)inputs;
    error = "built without the ASIO SDK";
    return false;
#endif
}

void AudioIO::fadeOutDecks() {
    if (!deviceRunning()) return;
    uint64_t before[2];
    for (int d = 0; d < 2; ++d) {
        before[d] = engine_.deck(d).processedFrames();
        engine_.deck(d).startFadeOut();
    }
    // done when every running deck is silent; a deck that processed nothing for 100 ms does not run (no devices, or
    // one channel) - longer than the longest device period (2048 frames at 44.1 kHz: 46 ms); never more than 300 ms
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool done = true;
        for (int d = 0; d < 2; ++d) {
            const DeckProcessor& k = engine_.deck(d);
            if (!k.fadedOut() && !(ms >= 100.0 && k.processedFrames() == before[d])) done = false;
        }
        if (done || ms > 300.0) break;
    }
    // the silence through the device: ASIO plays the period computed before, WASAPI keeps a period or two queued
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void AudioIO::stopDevice() {
#if AP_HAVE_ASIO
    resetPending_ = false; // Stop also ends an automatic reopening
#endif
    fadeOutDecks(); // Stop, another API, a device that stopped by itself (then nothing runs and nothing waits)
    sys_.stop();
#if AP_HAVE_ASIO
    host_.stop();
#endif
}

bool AudioIO::driverOpen() const {
#if AP_HAVE_ASIO
    return host_.isOpen();
#else
    return false;
#endif
}

bool AudioIO::deviceRunning() const {
#if AP_HAVE_ASIO
    if (host_.running()) return true;
#endif
    return sys_.running();
}

void AudioIO::setDeck(int d, const PresetValues& values, const DeckControl& control) {
    values_[d] = values;
    control_[d] = control;
    haveDeck_[d] = true;
    publish(d);
}

void AudioIO::publish(int d) {
    if (haveDeck_[d] && engine_.sampleRate() > 0)
        engine_.deck(d).publish(makeDeckConfig(values_[d], control_[d], engine_.sampleRate()));
}

void AudioIO::poll() {
    const double dt = 0.05; // the GUI timer's period
#if AP_HAVE_ASIO
    const auto now = std::chrono::steady_clock::now();
    if (host_.isOpen() && host_.poll() == AsioEvent::Reset) { // the driver asked: close, open and start again
        resetWasRunning_ = host_.running();
        if (resetWasRunning_) fadeOutDecks(); // the stream may still run: no hard stop (a click)
        host_.close();
        resetPending_ = true;
        resetTries_ = 0;
        resetDue_ = now;
    }
    if (resetPending_ && now >= resetDue_) {
        std::string err;
        if (host_.isOpen()) host_.close(); // the last try opened it but could not start it
        retrying_ = true;
        const bool ok = host_.open(driver_, window_, err) &&
                        (!resetWasRunning_ || startDevice(rate_, buffer_, channels_, SysInputs(inputs_), err));
        retrying_ = false;
        if (ok) {
            resetPending_ = false;
            deviceError_.clear();
        } else if (++resetTries_ >= kResetTries) {
            resetPending_ = false;
            deviceError_ = "the driver asked for a reset; reopening failed: " + err;
        } else {
            resetDue_ = now + std::chrono::seconds(1);
            deviceError_ = "the driver asked for a reset; trying again: " + err;
        }
    }
#endif
    if (sys_.running() && sys_.failed()) { // a stream stopped by itself
        stopDevice(); // with ASIO too, when it was a deck's input
        deviceError_ = "an audio device stopped (unplugged, or taken by another program); start the audio again";
    }
    for (int d = 0; d < 2; ++d) monitor_[d].poll(engine_.deck(d), engine_.sampleRate(), dt);
}

void AudioIO::resetMeters(int d) { monitor_[d].reset(); }

bool AudioIO::simulate(const std::filesystem::path& file, std::string& error) {
    stopSimulation();
    stopDevice(); // the engine is prepared again below: the driver must not be using it
    WavData w;
    try { // a file too large for memory: a message, not the end of the program
        if (!readWav(file, w, error)) return false;
    } catch (const std::exception& e) {
        error = std::string("cannot read the file: ") + e.what();
        return false;
    }
    if (w.channels != 2 && w.channels != 4) {
        error = "the simulation needs a 2-channel (deck A) or 4-channel (deck A + B) WAV file";
        return false;
    }
    if (w.frames() < static_cast<size_t>(AudioEngine::kChunk)) {
        error = "the file is too short";
        return false;
    }
    simData_ = std::move(w.samples);
    simChannels_ = w.channels;
    simFile_ = file;
    engine_.prepare(w.sampleRate);
    for (int d = 0; d < 2; ++d) {
        publish(d);
        float z = 0;
        engine_.deck(d).process(&z, &z, 0); // take the configuration before the first period
        resetMeters(d);
    }
    simStop_ = false;
    simRunning_ = true;
    sim_ = std::thread([this] { simLoop(); });
    return true;
}

void AudioIO::stopSimulation() {
    simStop_ = true;
    if (sim_.joinable()) sim_.join();
    simRunning_ = false;
}

void AudioIO::simLoop() {
    constexpr int kPeriod = AudioEngine::kChunk;
    float in[4][kPeriod] = {}, out[4][kPeriod] = {};
    const void* inp[4] = {in[0], in[1], in[2], in[3]};
    void* outp[4] = {out[0], out[1], out[2], out[3]};
    const SampleFormat f[4] = {SampleFormat::Float32, SampleFormat::Float32, SampleFormat::Float32,
                               SampleFormat::Float32};
    const size_t frames = simData_.size() / static_cast<size_t>(simChannels_);
    const auto period = std::chrono::duration<double>(kPeriod / engine_.sampleRate());
    auto next = std::chrono::steady_clock::now();
    size_t pos = 0;
    while (!simStop_.load(std::memory_order_relaxed)) {
        for (int i = 0; i < kPeriod; ++i) {
            const float* s = &simData_[(pos % frames) * static_cast<size_t>(simChannels_)];
            for (int c = 0; c < 4; ++c) in[c][i] = c < simChannels_ ? s[c] : 0.0f;
            ++pos;
        }
        engine_.process(inp, f, outp, f, kPeriod); // output thrown away: nothing is played
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

} // namespace ap
