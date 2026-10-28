// SPDX-License-Identifier: GPL-3.0-or-later
// DJAP.exe: the GUI program. Options (for tests): --ini <file> uses another settings file;
// --simulate <file.wav> runs the file through both decks in real time with the output thrown away (no sound).
// Without them only one instance runs; the first start (no audio device chosen) opens the Audio window.
// --autostart (the Run entry of "Start with Windows"): the window starts hidden in the notification area.
#include "appstate.h"
#include "audioio.h"
#include "mainwnd.h"
#include "placement.h"
#include "settings.h"
#include "settingslock.h"
#include "theme.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <dbghelp.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ap;

namespace {

// --diag <file>: on an access violation, a C++ exception or a failed callback, the call stack goes into <file>
// (first chance, so also exceptions that are caught later). For finding faults on machines without a debugger.
std::wstring g_diagFile;

LONG CALLBACK diagHandler(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xE06D7363 && code != 0xC000041D &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    FILE* f = _wfopen(g_diagFile.c_str(), L"a");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE proc = GetCurrentProcess();
    std::fprintf(f, "exception 0x%08lX at %p, thread %lu\n", code, ep->ExceptionRecord->ExceptionAddress,
                 GetCurrentThreadId());
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr);
         ++i) {
        const DWORD64 pc = sf.AddrPC.Offset;
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        char module[MAX_PATH] = "?";
        const DWORD64 base = SymGetModuleBase64(proc, pc);
        if (base) GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
        const char* name = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof line;
        DWORD ldisp = 0;
        if (SymGetLineFromAddr64(proc, pc, &ldisp, &line))
            std::fprintf(f, "  %s!%s +0x%llx  (%s:%lu)\n", std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module,
                         name, disp, line.FileName, line.LineNumber);
        else
            std::fprintf(f, "  %s!%s +0x%llx\n", std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module,
                         name, disp);
    }
    std::fprintf(f, "\n");
    std::fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // the ASIO driver is a COM object created and called on this thread (handbook 12)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // No power throttling (EcoQoS): Windows 11 may slow a program that is hidden in the notification area - DJAP's
    // usual place - and coalesce its timers; its audio must keep its pace. Ignored before Windows 10 1709.
    PROCESS_POWER_THROTTLING_STATE power{};
    power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    power.StateMask = 0; // both off
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &power, sizeof power);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS}; // (About's link)
    InitCommonControlsEx(&icc);
    theme::init();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path ini, simulate;
    bool autostart = false; // started by Windows at logon (the Run entry): straight into the notification area
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--autostart") autostart = true;
        else if (a == L"--ini" && i + 1 < argc) ini = argv[++i];
        else if (a == L"--simulate" && i + 1 < argc) simulate = argv[++i];
        else if (a == L"--diag" && i + 1 < argc) g_diagFile = argv[++i];
    }
    if (argv) LocalFree(argv);
    const bool testRun = !ini.empty() || !simulate.empty();
    // One program on the user's own settings at a time (it may be hidden in the notification area): a second start
    // shows the first one's window and ends. Test runs with --ini or --simulate are left alone.
    HANDLE running = nullptr;
    if (!testRun) {
        // createUserMutex: open to the user's programs elevated or not; access denied all the same means one runs
        auto exists = [&] {
            const DWORD e = GetLastError();
            return running ? e == ERROR_ALREADY_EXISTS : e == ERROR_ACCESS_DENIED;
        };
        running = createUserMutex(L"Local\\DJAP.Running");
        bool another = exists();
        // the first one may be ending (its window gone, the driver not yet let go - a quick restart): wait for it,
        // up to 5 s, instead of ending with nothing running (outside review)
        for (int i = 0; i < 25 && another && !FindWindowW(MainWindow::windowClass(), nullptr); ++i) {
            if (running) CloseHandle(running);
            Sleep(200);
            running = createUserMutex(L"Local\\DJAP.Running");
            another = exists();
        }
        if (another) {
            if (HWND first = FindWindowW(MainWindow::windowClass(), nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(first, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(first, MainWindow::showMessage(), 0, 0);
            }
            if (running) CloseHandle(running);
            theme::shutdown();
            CoUninitialize();
            return 0;
        }
    }
    if (!g_diagFile.empty()) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        AddVectoredExceptionHandler(1, diagHandler);
    }
    if (ini.empty()) {
        adoptOldSettings(); // the settings of the program's old name, AudioProcessor
        ini = settingsFile();
    }

    Settings settings;
    std::error_code ec;
    // a settings file that is there but cannot be read (a virus scanner, a sync tool holding it): tried for about 2 s;
    // then the program runs on the defaults and saves nothing - its first save would have written the defaults over
    // the user's driver, devices and StartAudio (outside review)
    bool unreadable = false;
    if (std::filesystem::exists(ini, ec)) {
        bool loaded = settings.load(ini);
        for (int i = 0; i < 10 && !loaded; ++i) {
            Sleep(200);
            loaded = settings.load(ini);
        }
        unreadable = !loaded;
    }

    int result = 0;
    {
        // the audio (the ASIO driver is a COM object) and the windows end inside the COM apartment: their
        // destructors stop and release the driver and the volume objects, so CoUninitialize comes after this block
        AudioIO audio;
        MainWindow window(settings, ini, audio);
        if (!window.create(instance, autostart && settings.audioConfigured(), testRun)) {
            MessageBoxW(nullptr, L"The main window could not be created.", L"DJAP", MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            if (unreadable) {
                window.holdSaves("settings file not readable - nothing is saved until DJAP starts again: " +
                                 toUtf8(ini.wstring()));
                const std::wstring msg = L"The settings file could not be read:\n" + ini.wstring() +
                                         L"\n\nDJAP runs with its default settings and saves nothing, so the file "
                                         L"stays as it is. Close the program that holds it, then start DJAP again.";
                placement::messageBox(window.hwnd(), msg.c_str(), L"DJAP", MB_OK | MB_ICONWARNING);
            }
            if (simulate.empty()) window.startSavedAudio(); // the audio of the last session (StartAudio)
            if (simulate.empty() && !settings.audioConfigured()) window.openAudioWindow(); // first start: a device
            if (!simulate.empty()) {
                std::string err;
                if (audio.simulate(simulate, err)) {
                    window.applyAll(); // configurations for the file's sample rate
                } else {
                    const std::wstring msg = L"Simulation not started: " + fromUtf8(err);
                    placement::messageBox(window.hwnd(), msg.c_str(), L"DJAP", MB_OK | MB_ICONWARNING);
                }
            }

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (AboutWindow::takeKey(msg)) continue;
                HWND active = GetActiveWindow(); // keyboard navigation in the main window and the preset window
                if (!active || !IsDialogMessageW(active, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            audio.stopSimulation();
        }
    }
    theme::shutdown();
    CoUninitialize();
    // last: a new start of the program waits until this one has let go of the driver
    if (running) CloseHandle(running);
    return result;
}
