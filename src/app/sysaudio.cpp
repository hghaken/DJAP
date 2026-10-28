#include "sysaudio.h"

#include "appstate.h"
#include "sampleformat.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#pragma warning(push, 0)
#include "miniaudio.h"
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace ap {

namespace {

std::atomic<bool> g_nullBackend{false};
std::atomic<bool> g_oneDevice{false}; // SysAudio::assumeOneDevice

ma_backend backendOf(AudioApi api) {
    if (g_nullBackend.load()) return ma_backend_null;
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

// A device's period (in its own rate's frames) in the decks' frames, for AsyncInput::setDevicePeriod: a device at
// 96 kHz under decks at 48 kHz made the burst cap twice too large (seventh outside review). (sampleformat.h)
int deckFrames(ma_uint32 frames, ma_uint32 deviceRate, double deckRate) { return framesAtRate(frames, deviceRate, deckRate); }

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

// One deck's stream: duplex, capture-only (into `capture`), or - with drift correction - a capture stream (`dev`, into
// `capture`) and a playback stream (`play`, reading `capture`).
struct DeckStream {
    ma_device dev{}, play{};
    bool inited = false, playInited = false;
    DeckProcessor* deck = nullptr;
    AsyncInput* capture = nullptr; // capture-only stream: writes here
    double rate = 48000;
    float l[AudioEngine::kChunk] = {}, r[AudioEngine::kChunk] = {};
    // health, written by the stream's thread
    std::atomic<long long> blocks{0}, overHalf{0}, late{0};
    std::atomic<uint32_t> heaviest{0}, last{0};
    std::atomic<long long> busyNs{0};
    std::atomic<int> core{-1};
    std::atomic<bool> resetHealth{false}, stopping{false}, failed{false};
    SysStreamInfo info;
};

struct SysAudio::Impl {
    ma_context ctx{};
    bool ctxInited = false;
    std::unique_ptr<DeckStream> decks[2];
};

namespace {

// The period's health (processing time against the period, blocks counted).
void account(DeckStream* s, std::chrono::steady_clock::time_point t0, ma_uint32 frames) {
    const auto spent = std::chrono::steady_clock::now() - t0;
    s->busyNs.store(s->busyNs.load(std::memory_order_relaxed) +
                        std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count(),
                    std::memory_order_relaxed);
    s->core.store(static_cast<int>(GetCurrentProcessorNumber()), std::memory_order_relaxed);
    const double used = std::chrono::duration<double>(spent).count();
    const uint32_t permille = static_cast<uint32_t>(std::min(1e9, used / (frames / s->rate) * 1000.0));
    s->last.store(permille, std::memory_order_relaxed);
    if (permille > s->heaviest.load(std::memory_order_relaxed)) s->heaviest.store(permille, std::memory_order_relaxed);
    if (permille > 500) s->overHalf.store(s->overHalf.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (permille > 1000) s->late.store(s->late.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    s->blocks.store(s->blocks.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void resetIfAsked(DeckStream* s) {
    if (s->resetHealth.exchange(false, std::memory_order_acq_rel)) {
        s->blocks.store(0, std::memory_order_relaxed);
        s->overHalf.store(0, std::memory_order_relaxed);
        s->late.store(0, std::memory_order_relaxed);
        s->heaviest.store(0, std::memory_order_relaxed);
    }
}

void dataCallback(ma_device* dev, void* output, const void* input, ma_uint32 frames) {
    DeckStream* s = static_cast<DeckStream*>(dev->pUserData);
    const auto t0 = std::chrono::steady_clock::now();
    resetIfAsked(s);
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
    account(s, t0, frames);
}

void captureCallback(ma_device* dev, void*, const void* input, ma_uint32 frames) {
    DeckStream* s = static_cast<DeckStream*>(dev->pUserData);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (input) s->capture->write(static_cast<const float*>(input), static_cast<int>(frames), now);
    // with a playback stream of its own the deck's periods are that stream's (counted there)
    if (!s->playInited) s->blocks.store(s->blocks.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

// Drift correction: the output device's period reads the deck's input from the AsyncInput (docs/design.md 6.1).
void playbackCallback(ma_device* dev, void* output, const void*, ma_uint32 frames) {
    DeckStream* s = static_cast<DeckStream*>(dev->pUserData);
    const auto t0 = std::chrono::steady_clock::now();
    resetIfAsked(s);
    s->capture->beginPeriod(static_cast<int>(frames), std::chrono::duration<double>(t0.time_since_epoch()).count());
    float* out = static_cast<float*>(output);
    for (ma_uint32 pos = 0; pos < frames; pos += AudioEngine::kChunk) {
        const ma_uint32 n = std::min<ma_uint32>(AudioEngine::kChunk, frames - pos);
        s->capture->read(s->l, s->r, static_cast<int>(n));
        s->deck->process(s->l, s->r, static_cast<int>(n));
        for (ma_uint32 i = 0; i < n; ++i) {
            out[2 * (pos + i)] = s->l[i];
            out[2 * (pos + i) + 1] = s->r[i];
        }
    }
    account(s, t0, frames);
}

void notificationCallback(const ma_device_notification* n) {
    DeckStream* s = static_cast<DeckStream*>(n->pDevice->pUserData);
    if (n->type == ma_device_notification_type_stopped && !s->stopping.load(std::memory_order_acquire))
        s->failed.store(true, std::memory_order_release); // stopped without being asked: unplugged, taken over, error
}

// The physical device a WASAPI endpoint belongs to (its container id), or a zero GUID when unknown - or when it is the
// computer's own container: every built-in device (an on-board codec, a graphics card's HDMI output) and every software
// device (a virtual cable) shares that one, on clocks of their own.
GUID containerOf(IMMDeviceEnumerator* e, const wchar_t* id) {
    static const PROPERTYKEY kContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
    static const GUID kComputer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
    GUID g{};
    IMMDevice* dev = nullptr;
    if (FAILED(e->GetDevice(id, &dev))) return g;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kContainerId, &v)) && v.vt == VT_CLSID && v.puuid) g = *v.puuid;
        PropVariantClear(&v);
        ps->Release();
    }
    dev->Release();
    return g == kComputer ? GUID{} : g;
}

// A deck's input and output on one physical device (WASAPI: the same container, not the computer's own): one clock, so
// nothing drifts - drift correction would only add its buffer (fourth outside review). DirectSound and MME do not say.
bool oneClock(AudioApi api, const ma_device_id& in, const ma_device_id& out) {
    if (g_oneDevice.load()) return true;
    if (api != AudioApi::Wasapi || g_nullBackend.load()) return false;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the helper thread (miniaudio joined it already)
    bool same = false;
    IMMDeviceEnumerator* e = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&e)))) {
        const GUID a = containerOf(e, in.wasapi), b = containerOf(e, out.wasapi);
        same = a != GUID{} && a == b;
        e->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return same;
}

// The shared-mode periods of a WASAPI endpoint, at its mix format (COM on the calling thread).
SysAudio::EnginePeriods periodsOf(const wchar_t* id) {
    SysAudio::EnginePeriods p;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* e = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&e)))) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(e->GetDevice(id, &dev))) {
            IAudioClient3* client = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
                WAVEFORMATEX* mix = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                    UINT32 def = 0, step = 0, lo = 0, hi = 0;
                    if (SUCCEEDED(client->GetSharedModeEnginePeriod(mix, &def, &step, &lo, &hi)) && step > 0) {
                        p.known = true;
                        p.rate = mix->nSamplesPerSec;
                        p.defaultFrames = def;
                        p.stepFrames = step;
                        p.minFrames = lo;
                        p.maxFrames = hi;
                    }
                    CoTaskMemFree(mix);
                }
                client->Release(); // never initialised: no stream, the device is not taken
            }
            dev->Release();
        }
        e->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return p;
}

const ma_device_id* findDevice(AudioApi api, const ma_device_info* list, ma_uint32 count, const std::string& name) {
    for (ma_uint32 i = 0; i < count; ++i)
        if (name == nameOf(api, list[i].name)) return &list[i].id;
    return nullptr;
}

} // namespace

SysAudio::SysAudio() : impl_(new Impl()) {}

void SysAudio::useNullBackend(bool on) { g_nullBackend.store(on); }

void SysAudio::assumeOneDevice(bool on) { g_oneDevice.store(on); }

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

// (the mode converts no rate: the device must run at the decks' rate in Windows; live, 2026-10-28: the device's
// shortest period of a virtual cable, 1 ms, was shorter than the cable's own 3 ms timer and the sound came out
// distorted - hence a choice of 5 / 3 / 2 ms in Setup, not "the shortest")
unsigned SysAudio::onStep(const EnginePeriods& p, unsigned frames, double rate) {
    if (!p.known || std::fabs(p.rate - rate) > 0.5 || frames == 0) return 0;
    const unsigned up = (std::max(frames, p.minFrames) + p.stepFrames - 1) / p.stepFrames * p.stepFrames;
    return up <= p.maxFrames ? up : 0;
}

SysAudio::EnginePeriods SysAudio::inputPeriods(const std::string& device) {
    EnginePeriods p;
    if (g_nullBackend.load()) return p;
    onHelper([&] {
        ma_context ctx;
        const ma_backend b = ma_backend_wasapi;
        if (ma_context_init(&b, 1, nullptr, &ctx) != MA_SUCCESS) return;
        ma_device_info* play = nullptr;
        ma_device_info* cap = nullptr;
        ma_uint32 nPlay = 0, nCap = 0;
        if (ma_context_get_devices(&ctx, &play, &nPlay, &cap, &nCap) == MA_SUCCESS)
            if (const ma_device_id* id = findDevice(AudioApi::Wasapi, cap, nCap, device)) p = periodsOf(id->wasapi);
        ma_context_uninit(&ctx);
    });
    return p;
}

bool SysAudio::start(AudioApi api, const SysRoute routes[2], double sampleRate, unsigned periodFrames, bool exclusive,
                     AsyncInput* const drift[2], bool oneDeviceDuplex, AudioEngine& engine, const std::function<void(double)>& beforeStart,
                     std::string& error) {
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
            const bool sameDevice = drift && drift[d] && oneDeviceDuplex && oneClock(api, *inId, *outId);
            if (drift && drift[d] && !sameDevice) { // a capture stream into the AsyncInput, a playback stream reading it
                s->capture = drift[d];
                ma_device_config c = ma_device_config_init(ma_device_type_capture);
                c.sampleRate = static_cast<ma_uint32>(sampleRate);
                c.periodSizeInFrames = periodFrames;
                c.performanceProfile = ma_performance_profile_low_latency;
                c.wasapi.usage = ma_wasapi_usage_pro_audio;
                c.capture.pDeviceID = inId;
                c.capture.format = ma_format_f32;
                c.capture.channels = 2;
                ma_device_config p = ma_device_config_init(ma_device_type_playback);
                p.sampleRate = static_cast<ma_uint32>(sampleRate);
                p.periodSizeInFrames = periodFrames;
                p.performanceProfile = ma_performance_profile_low_latency;
                p.wasapi.usage = ma_wasapi_usage_pro_audio;
                p.playback.pDeviceID = outId;
                p.playback.format = ma_format_f32;
                p.playback.channels = 2;
                // the output's own thread only reads the drift buffer (it never waits for the input): 2 periods are
                // enough for WASAPI, one period less latency (fourth outside review). DirectSound and MME keep 3.
                if (api == AudioApi::Wasapi) p.periods = 2;
                if (exclusive && api == AudioApi::Wasapi) {
                    c.capture.shareMode = p.playback.shareMode = ma_share_mode_exclusive;
                    c.periods = p.periods = 2;
                    // exclusive streams run at the device's own rate: when it is not the decks' rate, miniaudio's
                    // converter takes it - with its steepest low-pass (the default order 4 lets more alias through)
                    c.resampling.linear.lpfOrder = p.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
                }
                c.dataCallback = captureCallback;
                p.dataCallback = playbackCallback;
                c.notificationCallback = p.notificationCallback = notificationCallback;
                c.pUserData = p.pUserData = s.get();
                r = ma_device_init(&m.ctx, &c, &s->dev);
                if (r != MA_SUCCESS) {
                    error = std::string(deckName) + ": " + maError("opening the input device failed", r);
                    return;
                }
                s->inited = true;
                r = ma_device_init(&m.ctx, &p, &s->play);
                if (r != MA_SUCCESS) {
                    m.decks[d] = std::move(s); // stop() closes the input device
                    error = std::string(deckName) + ": " + maError("opening the output device failed", r);
                    return;
                }
                s->playInited = true;
                // the devices' own periods: a burst may be 1.5 of them (asyncinput.h; neither stream runs yet)
                drift[d]->setDevicePeriod(std::max(
                    deckFrames(s->dev.capture.internalPeriodSizeInFrames, s->dev.capture.internalSampleRate, sampleRate),
                    deckFrames(s->play.playback.internalPeriodSizeInFrames, s->play.playback.internalSampleRate,
                               sampleRate)));
                SysStreamInfo& in = s->info;
                in.input = routes[d].input;
                in.output = routes[d].output;
                in.driftCorrected = true;
                in.askedFrames = periodFrames;
                in.deviceRate = s->play.playback.internalSampleRate;
                in.periodFrames = s->play.playback.internalPeriodSizeInFrames;
                in.periods = s->play.playback.internalPeriods;
                const double capMs = s->dev.capture.internalSampleRate
                                         ? 1000.0 * s->dev.capture.internalPeriodSizeInFrames / s->dev.capture.internalSampleRate
                                         : 0.0;
                const double playMs = in.deviceRate ? 1000.0 * in.periodFrames * in.periods / in.deviceRate : 0.0;
                in.latencyMs = capMs + playMs; // the drift buffer comes on top (AsyncInputState::targetMs)
                m.decks[d] = std::move(s);
                continue;
            }
            ma_device_config c = ma_device_config_init(ma_device_type_duplex);
            c.sampleRate = static_cast<ma_uint32>(sampleRate);
            c.periodSizeInFrames = periodFrames;
            c.performanceProfile = ma_performance_profile_low_latency;
            c.wasapi.usage = ma_wasapi_usage_pro_audio; // the stream's thread in MMCSS "Pro Audio" (WASAPI only)
            c.capture.pDeviceID = inId;
            c.capture.format = ma_format_f32;
            c.capture.channels = 2;
            c.playback.pDeviceID = outId;
            c.playback.format = ma_format_f32;
            c.playback.channels = 2;
            // shared mode: when the rate asked for is the device's own, miniaudio takes Windows' low-latency engine
            // (IAudioClient3) by itself - it leaves out the conversion flags that would block it (miniaudio.h, "Low
            // latency shared mode"). Exclusive: 2 periods instead of 3, one period less latency (outside review)
            if (exclusive && api == AudioApi::Wasapi) {
                c.capture.shareMode = c.playback.shareMode = ma_share_mode_exclusive;
                c.periods = 2;
                c.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER; // see above
            }
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
            in.sameDevice = sameDevice;
            in.askedFrames = periodFrames;
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
            // the input first: the output then finds it filling up (it plays silence until the buffer is full)
            r = ma_device_start(&m.decks[d]->dev);
            if (r == MA_SUCCESS && m.decks[d]->playInited) r = ma_device_start(&m.decks[d]->play);
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
            // shared WASAPI: a period asked for goes on the device's step (else Windows' default)
            unsigned asked = periodFrames;
            if (periodFrames > 0 && api == AudioApi::Wasapi && !exclusive && !g_nullBackend.load())
                asked = onStep(periodsOf(inId->wasapi), periodFrames, sampleRate);
            ma_device_config c = ma_device_config_init(ma_device_type_capture);
            c.sampleRate = static_cast<ma_uint32>(sampleRate);
            c.periodSizeInFrames = asked;
            c.performanceProfile = ma_performance_profile_low_latency;
            c.wasapi.usage = ma_wasapi_usage_pro_audio;
            c.capture.pDeviceID = inId;
            c.capture.format = ma_format_f32;
            c.capture.channels = 2;
            if (exclusive && api == AudioApi::Wasapi) {
                c.capture.shareMode = ma_share_mode_exclusive;
                c.periods = 2;
            }
            c.dataCallback = captureCallback;
            c.notificationCallback = notificationCallback;
            c.pUserData = s.get();
            r = ma_device_init(&m.ctx, &c, &s->dev);
            if (r != MA_SUCCESS) {
                error = std::string(deckName) + ": " + maError("opening the input device failed", r);
                return;
            }
            s->inited = true;
            // as above; the output (ASIO) already reads here - setDevicePeriod is atomic for that
            targets[d]->setDevicePeriod(
                deckFrames(s->dev.capture.internalPeriodSizeInFrames, s->dev.capture.internalSampleRate, sampleRate));
            SysStreamInfo& in = s->info;
            in.input = devices[d];
            in.askedFrames = asked;
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
            if (s->playInited) ma_device_uninit(&s->play); // the reader first, then the input it reads
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
        h.busySeconds += s->busyNs.load(std::memory_order_relaxed) * 1e-9;
        if (h.core < 0) h.core = s->core.load(std::memory_order_relaxed); // (each deck has a thread: deck A's)
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
