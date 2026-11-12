// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwnd.h"

#include "appstate.h"
#include "meterview.h"
#include "placement.h"
#include "resource.h"
#include "settingslock.h"
#include "stages.h"
#include "version.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace ap {

namespace {

const wchar_t* const kClass = L"APMainWindow";
const UINT_PTR kTimer = 1;
const UINT kTimerMs = 50; // meters at 20 Hz
const UINT_PTR kStartTimer = 2; // startSavedAudio() again
// The Windows volume is read more often than the meters are drawn: the input factor that undoes it (updateVolume) must
// not come later than the music that carries the new volume - that music needs 30-60 ms to get here (WASAPI), a look
// every 50 ms could be later than that. Read in time, the factor is only ever early, which the deck covers
// (kVolumeHoldS). One read takes some tens of microseconds (winvolume_test prints it).
const UINT_PTR kVolumeTimer = 3;
const UINT kVolumeMs = 15;
const UINT kStartRetryMs = 5000;

// control ids: deck d uses 1000 + 100 d + item
enum Item { iPreset = 1, iProcessing, iEdit = 5, iColour, iEditColour, iGraphs, iMeter = 30 };
int idOf(int d, int item) { return 1000 + 100 * d + item; }

// commands of the right-click and notification-area menus (also sent by GUI tests: 100 setup, 102 exit, 103
// analysis, 104 open)
enum MenuId { mAudio = 100, mAbout, mExit, mAnalysis, mOpen, mSpectrum, mHealth }; // 103 / 105: GUI tests open the windows

std::wstring deckName(int d) { return d == 0 ? L"Deck A" : L"Deck B"; }

// " (P-core)" / " (E-core)" for a logical processor of a processor with both kinds (Windows' efficiency class: the
// higher, the faster), else "".
const wchar_t* coreKind(int core) {
    static std::vector<BYTE> cls; // by logical processor index (group 0)
    static bool read = false;
    if (!read) {
        read = true;
        ULONG len = 0;
        GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<char> buf(len);
        if (len && GetSystemCpuSetInformation(reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data()), len, &len,
                                              GetCurrentProcess(), 0)) {
            for (ULONG at = 0; at < len;) {
                const auto* i = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + at);
                if (i->Type == CpuSetInformation && i->CpuSet.Group == 0) {
                    const size_t k = i->CpuSet.LogicalProcessorIndex;
                    if (cls.size() <= k) cls.resize(k + 1, 0);
                    cls[k] = i->CpuSet.EfficiencyClass;
                }
                at += i->Size ? i->Size : static_cast<ULONG>(sizeof(SYSTEM_CPU_SET_INFORMATION));
            }
        }
    }
    if (core < 0 || static_cast<size_t>(core) >= cls.size()) return L"";
    const BYTE lo = *std::min_element(cls.begin(), cls.end()), hi = *std::max_element(cls.begin(), cls.end());
    if (lo == hi) return L"";
    return cls[static_cast<size_t>(core)] == hi ? L" (P-core)" : L" (E-core)";
}

// the size of a text in a font, in pixels (a line's height for an empty text)
SIZE textSize(HWND h, HFONT font, const std::wstring& text) {
    SIZE sz{0, 0};
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, font);
    if (text.empty()) {
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        sz.cy = tm.tmHeight;
    } else {
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    }
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return sz;
}

} // namespace

MainWindow::MainWindow(Settings& settings, std::filesystem::path iniFile, AudioIO& audio)
    : settings_(settings), ini_(std::move(iniFile)), base_(settings), audio_(audio),
      editor_(settings, [this](bool listChanged, bool save) { presetsChanged(listChanged, save); }),
      colourEditor_(settings, [this](bool listChanged, bool save) { presetsChanged(listChanged, save); },
                    PresetKind::Colour),
      audioWindow_(settings, audio, [this] {
          // the user changed the audio (a device, Start, Stop): the message of the start at program start no longer
          // holds - it stayed on after the device had been chosen again (client, 2026-10-27); a retry still under way
          // sets it again if it fails again
          if (!message_.empty()) setMessage("");
          save();
          applyAll(); // routes, and deck B's preset when the presets were just linked
          editor_.refresh();
          colourEditor_.refresh();
          recoverAudio(); // a Start that failed: the tries begin now (the countdown shows at once)
      }),
      healthWindow_(healthHistory_,
                    {[this] { return audioWindow_.healthState(); }, [this] { return audioWindow_.healthCounters(); },
                     [this] { audio_.resetHealth(); }, [this] { return settings_.decks; },
                     [this] { return settings_.latencyProbe; },
                     [this](bool on) {
                         settings_.latencyProbe = on;
                         if (on) latencyProbe_.start();
                         else latencyProbe_.stop();
                         save();
                     }}),
      graphs_(settings, audio), analysisWindow_(graphs_),
      spectrumSource_(settings, graphs_, [&audio] { return audio.sampleRate(); },
                      [this](bool saveNow) { // a colour point or band handle dragged, a list, snap switched
                          presetsChanged(false, saveNow);
                          refreshStrip(0);
                          refreshStrip(1);
                          editor_.refresh();
                          colourEditor_.refresh();
                      }),
      spectrumWindow_(spectrumSource_) {
    // a click on a step in the analysis window: that deck's preset (or colour) window on the step's tab
    for (int d = 0; d < 2; ++d) analysisWindow_.setScope(d, settings_.scope[d].input, settings_.scope[d].ms);
    analysisWindow_.onScopeChanged([this](int d, bool input, int ms) {
        settings_.scope[d] = {input, ms};
        save();
    });
    analysisWindow_.onOpenStage([this](int d, const std::string& stage, bool colour) {
        if (colour) colourEditor_.show(hwnd_, settings_.deck[d].colour, d, stage);
        else editor_.show(hwnd_, settings_.deck[d].preset, d, stage);
    });
}

std::wstring MainWindow::Graphs::title(int d) const {
    return (settings_.decks == 1 ? L"Channel" : d == 0 ? L"Deck A" : L"Deck B") + std::wstring(L"   ") +
           fromUtf8(settings_.deck[d].preset);
}

void MainWindow::presetsChanged(bool listChanged, bool saveNow) {
    if (saveNow) save(); // not while a slider is dragged
    if (listChanged) {
        fillPresets(0);
        fillPresets(1);
    }
    applyAll();
}

bool MainWindow::create(HINSTANCE instance, bool hidden, bool testRun) {
    testRun_ = testRun;
    Tooltips::setEnabled(settings_.showTooltips); // before the first window makes its tooltips
    audioWindow_.setTestRun(testRun);
    audioWindow_.setRetryStatus([this] { return retryStatus(); });
    if (settings_.latencyProbe) latencyProbe_.start();
    // the spectra's averaging (the spectrum window's list): both decks' meter panels and the spectrum window
    audio_.setSpectrumAverage(settings_.spectrumAverageMs[0] / 1000.0);
    spectrumSource_.onAverage([this](int ms) {
        audio_.setSpectrumAverage(ms / 1000.0);
        save();
    });
    if (testRun && GetEnvironmentVariableW(L"DJAP_HEALTH_DEMO", nullptr, 0)) fillHealthDemo();
    registerMeterView(instance);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);
    const std::wstring title = L"DJAP (DJ Audio Processor) " + fromUtf8(AP_VERSION_STRING);
    // every window remembers its place in the settings, saved at each change (placement.h)
    placement::setStore(&settings_.windows, [this] { save(); });
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    // a second start may come from a program at a lower integrity level than this one (this one run as administrator):
    // let its "show the window" through (fifth outside review: Windows dropped it and the second start just ended)
    ChangeWindowMessageFilterEx(hwnd_, showMessage(), MSGFLT_ALLOW, nullptr);
    // the first size for the window's DPI; then its remembered place (or centred on the screen)
    RECT r{0, 0, px(1180), px(640)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_); // no menu bar
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    placement::remember(hwnd_, "Main", nullptr);
    applyAll();
    // hidden: the icon in the notification area opens it
    ShowWindow(hwnd_, hidden ? SW_HIDE : placement::showCommand(hwnd_));
    SetTimer(hwnd_, kTimer, kTimerMs, nullptr);
    SetTimer(hwnd_, kStartTimer, kStartRetryMs, nullptr);
    SetTimer(hwnd_, kVolumeTimer, kVolumeMs, nullptr);
    // the icon in the notification area: the small size for this screen (16 px at 100 %, 24 at 150 %, ...)
    if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &iconSmall_))) iconSmall_ = nullptr;
    tray_.add(hwnd_, iconSmall_ ? iconSmall_ : wc.hIconSm, L"DJAP");
    updateTrayTip();
    nextCheck_ = GetTickCount64() + 15000; // the first update check a little after the start
    audioWindow_.setUpdater(&updater_);
    about_.setBeatSource([this] { return &audio_.analysis(0).scopeOut; });
    about_.setUpdates([this] { return audioWindow_.updateView(); }, [this] { checkForUpdate(); },
                      [this] { installUpdate(); });
    return true;
}

const wchar_t* MainWindow::windowClass() { return kClass; }

UINT MainWindow::showMessage() {
    static const UINT msg = RegisterWindowMessageW(L"DJAP.ShowWindow");
    return msg;
}

void MainWindow::openAudioWindow() { audioWindow_.show(hwnd_); }

void MainWindow::startSavedAudio() {
    programStart_ = true; // a chosen output that is not there: the fallback at once
    recoverAudio();
    programStart_ = false;
}

// The chosen devices; with `useFallback` the fallback output in the place of the outputs that are gone.
bool MainWindow::startChosen(bool useFallback, const std::vector<SysDevice>& outputs) {
    // the Windows volume first: the decks take it over without a glide when the stream starts (prepare); else the
    // first 50 ms played at 100 % and glided down - a loud blast at program start (client, 2026-10-25)
    updateVolume();
    startError_.clear();
    AudioApi api = AudioApi::Asio;
    apiFromName(settings_.api, api);
    bool ok = false;
    if (api == AudioApi::Asio) {
        if (audio_.driverOpen() && audio_.driverName() != settings_.driver) audio_.closeDriver();
        ok = (audio_.driverOpen() || audio_.openDriver(settings_.driver, hwnd_, startError_)) &&
             audio_.startDevice(settings_.sampleRate, settings_.bufferSize, asioChannelsFrom(settings_),
                                sysInputsFrom(settings_), startError_);
    } else {
        SysRoute routes[2];
        sysRoutesFrom(settings_, routes);
        if (useFallback) fallbackRoutes(settings_, outputs, routes);
        ok = audio_.startSystem(api, routes, settings_.sampleRate, static_cast<unsigned>(settings_.bufferSize),
                                settings_.exclusive, settings_.driftCorrection, startError_);
    }
    if (ok) applyAll();
    return ok;
}

// Every 5 s (and at program start): the audio that should run and does not (client, 2026-11-05). DeviceRecovery says
// what to do.
std::wstring MainWindow::retryStatus() const {
    if (!settings_.startAudio) return L"";
    if (onFallback_ && audio_.deviceRunning()) return L"On fallback"; // (also when Start went there at once)
    if (!recovery_.trying()) return L"";
    const int left = static_cast<int>(std::ceil(
        recovery_.secondsLeft(static_cast<double>(GetTickCount64()) * 1e-3, settings_.retryMinutes * 60.0)));
    if (left <= 0) return L"Trying";
    wchar_t b[24];
    swprintf(b, 24, L"%d:%02d", left / 60, left % 60);
    return b;
}

void MainWindow::recoverAudio() {
    if (audio_.simulating()) return;
    AudioApi api = AudioApi::Asio;
    apiFromName(settings_.api, api);
    const bool system = api != AudioApi::Asio;
    if (settings_.startAudio && !system && settings_.driver.empty()) {
        if (message_.empty()) setMessage("audio not started: no ASIO driver chosen");
        return;
    }
    DeviceRecovery::Facts f;
    f.wanted = settings_.startAudio;
    f.running = audio_.deviceRunning();
    f.keepTrying = system;
    f.retrySeconds = settings_.retryMinutes * 60.0;
    f.atStart = programStart_;
    SysRoute routes[2];
    sysRoutesFrom(settings_, routes);
    std::vector<SysDevice> inputs, outputs;
    std::string missing, fallbackNote;
    if (system && f.wanted) {
        for (int d = 0; d < 2; ++d)
            if (f.running && audio_.systemRunning() && audio_.sys().info(d).running &&
                audio_.sys().info(d).output != routes[d].output)
                f.onFallback = true;
        if (!f.running || f.onFallback) { // (not every 5 s while all is well)
            std::string err;
            SysAudio::devices(api, inputs, outputs, err);
            auto listed = [&](const std::string& name) {
                return std::any_of(outputs.begin(), outputs.end(), [&](const SysDevice& o) { return o.name == name; });
            };
            for (const SysRoute& r : routes)
                if (!r.output.empty() && !listed(r.output)) missing = r.output;
            f.chosenOutputsThere = missing.empty();
            const std::string& fb = settings_.fallbackOutput;
            bool usable = !fb.empty() && listed(fb) && !missing.empty();
            for (const SysRoute& r : routes)
                if (fb == r.input) { // a virtual cable's two ends carry one name: DJAP would hear itself
                    if (usable) fallbackNote = "; the fallback output is this input's own device: not used";
                    usable = false;
                }
            f.fallbackUsable = usable;
        }
    }
    onFallback_ = f.onFallback;
    const double now = static_cast<double>(GetTickCount64()) * 1e-3;
    const std::string gone = missing.empty() ? std::string("audio not running") : "the output \"" + missing + "\" is gone";
    switch (recovery_.tick(now, f)) {
    case DeviceRecovery::Action::None:
        if (f.onFallback) {
            const std::string text = gone + ": playing on \"" + settings_.fallbackOutput + "\" until it is back";
            if (message_ != text) setMessage(text);
        }
        break;
    case DeviceRecovery::Action::Retry:
        if (startChosen(false, outputs)) {
            setMessage("");
        } else {
            const int left = static_cast<int>(std::ceil(recovery_.secondsLeft(now, f.retrySeconds)));
            std::string text = gone + ", trying again";
            if (left > 0 && f.fallbackUsable)
                text += " for " + std::to_string(left / 60) + ":" + (left % 60 < 10 ? "0" : "") + std::to_string(left % 60) +
                        ", then on \"" + settings_.fallbackOutput + "\"";
            setMessage(text + ": " + startError_ + fallbackNote);
        }
        break;
    case DeviceRecovery::Action::StartFallback:
        if (startChosen(true, outputs)) {
            onFallback_ = true; // the countdown says so at once
            setMessage(gone + ": playing on \"" + settings_.fallbackOutput + "\" until it is back");
        } else {
            setMessage(gone + "; the fallback output did not start either: " + startError_);
        }
        break;
    case DeviceRecovery::Action::ReturnToChosen:
        if (startChosen(false, outputs)) {
            setMessage("");
        } else { // listed, but it cannot be opened yet: the fallback goes on
            recovery_.returnFailed(now);
            for (const SysRoute& r : routes)
                if (!r.output.empty()) outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [&](const SysDevice& o) { return o.name == r.output; }), outputs.end());
            startChosen(true, outputs);
        }
        break;
    case DeviceRecovery::Action::GiveUp:
        setMessage("audio not started: " + startError_);
        break;
    }
}

LRESULT CALLBACK MainWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (tray_.taskbarCreated(msg)) return 0; // Explorer restarted: the icon is back
    if (msg == showMessage() && msg != 0) { // a second start of the program
        fromTray();
        return 0;
    }
    if (msg == Updater::message() && msg != 0) {
        updateEvent(wp);
        return 0;
    }
    LRESULT themed = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, themed)) return themed; // brushed aluminium, 3D buttons
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        createControls();
        setFonts();
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) toTray(); // minimised = into the notification area
        else layout();
        return 0;
    case TrayIcon::kMessage:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            fromTray();
            break;
        case NIN_BALLOONUSERCLICK: // the message about a new version: About, where Install update is
            if (updater_.state() == Updater::State::Available) about_.show(hwnd_, ini_);
            break;
        case WM_CONTEXTMENU:
            trayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        default:
            break;
        }
        return 0;
    case WM_GETMINMAXINFO: {
        // the smallest window that still shows every stage row and a readable spectrum (client, 2026-10-26); one
        // channel narrower than two decks; larger is always allowed
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
        int statusH = px(22);
        if (status_) {
            RECT sr;
            GetWindowRect(status_, &sr);
            statusH = sr.bottom - sr.top;
        }
        RECT r{0, 0, settings_.decks == 1 ? px(kMinWidthOne) : px(kMinWidthTwo),
               meterTop() + meterViewMinHeight(dpi_) + px(12) + statusH};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        setFonts();
        tips_.setDpi(dpi_);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        layout();
        return 0;
    }
    case WM_COMMAND:
        command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_TIMER:
        if (wp == kStartTimer) {
            recoverAudio();
            return 0;
        }
        if (wp == kVolumeTimer) {
            // (not followed: the volume is only shown in the status bar - every fourth look is enough for that)
            static unsigned looks = 0;
            if (settings_.followsVolume() || looks++ % 4 == 0) updateVolume();
            return 0;
        }
        if (wp == kTimer) {
            audio_.poll();
            updateMeters();
            analysisWindow_.refresh();
            spectrumWindow_.refresh();
            sampleHealth();
            if (ticks_++ % 10 == 0) {
                measureCpu();
                updateStatus();
                audioWindow_.refresh();
                about_.refresh(); // the download's percentage
                // the daily update check (a test run asks only a test server of its own settings)
                if (settings_.checkUpdates && GetTickCount64() >= nextCheck_ && !updater_.busy() &&
                    (!testRun_ || !settings_.updateUrl.empty()))
                    checkForUpdate();
            }
        }
        return 0;
    case WM_CLOSE: // the close box hides the window, the audio goes on; Shutdown really closes
        if (quitting_) DestroyWindow(hwnd_);
        else toTray();
        return 0;
    case WM_CONTEXTMENU: { // a right-click anywhere on the window (its controls pass it up): Setup, Shutdown
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.x == -1 && pt.y == -1) { // from the keyboard: at the window's top left
            pt = {px(40), px(40)};
            ClientToScreen(hwnd_, &pt);
        }
        windowMenu(pt.x, pt.y);
        return 0;
    }
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION: // Windows logs off or shuts down, or setup replaces the program: save and end
        if (wp) {
            quitting_ = true;
            DestroyWindow(hwnd_);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, kTimer);
        KillTimer(hwnd_, kStartTimer);
        KillTimer(hwnd_, kVolumeTimer);
        tray_.remove();
        if (iconSmall_) DestroyIcon(iconSmall_);
        iconSmall_ = nullptr;
        winVolume_.close(); // COM objects go before the apartment ends (main)
        // StartAudio keeps the user's last Start / Stop: a device that was not there (yet) when Windows started does
        // not switch the start at program start off
        save();
        if (font_) DeleteObject(font_);
        if (bold_) DeleteObject(bold_);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::createControls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    for (int d = 0; d < 2; ++d) {
        Strip& s = strips_[d];
        s.title = make(L"STATIC", deckName(d).c_str(), SS_LEFT, 0);
        s.route = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX | SS_NOTIFY, 0);
        s.routeOut = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX | SS_NOTIFY, 0);
        // checked = bypassed: "Bypass On", red (theme::setBypass)
        s.processing = make(L"BUTTON", L"Bypass Off",BS_OWNERDRAW | WS_TABSTOP, idOf(d, iProcessing));
        // "Analysis", the window's name (client, 2026-10-30; was "Graphs")
        s.graphs = make(L"BUTTON", L"Analysis", BS_OWNERDRAW | WS_TABSTOP, idOf(d, iGraphs));
        s.presetLabel = make(L"STATIC", L"Loudness", SS_LEFT | SS_CENTERIMAGE, 0);
        s.preset = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idOf(d, iPreset));
        s.edit = make(L"BUTTON", L"Edit...", BS_OWNERDRAW | WS_TABSTOP, idOf(d, iEdit));
        s.colourLabel = make(L"STATIC", L"EQ", SS_LEFT | SS_CENTERIMAGE, 0);
        s.colour = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idOf(d, iColour));
        s.editColour = make(L"BUTTON", L"Edit...", BS_OWNERDRAW | WS_TABSTOP, idOf(d, iEditColour));
        for (HWND b : {s.processing, s.graphs, s.edit, s.editColour}) theme::makeButton(b);
        for (HWND c : {s.preset, s.colour}) theme::makeCombo(c);
        s.meter = createMeterView(hwnd_, idOf(d, iMeter));
        fillPresets(d);
    }
    // (its own tooltip shows the whole line when it does not fit - tenth outside review: a message at its end was cut)
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | SBARS_TOOLTIPS, 0, 0,
                              0, 0, hwnd_, nullptr, inst, nullptr);

    tips_.create(hwnd_);
    for (int d = 0; d < 2; ++d) {
        Strip& s = strips_[d];
        for (HWND h : {s.route, s.routeOut})
            tips_.add(h, L"Where this deck's audio comes from (In) and where it goes (Out). Change it in Setup (right-click "
                         L"the window). With linked presets (Setup) deck A's controls count for both decks.");
        tips_.add(s.preset,
                  L"The loudness preset this deck uses: the target loudness and every stage's values. Shipped: Gentle -18, "
                  L"Light -16, Medium -14, Loud -12, Very Loud -10 LUFS; FM Station -13 (dense, the sound of FM radio), DAB+ "
                  L"-23 (EBU R 128, wide dynamics), Web Stream -16 (-2 dBTP for MP3 / AAC); your own follow. A new choice is "
                  L"heard at once, without a click. With linked presets (Setup) both decks change.");
        tips_.add(s.processing,
                  L"Bypass Off (green): the deck runs through its chain.\nBypass On (red): the input goes out "
                  L"unchanged (bit-exact), delayed by the same limiter look-ahead, so switching never shifts the sound "
                  L"in time. Saved per deck; with linked presets deck A's switch counts for both.");
        tips_.add(s.graphs,
                  L"Opens the analysis window: for each deck the signal chain step "
                  L"by step, the compressor's curve, loudness and gains of the last 60 s, the goniometer with "
                  L"correlation and stereo width.");
        tips_.add(s.colour,
                  L"The EQ this deck uses, on top of its loudness preset: tone (bass, low-mid, presence, treble), auto "
                  L"tone (each song's balance pulled towards a modern reference), sub bass and a contour (more bass and highs for quiet listening). "
                  L"Neutral changes nothing; Warm, Bass+, Less mid, Bright, Vocal: a fixed tone; Oldies: auto tone "
                  L"and a little sub bass for thin old recordings; Auto: only the auto tone; Quiet: the contour "
                  L"for quiet listening. Your own EQs follow.");
        tips_.add(s.editColour, L"Opens the EQ window: every value of this deck's EQ; Save as, Rename, Delete "
                                L"and Reset.");
        tips_.add(s.edit, L"Opens the loudness window: the nine stage switches and every value of this deck's loudness preset "
                          L"with its unit, range and meaning; Save as, Rename, Delete and Reset.");
    }
}

void MainWindow::setFonts() {
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = lf.lfHeight * 13 / 10;
    bold_ = CreateFontIndirectW(&lf);
    auto set = [&](HWND h, HFONT f) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    for (Strip& s : strips_) {
        set(s.title, bold_);
        for (HWND h : {s.route, s.routeOut, s.presetLabel, s.preset, s.colourLabel, s.colour, s.processing, s.graphs, s.edit,
                       s.editColour})
            set(h, font_);
    }
    set(status_, font_);
}

void MainWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status_, &sr);
    const int statusH = sr.bottom - sr.top;
    const int m = px(12), gap = px(6), rowH = px(28);
    const int strips = settings_.decks; // one channel: deck A's strip over the whole width
    const int stripW = strips == 1 ? rc.right - 2 * m : (rc.right - 3 * m) / 2;
    layoutDecks_ = strips;
    for (int d = 0; d < strips; ++d) {
        Strip& s = strips_[d];
        const int x0 = m + d * (stripW + m);
        int y = m;
        // Deck A  route .......... [Analysis] [Bypass]
        const int bypassW = px(90), graphsW = px(90);
        // the route as two lines, In and Out, right after the title (client, 2026-10-27: "in and out on 2 lines, on a
        // small screen it does not fit - and more to the left")
        int titleW = 0; // the widest title, so both strips' routes start at the same place
        for (const wchar_t* t : {L"Deck A", L"Deck B", L"Channel"})
            titleW = std::max(titleW, static_cast<int>(textSize(hwnd_, bold_, t).cx) + px(4));
        const int routeX = x0 + titleW + px(10), lineH = textSize(hwnd_, font_, L"").cy;
        const int routeW = std::max(0, x0 + stripW - bypassW - graphsW - 2 * gap - routeX);
        const int routeY = y + (rowH - 2 * lineH) / 2;
        MoveWindow(s.title, x0, y - px(5), titleW, rowH, TRUE); // a little higher (client: "Deck A and B a bit up")
        MoveWindow(s.route, routeX, routeY, routeW, lineH, TRUE);
        MoveWindow(s.routeOut, routeX, routeY + lineH, routeW, lineH, TRUE);
        MoveWindow(s.graphs, x0 + stripW - bypassW - gap - graphsW, y, graphsW, rowH, TRUE);
        MoveWindow(s.processing, x0 + stripW - bypassW, y, bypassW, rowH, TRUE);
        y += rowH + gap;
        // Preset [....] [Edit...]  Colour [....] [Edit...]
        const int labelW = px(48), colourLabelW = px(26), editW = px(70); // "EQ" (was "Colour", 50 px)
        const int combos = stripW - labelW - colourLabelW - 2 * editW - 5 * gap - px(10);
        const int presetW = combos / 2, colourW = combos - presetW;
        int x = x0;
        MoveWindow(s.presetLabel, x, y, labelW, rowH, TRUE);
        x += labelW + gap;
        MoveWindow(s.preset, x, y, presetW, px(300), TRUE);
        x += presetW + gap;
        MoveWindow(s.edit, x, y, editW, rowH, TRUE);
        x += editW + gap + px(10);
        MoveWindow(s.colourLabel, x, y, colourLabelW, rowH, TRUE);
        x += colourLabelW + gap;
        MoveWindow(s.colour, x, y, colourW, px(300), TRUE);
        x += colourW + gap;
        MoveWindow(s.editColour, x, y, editW, rowH, TRUE);
        y += rowH + gap; // = meterTop()
        MoveWindow(s.meter, x0, y, stripW, rc.bottom - statusH - m - y, TRUE);
    }
    const int parts[1] = {-1};
    SendMessageW(status_, SB_SETPARTS, 1, reinterpret_cast<LPARAM>(parts));
    // the whole window again: the metal is drawn for the new size, and a moved button leaves no piece of itself
    // behind (client, 2026-10-26: after a resize part of the old Bypass button stayed beside the new one)
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void MainWindow::fillPresets(int d) {
    HWND c = strips_[d].preset;
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (const Preset& p : settings_.presets) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(p.name).c_str()));
    c = strips_[d].colour;
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (const Preset& p : settings_.colours) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(p.name).c_str()));
}

void MainWindow::command(int id, int code) {
    if (id == mExit) {
        quitting_ = true;
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }
    if (id == mOpen) {
        fromTray();
        return;
    }
    if (id == mAbout) {
        about_.show(hwnd_, ini_);
        return;
    }

    if (id == mAudio) {
        audioWindow_.show(hwnd_);
        return;
    }
    if (id == mHealth) {
        healthWindow_.show(hwnd_);
        return;
    }
    if (id == mAnalysis) {
        analysisWindow_.show(hwnd_);
        return;
    }
    if (id == mSpectrum) {
        spectrumWindow_.show(hwnd_, 0);
        return;
    }
    if (id < 1000) return;
    const int d = (id - 1000) / 100, item = (id - 1000) % 100;
    if (d < 0 || d > 1) return;
    Strip& s = strips_[d];
    if (item == iPreset && code == CBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(s.preset, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && static_cast<size_t>(sel) < settings_.presets.size()) {
            settings_.deck[d].preset = settings_.presets[static_cast<size_t>(sel)].name;
            if (linked()) settings_.deck[1 - d].preset = settings_.deck[d].preset; // both decks
            save();
            applyAll(); // the other strip's note ("also used by ...") changes too
            editor_.refresh();
        }
    } else if (item == iColour && code == CBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(s.colour, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && static_cast<size_t>(sel) < settings_.colours.size()) {
            settings_.deck[d].colour = settings_.colours[static_cast<size_t>(sel)].name;
            if (linked()) settings_.deck[1 - d].colour = settings_.deck[d].colour; // both decks
            save();
            applyAll();
            colourEditor_.refresh();
        }
    } else if (item == iEditColour && code == BN_CLICKED) {
        colourEditor_.show(hwnd_, settings_.deck[d].colour, d);
    } else if (item == iGraphs && code == BN_CLICKED) { // the analysis window
        analysisWindow_.show(hwnd_);
    } else if (item == iProcessing && code == BN_CLICKED) {
        settings_.deck[d].processing = !settings_.deck[d].processing; // red "Bypass On" = bypassed
        save();
        applyAll(); // linked: deck A's switch counts for both decks
    } else if (item == iMeter && code == kMeterOpenRow + kMeterSpectrum) { // the spectrum: the spectrum window
        spectrumWindow_.show(hwnd_, d);
    } else if (item == iMeter && code >= kMeterOpenRow && code < kMeterOpenRow + kMeterTargets) { // a row or bar clicked
        editor_.show(hwnd_, settings_.deck[d].preset, d, meterStage(code - kMeterOpenRow));
    } else if (item == iEdit && code == BN_CLICKED) {
        editor_.show(hwnd_, settings_.deck[d].preset, d);
    }
}

void MainWindow::applyAll() {
    if (layoutDecks_ != settings_.decks) { // two decks <-> one channel: the minimum size changed too
        RECT w;
        GetWindowRect(hwnd_, &w);
        if (!IsZoomed(hwnd_))
            SetWindowPos(hwnd_, nullptr, 0, 0, w.right - w.left, w.bottom - w.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
    }
    if (linked()) { // one set of controls, deck A's: deck B follows its colour and processing switch
        settings_.deck[1].colour = settings_.deck[0].colour;
        settings_.deck[1].processing = settings_.deck[0].processing;
    }
    applyDeck(0);
    applyDeck(1);
    updateStatus();
}

void MainWindow::applyDeck(int d) {
    DeckControl c;
    c.lookMs = settings_.lookAheadMs;
    c.oversampling = settings_.oversampling;
    c.mono = settings_.mono;
    c.processing = settings_.deck[d].processing;
    audio_.setDeck(d, settings_.deckValues(d), c); // its preset with its colour
    refreshStrip(d);
}

void MainWindow::refreshStrip(int d) {
    Strip& s = strips_[d];
    const std::string& name = settings_.deck[d].preset;
    for (size_t i = 0; i < settings_.presets.size(); ++i)
        if (settings_.presets[i].name == name) SendMessageW(s.preset, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    for (size_t i = 0; i < settings_.colours.size(); ++i)
        if (settings_.colours[i].name == settings_.deck[d].colour) SendMessageW(s.colour, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    theme::setBypass(s.processing, !settings_.deck[d].processing);
    // one channel: deck B's strip is not there; linked presets: one set of controls, on deck A
    const bool gone = d == 1 && settings_.decks == 1, single = gone || (d == 1 && linked());
    for (HWND h : {s.presetLabel, s.preset, s.colourLabel, s.colour, s.edit, s.editColour, s.processing, s.graphs})
        ShowWindow(h, single ? SW_HIDE : SW_SHOW);
    for (HWND h : {s.title, s.route, s.routeOut, s.meter}) ShowWindow(h, gone ? SW_HIDE : SW_SHOW);
    if (d == 0) SetWindowTextW(s.title, settings_.decks == 1 ? L"Channel" : L"Deck A");
    std::wstring in, out; // each side names its API
    if (settings_.api == "ASIO") {
        const int inPair = d == 0 ? settings_.inputA : settings_.inputB;
        const int outPair = d == 0 ? settings_.outputA : settings_.outputB;
        const SysInputs sysIn = sysInputsFrom(settings_);
        if (!sysIn.device[d].empty()) // an input device of another API (drift-corrected)
            in = fromUtf8(std::string(apiName(sysIn.api)) + ": " + sysIn.device[d]);
        else
            in = L"ASIO: inputs " + std::to_wstring(inPair) + L"-" + std::to_wstring(inPair + 1);
        out = L"ASIO: outputs " + std::to_wstring(outPair) + L"-" + std::to_wstring(outPair + 1);
    } else if (settings_.inDevice[d].empty() || settings_.outDevice[d].empty()) {
        in = fromUtf8(settings_.api) + L": no devices (not used)";
    } else {
        in = fromUtf8(settings_.api + ": " + settings_.inDevice[d]);
        // what runs: on the fallback output that one
        const bool other = audio_.systemRunning() && audio_.sys().info(d).running && audio_.sys().info(d).output != settings_.outDevice[d];
        out = fromUtf8(settings_.api + ": " + (other ? audio_.sys().info(d).output + " (fallback)" : settings_.outDevice[d]));
    }
    if (d == 1 && linked()) (out.empty() ? in : out) += L"   (linked to deck A)"; // deck B has no controls: say why
    SetWindowTextW(s.route, out.empty() ? in.c_str() : (L"In:  " + in).c_str());
    SetWindowTextW(s.routeOut, out.empty() ? L"" : (L"Out: " + out).c_str());
}

void MainWindow::updateMeters() {
    for (int d = 0; d < 2; ++d) {
        MeterViewData data;
        data.m = audio_.meters(d);
        const PresetValues v = settings_.valuesOf(settings_.deck[d].preset);
        data.target = v[pTarget];
        data.ceiling = v[pLimCeiling];
        data.levRange = v[pLevRange];
        data.inputGain = settings_.deck[d].processing ? v[pInputGain] : 0.0; // bypassed: nothing is applied
        if (data.m.bands == 0) data.m.bands = static_cast<int>(v[pDensBands]); // before the first meter record
        for (int k = 0; k < kStageCount; ++k) data.stageOn[k] = v[kStageParam[k]] >= 0.5;
        data.processing = settings_.deck[d].processing;
        data.audio = audio_.audioActive();
        const DeckAnalysis& an = audio_.analysis(d);
        data.spectrum = data.audio && data.m.live && an.specOut.frames() > 0;
        data.specIn = an.specIn.points();
        data.specOut = an.specOut.points();
        for (int k = 0; k < kMaxSplits; ++k) data.splits[k] = v[static_cast<size_t>(pDensSplit1 + k)];
        data.colourValues = settings_.deckValues(d);
        for (int k = 0; k < 4; ++k) data.autoDb[k] = data.m.autoToneDb[k];
        data.fs = audio_.sampleRate() > 0 ? audio_.sampleRate() : 48000.0;
        setMeterView(strips_[d].meter, data);
    }
}

// Once a second: the audio's health into the history of the Health window's graphs.
void MainWindow::sampleHealth() {
    const ULONGLONG now = GetTickCount64();
    if (healthLast_ != 0 && now - healthLast_ < 1000) return;
    const double seconds = healthLast_ != 0 ? static_cast<double>(now - healthLast_) * 1e-3 : 1.0;
    healthLast_ = now;
    HealthReading r;
    r.running = audio_.audioActive();
    r.probeOn = latencyProbe_.running();
    r.probeMs = latencyProbe_.takePeakMs();
    if (r.running) {
        const EngineHealth h = audio_.health();
        r.peakPercent = audio_.takePeakPercent();
        r.lateMs = std::max(0.0, audio_.takeLateMs());
        r.periodMs = audio_.periodMs();
        r.busySeconds = h.busySeconds;
        r.overHalf = h.overHalf;
        r.late = h.late;
        r.refused = h.refused;
#if AP_HAVE_ASIO
        r.resyncs = audio_.host().resyncs();
        r.overloads = audio_.host().overloads();
#endif
        for (int d = 0; d < 2; ++d) {
            if (!audio_.asyncInput(d)) continue;
            const AsyncInputState a = audio_.asyncState(d);
            r.input[d].on = a.active; // (filling up after the start or an underrun: no drift or fill to speak of)
            r.input[d].driftPpm = a.driftPpm;
            r.input[d].fillMs = a.fillMs;
            r.input[d].targetMs = a.targetMs;
            r.input[d].underruns = a.underruns;
            r.input[d].overruns = a.overruns;
        }
    }
    healthHistory_.add(r, seconds);
    healthWindow_.refresh();
}

void MainWindow::fillHealthDemo() {
    HealthReading r;
    r.running = true;
    r.probeOn = true;
    r.periodMs = 2.9;
    for (int t = 0; t < 900; ++t) {
        r.probeMs = 0.15 + 0.05 * std::sin(t * 0.2) + (t % 131 == 0 ? 0.9 : 0.0);
        r.lateMs = 0.1 + 0.05 * std::sin(t * 0.3) + (t == 640 ? 3.4 : 0.0);
        r.peakPercent = 18.0 + 6.0 * std::sin(t * 0.05) + (t % 97 == 0 ? 25.0 : 0.0);
        r.busySeconds += 0.06 + 0.01 * std::sin(t * 0.03);
        r.overHalf += t == 400 || t == 640 ? 1 : 0;
        r.late += t == 640 ? 1 : 0;
        for (int d = 0; d < 2; ++d) {
            r.input[d].on = true;
            r.input[d].driftPpm = (d == 0 ? -29.0 : 12.0) + 1.5 * std::sin(t * 0.01 + d);
            r.input[d].targetMs = d == 0 ? 13.2 : 9.5;
            r.input[d].fillMs = r.input[d].targetMs + 0.6 * std::sin(t * 0.04 + d);
            r.input[d].underruns += d == 1 && t == 640 ? 1 : 0;
        }
        healthHistory_.add(r, 1.0);
    }
}

// The program's CPU use (all its threads, the audio included) as a share of the whole computer, as Task Manager shows
// it: the process's kernel + user time against the wall time and the number of logical processors, over at least 1 s.
void MainWindow::measureCpu() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
    GetSystemTimeAsFileTime(&now);
    auto u64 = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const unsigned long long busy = u64(kernel) + u64(user), wall = u64(now);
    if (cpuLastWall_ != 0 && wall - cpuLastWall_ < 10000000ull) return; // 100 ns units: a second at least
    // the audio thread on its own (client, 2026-10-28: "can you show the core DJAP runs on"): its processing time
    // against the wall time - the share of the one core it runs on - and that core (it may move between cores)
    const bool running = audio_.deviceRunning() || audio_.systemRunning();
    const EngineHealth h = audio_.health();
    if (running && cpuLastWall_ != 0 && wall > cpuLastWall_ && audioLastBusy_ >= 0.0 && h.busySeconds >= audioLastBusy_)
        audioPercent_ = 100.0 * (h.busySeconds - audioLastBusy_) / (static_cast<double>(wall - cpuLastWall_) * 1e-7);
    else if (!running)
        audioPercent_ = -1.0;
    audioLastBusy_ = running ? h.busySeconds : -1.0;
    audioCore_ = running ? h.core : -1;
    if (cpuLastWall_ != 0 && wall > cpuLastWall_) {
        const DWORD cpus = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        cpuPercent_ = 100.0 * static_cast<double>(busy - cpuLastBusy_) / static_cast<double>(wall - cpuLastWall_) / cpus;
    }
    cpuLastBusy_ = busy;
    cpuLastWall_ = wall;
}

// The status bar (client, 2026-10-27): the Windows volume and the program's CPU use; the audio's state is in the
// Setup window. A message that needs attention (an error, a start that is retried, a new version) comes after them.
void MainWindow::updateStatus() {
    if (!status_) return;
    // the audio runs (however it was started): a message that it did not start is past
    if (!message_.empty() && (audio_.deviceRunning() || audio_.systemRunning())) message_.clear();
    std::wstring notes; // also shown in the Setup window
    if (!message_.empty()) notes += fromUtf8(message_);
    if (!saveError_.empty()) notes += (notes.empty() ? L"" : L"\n") + fromUtf8(saveError_);
    audioWindow_.setNotes(notes);
    wchar_t b[160];
    if (!winVolume_.valid()) swprintf(b, 160, L"Windows volume: unknown");
    else if (winVolume_.muted()) swprintf(b, 160, L"Windows volume: muted");
    else swprintf(b, 160, L"Windows volume %.0f %%", winVolume_.percent());
    std::wstring line = b;
    if (cpuPercent_ >= 0.0) {
        swprintf(b, 160, L"   |   CPU %.1f %%", cpuPercent_);
        line += b;
    }
    if (audioPercent_ >= 0.0 && audioCore_ >= 0) { // the audio thread: its share of the core it runs on
        swprintf(b, 160, L"   |   audio %.1f %% of core %d%s", audioPercent_, audioCore_, coreKind(audioCore_));
        line += b;
    }
    if (!audio_.deviceError().empty()) line += L"   |   " + fromUtf8(audio_.deviceError());
    if (!message_.empty()) line += L"   |   " + fromUtf8(message_);
    if (updater_.state() == Updater::State::Available)
        line += L"   |   DJAP " + fromUtf8(updater_.release().version) + L" is available (right-click: About DJAP)";
    if (!saveError_.empty()) line += L"   |   " + fromUtf8(saveError_);
    // ten minutes of silence on every deck while the audio runs
    const ULONGLONG now = GetTickCount64();
    bool silent = audio_.deviceRunning() || audio_.systemRunning();
    for (int d = 0; d < settings_.decks && silent; ++d)
        silent = audio_.meters(d).inM < -70.0 && audio_.meters(d).outM < -70.0;
    if (!silent) quietSince_ = now;
    if (silent && now - quietSince_ >= 10ull * 60 * 1000) line += L"   |   Is the party over?";
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(status_, SB_SETTIPTEXTW, 0, reinterpret_cast<LPARAM>(line.c_str())); // shown only when cut off
    updateTrayTip();
}

// The tray icon's tooltip: whether the audio runs and what each deck uses.
void MainWindow::updateTrayTip() {
    const bool running = audio_.simulating() || audio_.systemRunning() || audio_.deviceRunning();
    std::wstring t = running ? L"DJAP - audio running" : L"DJAP - audio stopped";
    const int decks = settings_.decks == 1 ? 1 : 2;
    for (int d = 0; d < decks; ++d) {
        const DeckSettings& k = settings_.deck[d];
        t += L"\n" + (decks == 1 ? std::wstring(L"Loudness") : deckName(d)) + L": ";
        if (!k.processing) t += L"bypass";
        else t += fromUtf8(k.preset) + (k.colour == "Neutral" ? L"" : L", " + fromUtf8(k.colour));
    }
    tray_.setTip(t);
}

void MainWindow::toTray() {
    ShowOwnedPopups(hwnd_, FALSE); // the Audio, preset and analysis windows go too
    ShowWindow(hwnd_, SW_HIDE);
    if (!settings_.trayHintShown) { // once: where the program went
        tray_.balloon(L"DJAP keeps running",
                      L"The audio goes on. Click the icon here to open the window; right-click it to shut DJAP down.");
        settings_.trayHintShown = true;
        save();
    }
}

void MainWindow::fromTray() {
    ShowWindow(hwnd_, placement::showCommand(hwnd_));
    ShowOwnedPopups(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);
}

void MainWindow::checkForUpdate() {
    nextCheck_ = GetTickCount64() + 24ull * 3600 * 1000; // once a day
    // [Updates] Url (a local test server) counts only in a test run (--ini / --simulate): in the user's own settings it
    // would let anything that can edit the file point the updater at a server of its own (outside review)
    updater_.check(hwnd_, testRun_ ? settings_.updateUrl : std::string(), AP_VERSION_STRING);
    audioWindow_.refresh();
    about_.refresh();
}

void MainWindow::installUpdate() {
    HWND owner = about_.hwnd() ? about_.hwnd() : hwnd_;
    if (audio_.simulating()) { // a test's simulation is never ended by an update
        placement::messageBox(owner, L"End the simulation first: the update closes DJAP.", L"Update",
                              MB_OK | MB_ICONINFORMATION);
        return;
    }
    // while the audio runs (client, 2026-10-26: "after the online update, start the audio again automatically"): it
    // stops when the setup starts and the new version starts it again (StartAudio stays on, startSavedAudio)
    if (audio_.deviceRunning() &&
        placement::messageBox(owner,
                              L"The audio stops while DJAP updates - usually less than half a minute - and starts again "
                              L"by itself with the new version.\n\nInstall the update now?",
                              L"Update", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    updater_.download(hwnd_);
}

void MainWindow::updateEvent(WPARAM what) {
    audioWindow_.refresh();
    about_.refresh();
    updateStatus();
    if (what == 0 && updater_.state() == Updater::State::Available) { // once per new version
        const std::string v = updater_.release().version;
        if (v != notified_) {
            notified_ = v;
            tray_.balloon(L"DJAP " + fromUtf8(v) + L" is available",
                          L"Click here (or right-click the window: About DJAP) to install it. Nothing is installed "
                          L"until you click Install update.");
        }
    }
    if (what == 1 && updater_.state() == Updater::State::Ready) {
        // running audio stops when the program ends (fade-out); StartAudio stays on, so the new version starts it
        std::string err;
        if (updater_.launch(testRun_ ? ini_ : std::filesystem::path(), err)) { // the setup waits for this program to end, installs, starts it again
            quitting_ = true;
            DestroyWindow(hwnd_);
            return;
        }
        placement::messageBox(about_.hwnd() ? about_.hwnd() : hwnd_, fromUtf8(err).c_str(), L"Update",
                              MB_OK | MB_ICONWARNING);
    }
    // a download that failed while About was closed: said in a balloon (seventh outside review: nothing showed it)
    if (what == 1 && updater_.state() == Updater::State::Failed && !about_.hwnd())
        tray_.balloon(L"The DJAP update could not be downloaded",
                      fromUtf8(updater_.error()) + L"\nRight-click the window: About DJAP to try again.");
}

// The main window's right-click menu (client, 2026-10-25: no menu bar; "Exit and Setup"; 2026-10-26: Exit is
// called Shutdown; 2026-10-27: About moved here from a button in the Setup window).
void MainWindow::windowMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, mAudio, L"&Setup...");
    AppendMenuW(m, MF_STRING, mHealth, L"&Health...");
    AppendMenuW(m, MF_STRING, mAbout, L"&About DJAP...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, mExit, L"Shut&down");
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON, x, y, hwnd_, nullptr);
    DestroyMenu(m);
}

void MainWindow::trayMenu(int x, int y) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, mOpen, L"&Open DJAP");
    AppendMenuW(m, MF_STRING, mAudio, L"&Setup...");
    AppendMenuW(m, MF_STRING, mHealth, L"&Health...");
    AppendMenuW(m, MF_STRING, mAbout, L"&About DJAP...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, mExit, L"Shut&down");
    SetMenuDefaultItem(m, mOpen, FALSE);
    SetForegroundWindow(hwnd_); // else the menu does not close when clicked elsewhere
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// The outputs follow Windows' volume (the volume keys) when chosen. A deck whose input is the default playback
// device itself - a virtual cable that Windows turns down in software - gets that volume undone at its input, so the
// processing sees the music's own level and the volume counts once.
void MainWindow::updateVolume() {
    const bool follow = settings_.followsVolume();
    winVolume_.poll(); // also when the outputs do not follow it: the status bar shows it (client, 2026-10-27)
    const bool on = follow && winVolume_.valid();
    const SysInputs sysIn = sysInputsFrom(settings_);
    for (int d = 0; d < 2; ++d) {
        double out = 1.0, in = 1.0;
        bool inputMuted = false;
        if (on) {
            out = winVolume_.gain();
            const std::string& input = settings_.api == "ASIO" ? sysIn.device[d] : settings_.inDevice[d];
            if (winVolume_.software() && !input.empty() && input == winVolume_.device()) {
                // the volume the device is set to, also while it is muted: the factor stays what the music needs when
                // it comes back (it went to 1 for the mute and jumped back after it), and the deck is told that its
                // input is silent because of the mute - not because the music stopped (DeckProcessor::setVolume)
                const double level = winVolume_.level();
                if (level > 0.0) in = std::min(kMaxInputVolume, 1.0 / level);
                inputMuted = out <= 0.0;
            }
        }
        audio_.setVolume(d, out, in, inputMuted);
    }
    // the status bar shows the volume: updated at once when it changes
    const double shown = !winVolume_.valid() ? -2.0 : winVolume_.muted() ? -1.0 : std::round(winVolume_.percent());
    if (shown != shownVolume_) {
        shownVolume_ = shown;
        updateStatus();
    }
}

void MainWindow::setMessage(const std::string& text) {
    message_ = text;
    updateStatus();
}

void MainWindow::holdSaves(const std::string& why) {
    holdSaves_ = why;
    save(); // shows it
}

void MainWindow::save() {
    if (!holdSaves_.empty()) {
        saveError_ = holdSaves_;
        updateStatus();
        return;
    }
    // merged with what a plug-in saved in the same file meanwhile (its own presets and colours)
    auto names = [this] {
        std::vector<std::string> n;
        for (const Preset& p : settings_.presets) n.push_back(p.name);
        for (const Preset& p : settings_.colours) n.push_back("\x01" + p.name);
        return n;
    };
    const std::vector<std::string> before = names();
    {
        const SettingsLock lock; // a plug-in saving at the same moment waits (settingslock.h)
        saveError_ = settings_.saveMerged(ini_, base_, Settings::SaveRole::Owner)
                         ? std::string()
                         : "settings not saved: " + toUtf8(ini_.wstring());
    }
    if (names() != before) { // a plug-in's new, renamed or deleted presets: the lists show them
        fillPresets(0);
        fillPresets(1);
        refreshStrip(0);
        refreshStrip(1);
        editor_.refresh();
        colourEditor_.refresh();
    }
    updateStatus();
}

} // namespace ap
