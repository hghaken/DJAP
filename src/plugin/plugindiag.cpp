#include "plugindiag.h"

#include "plugin.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace ap::plugin {

namespace {

std::mutex g_fileMutex;
std::mutex g_regMutex;
std::vector<Plugin*> g_live;
std::thread g_logger;
std::condition_variable g_wake;
bool g_stop = false;

std::wstring logFile() {
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, tmp);
    return std::wstring(tmp) + L"DJAP-plugin.log";
}

double db(float x) { return x > 0 ? 20.0 * std::log10(static_cast<double>(x)) : -150.0; }

// Every 2 s a line per live instance: what its audio thread saw since the last line.
void loggerLoop() {
    std::vector<uint64_t> lastCalls, lastChanges;
    std::vector<int> ids;
    std::unique_lock<std::mutex> lock(g_regMutex);
    while (!g_stop) {
        g_wake.wait_for(lock, std::chrono::seconds(2), [] { return g_stop; });
        if (g_stop) break;
        for (Plugin* p : g_live) {
            const ProcessStats& s = p->stats();
            size_t k = 0;
            while (k < ids.size() && ids[k] != p->id()) ++k;
            if (k == ids.size()) ids.push_back(p->id()), lastCalls.push_back(0), lastChanges.push_back(0);
            const uint64_t calls = s.calls.load(), changes = s.changes.load();
            diagLog("#%d: %llu calls in 2 s (%llu in all), %d frames, %s, mode %d, buses %d in / %d out, "
                    "channels %d / %d, %s, %llu parameter changes in 2 s, %llu updates from the controller in all, bypass %d, "
                    "active %d, peak in %.1f / out "
                    "%.1f dBFS, preset %d, window %s",
                    p->id(), static_cast<unsigned long long>(calls - lastCalls[k]),
                    static_cast<unsigned long long>(calls), s.frames.load(), sampleSizeName(s.sampleSize.load()),
                    s.mode.load(), s.inputs.load(), s.outputs.load(), s.inChannels.load(), s.outChannels.load(),
                    s.inPlace.load() == 1 ? "in place" : s.inPlace.load() == 0 ? "separate buffers" : "no buffers yet",
                    static_cast<unsigned long long>(changes - lastChanges[k]),
                    static_cast<unsigned long long>(s.updates.load()), s.bypass.load() ? 1 : 0,
                    p->active() ? 1 : 0, db(s.inPeak.load()), db(s.outPeak.load()), p->presetIndex(),
                    p->hasView() ? "open" : "closed");
            lastCalls[k] = calls;
            lastChanges[k] = changes;
        }
    }
}

} // namespace

void diagLog(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    // at most about 2 MB: past 1 MB the file becomes DJAP-plugin.log.old (the one before that goes)
    const std::wstring file = logFile();
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a) && (a.nFileSizeHigh > 0 || a.nFileSizeLow > (1u << 20)))
        MoveFileExW(file.c_str(), (file + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    if (FILE* f = _wfopen(file.c_str(), L"a")) {
        std::fprintf(f, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
        std::fclose(f);
    }
}

void diagRegister(Plugin* p) {
    std::lock_guard<std::mutex> lock(g_regMutex);
    g_live.push_back(p);
    if (!g_logger.joinable()) {
        g_stop = false;
        g_logger = std::thread(loggerLoop);
    }
}

void diagUnregister(Plugin* p) {
    std::thread stop;
    {
        std::lock_guard<std::mutex> lock(g_regMutex);
        g_live.erase(std::remove(g_live.begin(), g_live.end(), p), g_live.end());
        if (g_live.empty() && g_logger.joinable()) {
            g_stop = true;
            stop = std::move(g_logger);
        }
    }
    g_wake.notify_all();
    if (stop.joinable()) stop.join();
}

int diagLiveInstances() {
    std::lock_guard<std::mutex> lock(g_regMutex);
    return static_cast<int>(g_live.size());
}

} // namespace ap::plugin
