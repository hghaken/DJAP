#include "sysaudio.h"

#include "appstate.h"

#include <windows.h>

#pragma warning(push, 0)
#include "miniaudio.h"
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace ap {

namespace {

ma_backend backendOf(AudioApi api) {
    switch (api) {
    case AudioApi::DirectSound:
        return ma_backend_dsound;
    case AudioApi::Mme:
        return ma_backend_winmm;
    default:
        return ma_backend_wasapi;
    }
}

// Runs f on a fresh thread and waits for it (device open / close outside the GUI thread's COM apartment).
template <class F>
void onHelper(F&& f) {
    std::thread t(std::forward<F>(f));
    t.join();
}

std::string maError(const char* what, ma_result r) { return std::string(what) + ": " + ma_result_description(r); }

// Device names as UTF-8: WASAPI reports UTF-8, DirectSound and MME the ANSI code page (handbook 12).
std::string nameOf(AudioApi api, const char* raw) {
    if (api == AudioApi::Wasapi) return raw;
    const int n = MultiByteToWideChar(CP_ACP, 0, raw, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw, -1, w.data(), n);
    return toUtf8(w);
}

} // namespace

const char* apiName(AudioApi api) {
    switch (api) {
    case AudioApi::Asio:
        return "ASIO";
    case AudioApi::Wasapi:
        return "WASAPI";
    case AudioApi::DirectSound:
        return "DirectSound";
    default:
        return "MME";
    }
}

const char* apiLabel(AudioApi api) {
    switch (api) {
    case AudioApi::Asio:
        return "ASIO (one driver, channel pairs)";
    case AudioApi::Wasapi:
        return "WASAPI (Windows; shared or exclusive)";
    case AudioApi::DirectSound:
        return "DirectSound (older)";
    default:
        return "MME (oldest)";
    }
}

bool apiFromName(const std::string& name, AudioApi& api) {
    for (AudioApi a : {AudioApi::Asio, AudioApi::Wasapi, AudioApi::DirectSound, AudioApi::Mme})
        if (name == apiName(a)) {
            api = a;
            return true;
        }
    return false;
}

// One deck's duplex stream.
struct DeckStream {
    ma_device dev{};
    bool inited = false;
    DeckProcessor* deck = nullptr;
    AsyncInput* capture = nullptr; // capture-only stream: writes here
    double rate = 48000;
    float l[AudioEngine::kChunk] = {}, r[AudioEngine::kChunk] = {};
    // health, written by the stream's thread
    std::atomic<long long> blocks{0}, overHalf{0}, late{0};
    std::atomic<uint32_t> heaviest{0}, last{0};
    std::atomic<bool> resetHealth{false}, stopping{false}, failed{false};
    SysStreamInfo info;
};

struct SysAudio::Impl {
    ma_context ctx{};
    bool ctxInited = false;
    std::unique_ptr<DeckStream> decks[2];
};

namespace {

void dataCallback(ma_device* dev, void* output, const void* input, ma_uint32 frames) {
    DeckStream* s = static_cast<DeckStream*>(dev->pUserData);
    const auto t0 = std::chrono::steady_clock::now();
    if (s->resetHealth.exchange(false, std::memory_order_acq_rel)) {
        s->blocks.store(0, std::memory_order_relaxed);
        s->overHalf.store(0, std::memory_order_relaxed);
        s->late.store(0, std::memory_order_relaxed);
        s->heaviest.store(0, std::memory_order_relaxed);
    }
    const float* in = static_cast<const float*>(input);
    float* out = static_cast<float*>(output);
    for (ma_uint32 pos = 0; pos < frames; pos += AudioEngine::kChunk) {
        const ma_uint32 n = std::min<ma_uint32>(AudioEngine::kChunk, frames - pos);
        for (ma_uint32 i = 0; i < n; ++i) {
            s->l[i] = in ? in[2 * (pos + i)] : 0.0f;
            s->r[i] = in ? in[2 * (pos + i) + 1] : 0.0f;
        }
        s->deck->process(s->l, s->r, static_cast<int>(n));
        for (ma_uint32 i = 0; i < n; ++i) {
            out[2 * (pos + i)] = s->l[i];
            out[2 * (pos + i) + 1] = s->r[i];
        }
    }
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const uint32_t permille = static_cast<uint32_t>(std::min(1e9, used / (frames / s->rate) * 1000.0));
    s->last.store(permille, std::memory_order_relaxed);
    if (permille > s->heaviest.load(std::memory_order_relaxed)) s->heaviest.store(permille, std::memory_order_relaxed);
    if (permille > 500) s->overHalf.store(s->overHalf.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (permille > 1000) s->late.store(s->late.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    s->blocks.store(s->blocks.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void captureCallback(ma_device* dev, void*, const void* input, ma_uint32 frames) {
    DeckStream* s = static_cast<DeckStream*>(dev->pUserData);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (input) s->capture->write(static_cast<const float*>(input), static_cast<int>(frames), now);
    s->blocks.store(s->blocks.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void notificationCallback(const ma_device_notification* n) {
    DeckStream* s = static_cast<DeckStream*>(n->pDevice->pUserData);
    if (n->type == ma_device_notification_type_stopped && !s->stopping.load(std::memory_order_acquire))
        s->failed.store(true, std::memory_order_release); // stopped without being asked: unplugged, taken over, error
}

const ma_device_id* findDevice(AudioApi api, const ma_device_info* list, ma_uint32 count, const std::string& name) {
    for (ma_uint32 i = 0; i < count; ++i)
        if (name == nameOf(api, list[i].name)) return &list[i].id;
    return nullptr;
}

} // namespace

SysAudio::SysAudio() : impl_(new Impl()) {}

SysAudio::~SysAudio() { stop(); }

bool SysAudio::devices(AudioApi api, std::vector<SysDevice>& inputs, std::vector<SysDevice>& outputs,
                       std::string& error) {
    inputs.clear();
    outputs.clear();
    bool ok = false;
    onHelper([&] {
        ma_context ctx;
        const ma_backend b = backendOf(api);
        ma_result r = ma_context_init(&b, 1, nullptr, &ctx);
        if (r != MA_SUCCESS) {
            error = maError("audio system not available", r);
            return;
        }
        ma_device_info* play = nullptr;
        ma_device_info* cap = nullptr;
        ma_uint32 nPlay = 0, nCap = 0;
        r = ma_context_get_devices(&ctx, &play, &nPlay, &cap, &nCap);
        if (r == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < nCap; ++i) inputs.push_back({nameOf(api, cap[i].name), cap[i].isDefault != 0});
            for (ma_uint32 i = 0; i < nPlay; ++i) outputs.push_back({nameOf(api, play[i].name), play[i].isDefault != 0});
            // alphabetical, ignoring case (the APIs list devices in no useful order)
            auto byName = [](const SysDevice& a, const SysDevice& b) { return lessIgnoringCase(a.name, b.name); };
            std::stable_sort(inputs.begin(), inputs.end(), byName);
            std::stable_sort(outputs.begin(), outputs.end(), byName);
            ok = true;
        } else {
            error = maError("device list", r);
        }
        ma_context_uninit(&ctx);
    });
    return ok;
}

bool SysAudio::start(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames, bool exclusive,
                     AudioEngine& engine, const std::function<void(double)>& beforeStart, std::string& error) {
    stop();
    for (int d = 0; d < 2; ++d)
        if (routes[d].input.empty() != routes[d].output.empty()) {
            error = std::string(d == 0 ? "deck A" : "deck B") + " needs an input and an output device (or neither)";
            return false;
        }
    if (routes[0].input.empty() && routes[1].input.empty()) {
        error = "no devices chosen for either deck";
        return false;
    }
    api_ = api;
    engine.prepare(sampleRate);
    if (beforeStart) beforeStart(sampleRate);
    Impl& m = *impl_;
    bool ok = false;
    onHelper([&] {
        const ma_backend b = backendOf(api);
        ma_result r = ma_context_init(&b, 1, nullptr, &m.ctx);
        if (r != MA_SUCCESS) {
            error = maError("audio system not available", r);
            return;
        }
        m.ctxInited = true;
        ma_device_info* play = nullptr;
        ma_device_info* cap = nullptr;
        ma_uint32 nPlay = 0, nCap = 0;
        r = ma_context_get_devices(&m.ctx, &play, &nPlay, &cap, &nCap);
        if (r != MA_SUCCESS) {
            error = maError("device list", r);
            return;
        }
        for (int d = 0; d < 2; ++d) {
            if (routes[d].input.empty()) continue;
            const char* deckName = d == 0 ? "deck A" : "deck B";
            const ma_device_id* inId = findDevice(api, cap, nCap, routes[d].input);
            const ma_device_id* outId = findDevice(api, play, nPlay, routes[d].output);
            if (!inId || !outId) {
                error = std::string(deckName) + ": the device \"" + (!inId ? routes[d].input : routes[d].output) +
                        "\" is not there";
                return;
            }
            auto s = std::make_unique<DeckStream>();
            s->deck = &engine.deck(d);
            s->rate = sampleRate;
            ma_device_config c = ma_device_config_init(ma_device_type_duplex);
            c.sampleRate = static_cast<ma_uint32>(sampleRate);
            c.periodSizeInFrames = periodFrames;
            c.performanceProfile = ma_performance_profile_low_latency;
            c.capture.pDeviceID = inId;
            c.capture.format = ma_format_f32;
            c.capture.channels = 2;
            c.playback.pDeviceID = outId;
            c.playback.format = ma_format_f32;
            c.playback.channels = 2;
            if (exclusive && api == AudioApi::Wasapi)
                c.capture.shareMode = c.playback.shareMode = ma_share_mode_exclusive;
            c.dataCallback = dataCallback;
            c.notificationCallback = notificationCallback;
            c.pUserData = s.get();
            r = ma_device_init(&m.ctx, &c, &s->dev);
            if (r != MA_SUCCESS) {
                error = std::string(deckName) + ": " + maError("opening the devices failed", r);
                return;
            }
            s->inited = true;
            SysStreamInfo& in = s->info;
            in.input = routes[d].input;
            in.output = routes[d].output;
            in.deviceRate = s->dev.playback.internalSampleRate;
            in.periodFrames = s->dev.playback.internalPeriodSizeInFrames;
            in.periods = s->dev.playback.internalPeriods;
            const double capMs = s->dev.capture.internalSampleRate
                                     ? 1000.0 * s->dev.capture.internalPeriodSizeInFrames / s->dev.capture.internalSampleRate
                                     : 0.0;
            const double playMs = in.deviceRate ? 1000.0 * in.periodFrames * in.periods / in.deviceRate : 0.0;
            in.latencyMs = capMs + playMs;
            m.decks[d] = std::move(s);
        }
        for (int d = 0; d < 2; ++d) {
            if (!m.decks[d]) continue;
            r = ma_device_start(&m.decks[d]->dev);
            if (r != MA_SUCCESS) {
                error = std::string(d == 0 ? "deck A" : "deck B") + ": " + maError("starting failed", r);
                return;
            }
            m.decks[d]->info.running = true;
        }
        ok = true;
    });
    if (!ok) stop();
    return ok;
}

bool SysAudio::startCapture(AudioApi api, const std::string devices[2], double sampleRate, unsigned periodFrames,
                            bool exclusive, AsyncInput* const targets[2], std::string& error) {
    stop();
    api_ = api;
    capturing_ = true;
    Impl& m = *impl_;
    bool ok = false;
    onHelper([&] {
        const ma_backend b = backendOf(api);
        ma_result r = ma_context_init(&b, 1, nullptr, &m.ctx);
        if (r != MA_SUCCESS) {
            error = maError("audio system not available", r);
            return;
        }
        m.ctxInited = true;
        ma_device_info* play = nullptr;
        ma_device_info* cap = nullptr;
        ma_uint32 nPlay = 0, nCap = 0;
        r = ma_context_get_devices(&m.ctx, &play, &nPlay, &cap, &nCap);
        if (r != MA_SUCCESS) {
            error = maError("device list", r);
            return;
        }
        for (int d = 0; d < 2; ++d) {
            if (devices[d].empty() || !targets[d]) continue;
            const char* deckName = d == 0 ? "deck A" : "deck B";
            const ma_device_id* inId = findDevice(api, cap, nCap, devices[d]);
            if (!inId) {
                error = std::string(deckName) + ": the input device \"" + devices[d] + "\" is not there";
                return;
            }
            auto s = std::make_unique<DeckStream>();
            s->capture = targets[d];
            s->rate = sampleRate;
            ma_device_config c = ma_device_config_init(ma_device_type_capture);
            c.sampleRate = static_cast<ma_uint32>(sampleRate);
            c.periodSizeInFrames = periodFrames;
            c.performanceProfile = ma_performance_profile_low_latency;
            c.capture.pDeviceID = inId;
            c.capture.format = ma_format_f32;
            c.capture.channels = 2;
            if (exclusive && api == AudioApi::Wasapi) c.capture.shareMode = ma_share_mode_exclusive;
            c.dataCallback = captureCallback;
            c.notificationCallback = notificationCallback;
            c.pUserData = s.get();
            r = ma_device_init(&m.ctx, &c, &s->dev);
            if (r != MA_SUCCESS) {
                error = std::string(deckName) + ": " + maError("opening the input device failed", r);
                return;
            }
            s->inited = true;
            SysStreamInfo& in = s->info;
            in.input = devices[d];
            in.deviceRate = s->dev.capture.internalSampleRate;
            in.periodFrames = s->dev.capture.internalPeriodSizeInFrames;
            in.periods = s->dev.capture.internalPeriods;
            in.latencyMs = in.deviceRate ? 1000.0 * in.periodFrames / in.deviceRate : 0.0;
            m.decks[d] = std::move(s);
        }
        for (int d = 0; d < 2; ++d) {
            if (!m.decks[d]) continue;
            r = ma_device_start(&m.decks[d]->dev);
            if (r != MA_SUCCESS) {
                error = std::string(d == 0 ? "deck A" : "deck B") + ": " + maError("starting the input failed", r);
                return;
            }
            m.decks[d]->info.running = true;
        }
        ok = true;
    });
    if (!ok) stop();
    return ok;
}

void SysAudio::stop() {
    Impl& m = *impl_;
    capturing_ = false;
    if (!m.ctxInited && !m.decks[0] && !m.decks[1]) return;
    onHelper([&] {
        for (auto& s : m.decks) {
            if (!s) continue;
            s->stopping.store(true, std::memory_order_release);
            if (s->inited) ma_device_uninit(&s->dev); // stops the stream first
            s.reset();
        }
        if (m.ctxInited) ma_context_uninit(&m.ctx);
        m.ctxInited = false;
    });
}

bool SysAudio::running() const {
    return (impl_->decks[0] && impl_->decks[0]->info.running) || (impl_->decks[1] && impl_->decks[1]->info.running);
}

SysStreamInfo SysAudio::info(int deck) const {
    return impl_->decks[deck] ? impl_->decks[deck]->info : SysStreamInfo();
}

EngineHealth SysAudio::health() const {
    EngineHealth h;
    for (const auto& s : impl_->decks) {
        if (!s) continue;
        h.blocks += s->blocks.load(std::memory_order_acquire);
        h.overHalf += s->overHalf.load(std::memory_order_relaxed);
        h.late += s->late.load(std::memory_order_relaxed);
        h.heaviestPercent = std::max(h.heaviestPercent, s->heaviest.load(std::memory_order_relaxed) / 10.0);
        h.lastPercent = std::max(h.lastPercent, s->last.load(std::memory_order_relaxed) / 10.0);
    }
    return h;
}

void SysAudio::resetHealth() {
    for (const auto& s : impl_->decks)
        if (s) s->resetHealth.store(true, std::memory_order_release);
}

bool SysAudio::failed() const {
    for (const auto& s : impl_->decks)
        if (s && s->failed.load(std::memory_order_acquire)) return true;
    return false;
}

} // namespace ap
