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

AudioIO::AudioIO() {
    for (int d = 0; d < 2; ++d) {
        in_[d].reset();
        out_[d].reset();
    }
}

AudioIO::~AudioIO() {
    fadeOutDecks(); // Exit, Windows ending the session, the setup closing the program
    sys_.stop();
    closeDriver();
    stopSimulation();
}

bool AudioIO::startSystem(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames,
                          bool exclusive, std::string& error) {
    fadeOutDecks(); // a running stream (another device or API) stops first
    stopSimulation(); // one source at a time; the engine is prepared again
    closeDriver();
    const bool ok = sys_.start(api, routes, sampleRate, periodFrames, exclusive, engine_,
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
    if (host_.running()) fadeOutDecks();
    host_.close();
#endif
}

bool AudioIO::startDevice(double sampleRate, long bufferSize, const AsioChannels& channels, const SysInputs& inputs,
                          std::string& error) {
#if AP_HAVE_ASIO
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
        if (!sys_.startCapture(inputs_.api, inputs_.device, host_.state().sampleRate, inputs_.periodFrames,
                               inputs_.exclusive, targets, error)) {
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
    if (host_.isOpen() && host_.poll() == AsioEvent::Reset) { // the driver asked: close, open and start again
        const bool wasRunning = host_.running();
        host_.close();
        std::string err;
        if (!host_.open(driver_, window_, err) ||
            (wasRunning && !startDevice(rate_, buffer_, channels_, SysInputs(inputs_), err)))
            deviceError_ = "the driver asked for a reset; reopening failed: " + err;
    }
#endif
    if (sys_.running() && sys_.failed()) { // a stream stopped by itself
        stopDevice(); // with ASIO too, when it was a deck's input
        deviceError_ = "an audio device stopped (unplugged, or taken by another program); start the audio again";
    }
    for (int d = 0; d < 2; ++d) {
        DeckMeters& m = meters_[d];
        MeterRecord recs[64];
        int n, total = 0;
        float tp[2] = {0, 0}, pin[2] = {0, 0};
        double comp = 0, dess = 0, clip = 0, lim = 0;
        while ((n = engine_.deck(d).meters().pop(recs, 64)) > 0) {
            for (int i = 0; i < n; ++i) {
                const MeterRecord& r = recs[i];
                in_[d].addRecord(r.energyIn, r.frames);
                out_[d].addRecord(r.energyOut, r.frames);
                tp[0] = std::max(tp[0], r.tpOut[0]);
                tp[1] = std::max(tp[1], r.tpOut[1]);
                pin[0] = std::max(pin[0], r.peakIn[0]);
                pin[1] = std::max(pin[1], r.peakIn[1]);
                comp = std::max(comp, static_cast<double>(r.compressorDb));
                dess = std::max(dess, static_cast<double>(r.deEsserDb));
                clip = std::max(clip, static_cast<double>(r.clipperDb));
                lim = std::max(lim, static_cast<double>(r.limiterDb));
                m.levellerDb = r.levellerDb;
                m.compressorIn = r.compressorIn;
                m.bands = r.bands;
                for (int b = 0; b < kMaxBands; ++b) m.densityDb[b] = r.densityDb[b];
                m.flags = r.flags;
                m.seconds += r.frames / std::max(1.0, engine_.sampleRate());
                // the signal-chain bars: each point's level, a 0.4 s one-pole over the records
                if (r.frames > 0) {
                    const double a = std::min(1.0, r.frames / (0.4 * std::max(1.0, engine_.sampleRate())));
                    for (int k = 0; k < kChainTaps; ++k) m.chain[k] += a * (r.chain[k] / r.frames - m.chain[k]);
                }
                m.active = r.active;
            }
            total += n;
        }
        quietFor_[d] = total > 0 ? 0.0 : quietFor_[d] + dt;
        m.live = quietFor_[d] < 0.5;
        if (total > 0) {
            m.compressorDb = comp;
            m.deEsserDb = dess;
            m.clipperDb = clip;
            m.limiterDb = lim;
        }
        // true peak: the newest value, falling 20 dB/s when lower
        for (int c = 0; c < 2; ++c) {
            const double now = linToDb(tp[c]);
            m.tpNow[c] = std::max(now, m.tpNow[c] - 20.0 * dt);
            m.tpMax = std::max(m.tpMax, now);
            m.inPeak[c] = std::max(linToDb(pin[c]), m.inPeak[c] - 20.0 * dt);
        }
        m.inM = in_[d].momentary();
        m.inI = in_[d].integrated();
        m.outM = out_[d].momentary();
        m.outS = out_[d].shortTerm();
        m.outI = out_[d].integrated();
        m.outLra = out_[d].loudnessRange();
        m.outMaxM = out_[d].maxMomentary();
        if (!m.live) {
            m.tpNow[0] = m.tpNow[1] = kFloorDb;
            m.inPeak[0] = m.inPeak[1] = kFloorDb;
            m.levellerDb = m.compressorDb = m.deEsserDb = m.clipperDb = m.limiterDb = 0;
            m.compressorIn = kFloorDb;
            for (double& x : m.densityDb) x = 0;
            for (double& x : m.chain) x = 0;
        }
        drainTaps(d);
    }
    // graph history: a point every 0.1 s
    if (++historyTick_ >= 2) {
        historyTick_ = 0;
        for (int d = 0; d < 2; ++d) {
            const DeckMeters& m = meters_[d];
            DeckAnalysis::Point p;
            p.live = m.live;
            p.inM = static_cast<float>(m.inM);
            p.outM = static_cast<float>(m.outM);
            p.leveller = static_cast<float>(m.levellerDb);
            p.compressor = static_cast<float>(-m.compressorDb);
            double dens = 0;
            for (int b = 0; b < m.bands; ++b) dens += m.densityDb[b];
            p.density = static_cast<float>(m.bands > 0 ? dens / m.bands : 0.0);
            DeckAnalysis& a = analysis_[d];
            a.history[static_cast<size_t>(a.historyPos)] = p;
            a.historyPos = (a.historyPos + 1) % DeckAnalysis::kHistory;
        }
    }
}

void AudioIO::drainTaps(int d) {
    DeckAnalysis& a = analysis_[d];
    const double fs = engine_.sampleRate();
    if (fs > 0 && fs != analysisRate_) { // a new rate: every analyzer starts again
        analysisRate_ = fs;
        for (DeckAnalysis& x : analysis_) {
            x.specIn.reset(fs);
            x.specOut.reset(fs);
            x.stereoIn.reset(fs);
            x.stereoOut.reset(fs);
        }
    }
    constexpr int kBlock = 1024;
    TapFrame frames[kBlock];
    float inL[kBlock], inR[kBlock], outL[kBlock], outR[kBlock], inMono[kBlock], outMono[kBlock];
    int n;
    while ((n = engine_.deck(d).taps().pop(frames, kBlock)) > 0) {
        for (int i = 0; i < n; ++i) {
            inL[i] = frames[i].inL;
            inR[i] = frames[i].inR;
            outL[i] = frames[i].outL;
            outR[i] = frames[i].outR;
            inMono[i] = 0.5f * (inL[i] + inR[i]);
            outMono[i] = 0.5f * (outL[i] + outR[i]);
        }
        a.specIn.feed(inMono, n);
        a.specOut.feed(outMono, n);
        a.stereoIn.feed(inL, inR, n);
        a.stereoOut.feed(outL, outR, n);
    }
}

void AudioIO::resetMeters(int d) {
    in_[d].reset();
    out_[d].reset();
    analysis_[d].history.assign(DeckAnalysis::kHistory, DeckAnalysis::Point());
    DeckMeters fresh;
    fresh.live = meters_[d].live;
    meters_[d] = fresh;
}

bool AudioIO::simulate(const std::filesystem::path& file, std::string& error) {
    stopSimulation();
    stopDevice(); // the engine is prepared again below: the driver must not be using it
    WavData w;
    if (!readWav(file, w, error)) return false;
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
