#include "asiohost.h"

#include "appstate.h"

#include <windows.h>
#include <objbase.h>

#include "asiodrivers.h"
#include "asio.h"
#include "asiolist.h"

#include <cmath>

// in asiodrivers.cpp (SDK), not in its header
bool loadAsioDriver(char* name);
extern AsioDrivers* asioDrivers;

namespace ap {

namespace {

std::atomic<AsioHost*> g_host{nullptr};

void cbBufferSwitch(long index, ASIOBool) {
    if (AsioHost* h = g_host.load(std::memory_order_acquire)) h->bufferSwitch(index);
}
ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) {
    cbBufferSwitch(index, direct);
    return params;
}
void cbRateChanged(ASIOSampleRate) {
    if (AsioHost* h = g_host.load(std::memory_order_acquire)) h->rateChanged();
}
long cbMessage(long selector, long value, void*, double*) {
    AsioHost* h = g_host.load(std::memory_order_acquire);
    return h ? h->message(selector, value) : 0;
}
ASIOCallbacks g_callbacks = {cbBufferSwitch, cbRateChanged, cbMessage, cbBufferSwitchTimeInfo};

// Driver strings are in the ANSI code page.
std::string fromAnsi(const char* s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return toUtf8(w);
}

std::string toAnsi(const std::string& utf8) {
    const std::wstring w = fromUtf8(utf8);
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

SampleFormat formatOf(ASIOSampleType t) {
    switch (t) {
    case ASIOSTInt16LSB:
        return SampleFormat::Int16;
    case ASIOSTInt24LSB:
        return SampleFormat::Int24;
    case ASIOSTInt32LSB:
        return SampleFormat::Int32;
    case ASIOSTInt32LSB16:
        return SampleFormat::Int32In16;
    case ASIOSTInt32LSB18:
        return SampleFormat::Int32In18;
    case ASIOSTInt32LSB20:
        return SampleFormat::Int32In20;
    case ASIOSTInt32LSB24:
        return SampleFormat::Int32In24;
    case ASIOSTFloat32LSB:
        return SampleFormat::Float32;
    case ASIOSTFloat64LSB:
        return SampleFormat::Float64;
    default:
        return SampleFormat::Unsupported; // big-endian, DSD
    }
}

std::string asioError(ASIOError e) {
    switch (e) {
    case ASE_NotPresent:
        return "the hardware input or output is not present or available";
    case ASE_HWMalfunction:
        return "the hardware is malfunctioning";
    case ASE_InvalidParameter:
        return "the driver refused a parameter";
    case ASE_InvalidMode:
        return "the driver is in the wrong mode for this";
    case ASE_SPNotAdvancing:
        return "the hardware's sample clock does not advance";
    case ASE_NoClock:
        return "no sample clock";
    case ASE_NoMemory:
        return "the driver ran out of memory";
    default:
        return "ASIO error " + std::to_string(e);
    }
}

} // namespace

AsioHost::AsioHost() { g_host.store(this, std::memory_order_release); }

AsioHost::~AsioHost() {
    close();
    g_host.store(nullptr, std::memory_order_release);
}

std::vector<AsioDriverEntry> AsioHost::drivers() {
    std::vector<AsioDriverEntry> out;
    AsioDriverList list; // reads HKLM\SOFTWARE\ASIO; opens nothing
    const LONG n = list.asioGetNumDev();
    for (LONG i = 0; i < n; ++i) {
        char name[MAXDRVNAMELEN + 1] = {};
        list.asioGetDriverName(static_cast<int>(i), name, MAXDRVNAMELEN);
        AsioDriverEntry e;
        e.name = fromAnsi(name);
        // The SDK lists a driver only when its DLL exists, but leaves the path in the list empty: read it here.
        CLSID clsid{};
        wchar_t guid[64] = {};
        if (list.asioGetDriverCLSID(static_cast<int>(i), &clsid) == 0 && StringFromGUID2(clsid, guid, 64) > 0) {
            const std::wstring key = std::wstring(L"CLSID\\") + guid + L"\\InprocServer32";
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD size = sizeof buf;
            if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) ==
                ERROR_SUCCESS)
                e.path = buf;
        }
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const AsioDriverEntry& a, const AsioDriverEntry& b) { return lessIgnoringCase(a.name, b.name); });
    return out;
}

bool AsioHost::open(const std::string& driver, void* window, std::string& error) {
    close();
    std::string ansi = toAnsi(driver);
    if (!loadAsioDriver(ansi.data())) {
        error = "the ASIO driver \"" + driver + "\" could not be loaded";
        return false;
    }
    ASIODriverInfo info{};
    info.asioVersion = 2;
    info.sysRef = window;
    const ASIOError e = ASIOInit(&info);
    if (e != ASE_OK) {
        error = "the ASIO driver \"" + driver + "\" did not start: " + (info.errorMessage[0] ? fromAnsi(info.errorMessage) : asioError(e));
        if (asioDrivers) asioDrivers->removeCurrentDriver();
        return false;
    }
    open_ = true;
    state_ = AsioDeviceState();
    state_.driver = driver;
    ASIOGetChannels(&state_.inputs, &state_.outputs);
    ASIOGetBufferSize(&state_.minBuffer, &state_.maxBuffer, &state_.preferredBuffer, &state_.granularity);
    for (double r : {44100.0, 48000.0, 88200.0, 96000.0})
        if (ASIOCanSampleRate(r) == ASE_OK) state_.rates.push_back(r);
    for (int dir = 0; dir < 2; ++dir) {
        const long n = dir == 0 ? state_.inputs : state_.outputs;
        for (long c = 0; c < n; ++c) {
            ASIOChannelInfo ci{};
            ci.channel = c;
            ci.isInput = dir == 0 ? ASIOTrue : ASIOFalse;
            std::string name = ASIOGetChannelInfo(&ci) == ASE_OK ? fromAnsi(ci.name) : std::string();
            (dir == 0 ? state_.inputNames : state_.outputNames).push_back(name);
        }
    }
    ASIOSampleRate rate = 0;
    if (ASIOGetSampleRate(&rate) == ASE_OK) state_.sampleRate = rate;
    return true;
}

bool AsioHost::start(AudioEngine& engine, double sampleRate, long bufferSize, const AsioChannels& ch,
                     const std::function<void(double)>& beforeStart, std::string& error) {
    if (!open_) {
        error = "no ASIO driver is open";
        return false;
    }
    stop();
    // channels: the inputs in use and four outputs, all different, inside the driver's counts
    const bool useIn[2] = {ch.inputA > 0, ch.inputB > 0}, useOut[2] = {ch.outputA > 0, ch.outputB > 0};
    if (!useOut[0] && !useOut[1]) {
        error = "no output pair chosen";
        return false;
    }
    const long in[4] = {ch.inputA - 1, ch.inputA, ch.inputB - 1, ch.inputB};
    const long out[4] = {ch.outputA - 1, ch.outputA, ch.outputB - 1, ch.outputB};
    for (int i = 0; i < 4; ++i) {
        if ((useIn[i / 2] && (in[i] < 0 || in[i] >= state_.inputs)) ||
            (useOut[i / 2] && (out[i] < 0 || out[i] >= state_.outputs))) {
            error = "the channel pairs do not fit the interface (" + std::to_string(state_.inputs) + " inputs, " +
                    std::to_string(state_.outputs) + " outputs)";
            return false;
        }
        for (int j = 0; j < i; ++j)
            if ((useIn[i / 2] && useIn[j / 2] && in[i] == in[j]) ||
                (useOut[i / 2] && useOut[j / 2] && out[i] == out[j])) {
                error = "deck A and deck B need different channel pairs";
                return false;
            }
    }
    // sample rate: the one asked for when the driver accepts it, else the driver's own
    ASIOSampleRate rate = 0;
    ASIOGetSampleRate(&rate);
    if (sampleRate > 0 && rate != sampleRate && ASIOCanSampleRate(sampleRate) == ASE_OK) {
        ASIOSetSampleRate(sampleRate);
        ASIOGetSampleRate(&rate);
    }
    if (!(rate >= 22050.0 && rate <= 192000.0)) {
        error = "the driver reports no usable sample rate";
        return false;
    }
    // buffer size: the one asked for when the driver allows it, else its preferred size
    long size = state_.preferredBuffer;
    if (bufferSize > 0 && bufferSize >= state_.minBuffer && bufferSize <= state_.maxBuffer) {
        bool ok = true;
        if (state_.granularity == -1) ok = (bufferSize & (bufferSize - 1)) == 0; // powers of two
        else if (state_.granularity > 0) ok = (bufferSize - state_.minBuffer) % state_.granularity == 0;
        else ok = bufferSize == state_.preferredBuffer; // granularity 0: one size only
        if (ok) size = bufferSize;
    }
    // sample formats
    for (int i = 0; i < 8; ++i) {
        if ((i < 4 && !useIn[i / 2]) || (i >= 4 && !useOut[(i - 4) / 2])) { // from elsewhere, or not used: no buffer
            state_.formats[i] = SampleFormat::Float32;
            continue;
        }
        ASIOChannelInfo ci{};
        ci.channel = i < 4 ? in[i] : out[i - 4];
        ci.isInput = i < 4 ? ASIOTrue : ASIOFalse;
        if (ASIOGetChannelInfo(&ci) != ASE_OK || (state_.formats[i] = formatOf(ci.type)) == SampleFormat::Unsupported) {
            error = "the driver's sample format is not supported (only little-endian PCM and float)";
            return false;
        }
    }
    // buffers
    ASIOBufferInfo infos[8] = {};
    int slot[8], count = 0; // slot[i] = index in infos of channel i (-1: none)
    for (int i = 0; i < 8; ++i) {
        if ((i < 4 && !useIn[i / 2]) || (i >= 4 && !useOut[(i - 4) / 2])) {
            slot[i] = -1;
            continue;
        }
        infos[count].isInput = i < 4 ? ASIOTrue : ASIOFalse;
        infos[count].channelNum = i < 4 ? in[i] : out[i - 4];
        slot[i] = count++;
    }
    const ASIOError e = ASIOCreateBuffers(infos, count, size, &g_callbacks);
    if (e != ASE_OK) {
        error = "the driver could not make its buffers: " + asioError(e);
        return false;
    }
    buffers_ = true;
    for (int i = 0; i < 8; ++i)
        for (int half = 0; half < 2; ++half) bufs_[half][i] = slot[i] >= 0 ? infos[slot[i]].buffers[half] : nullptr;
    for (int i = 4; i < 8; ++i) // outputs start silent
        for (int half = 0; half < 2; ++half)
            if (bufs_[half][i]) silence(state_.formats[i], bufs_[half][i], 0, static_cast<int>(size));
    state_.sampleRate = rate;
    state_.bufferSize = size;
    ASIOGetLatencies(&state_.inputLatency, &state_.outputLatency);
    postOutput_ = ASIOOutputReady() == ASE_OK;

    engine.prepare(rate);
    if (beforeStart) beforeStart(rate);
    engine_ = &engine;
    resetRequest_.store(false, std::memory_order_relaxed);
    latencyChange_.store(false, std::memory_order_relaxed);
    resetPending_ = false;
    const ASIOError s = ASIOStart();
    if (s != ASE_OK) {
        error = "the driver did not start: " + asioError(s);
        stop();
        return false;
    }
    running_ = true;
    return true;
}

void AsioHost::stop() {
    if (running_) ASIOStop();
    running_ = false;
    if (buffers_) ASIODisposeBuffers(); // no more buffer switches after this
    buffers_ = false;
    engine_ = nullptr;
}

void AsioHost::close() {
    stop();
    if (open_) {
        ASIOExit();
        if (asioDrivers) asioDrivers->removeCurrentDriver();
    }
    open_ = false;
}

void AsioHost::controlPanel() {
    if (open_) ASIOControlPanel();
}

AsioEvent AsioHost::poll() {
    const auto now = std::chrono::steady_clock::now();
    if (resetRequest_.exchange(false, std::memory_order_acq_rel)) {
        resetPending_ = true;
        lastReset_ = now;
    }
    if (resetPending_ && now - lastReset_ >= std::chrono::milliseconds(kResetQuietMs)) {
        resetPending_ = false;
        return AsioEvent::Reset;
    }
    if (latencyChange_.exchange(false, std::memory_order_acq_rel)) {
        if (running_) ASIOGetLatencies(&state_.inputLatency, &state_.outputLatency);
        return AsioEvent::LatenciesChanged;
    }
    return AsioEvent::None;
}

void AsioHost::bufferSwitch(long index) {
    AudioEngine* e = engine_;
    if (!e || index < 0 || index > 1) return;
    const void* in[4] = {bufs_[index][0], bufs_[index][1], bufs_[index][2], bufs_[index][3]};
    void* out[4] = {bufs_[index][4], bufs_[index][5], bufs_[index][6], bufs_[index][7]};
    e->noteCore(static_cast<int>(GetCurrentProcessorNumber())); // for the status bar (a few cycles)
    e->process(in, &state_.formats[0], out, &state_.formats[4], static_cast<int>(state_.bufferSize));
    if (postOutput_) ASIOOutputReady();
}

long AsioHost::message(long selector, long value) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
                       value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo ||
                       value == kAsioBufferSizeChange || value == kAsioOverload
                   ? 1
                   : 0;
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest:
    case kAsioBufferSizeChange: // treated like a reset: close and open again with the new size
        resetRequest_.store(true, std::memory_order_release);
        return 1;
    case kAsioResyncRequest:
        resyncs_.fetch_add(1, std::memory_order_relaxed); // may come from any of the driver's threads
        return 1;
    case kAsioLatenciesChanged:
        latencyChange_.store(true, std::memory_order_release);
        return 1;
    case kAsioSupportsTimeInfo:
        return 1; // both buffer switch callbacks lead to the same processing
    case kAsioOverload:
        overloads_.fetch_add(1, std::memory_order_relaxed);
        return 1;
    default:
        return 0;
    }
}

} // namespace ap
