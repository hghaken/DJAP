#include "audiodlg.h"

#include "appstate.h"
#include "version.h"
#include "placement.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <string>

namespace ap {

namespace {

const wchar_t* const kClass = L"APAudioWindow";
enum Id { idDriver = 10, idOpen, idClose, idPanel, idInA, idOutA, idInB, idOutB, idRate, idBuffer, idStart, idStop,
          idResetHealth, idApi, idExclusive, idLink, idInputs, idDecks, idFollow, idLook, idStartup, idOversampling,
          idCheckUpdates, idTooltips, idDrift, idInPeriod };

const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0};
// limiter look-ahead choices (ms; 1.5 the default). Nothing between 0 and 1.5: below about 1.3 ms the true-peak
// detector does not fit (deck.h, kMinLookFrames) - 0.5 and 1.0 limited the sample peak only, without a word
const long kPeriods[] = {64, 128, 192, 256, 480, 512, 1024}; // frames, for WASAPI / DirectSound / MME
const AudioApi kApis[] = {AudioApi::Asio, AudioApi::Wasapi, AudioApi::DirectSound, AudioApi::Mme};
const LPARAM kNone = -1;                                     // "(none)" in a device list

void addItem(HWND combo, const std::wstring& text, LPARAM data) {
    const LRESULT i = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(i), data);
}

// Selects the item whose data is `data`; returns false when there is none.
bool selectData(HWND combo, LPARAM data) {
    const LRESULT n = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < n; ++i)
        if (SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == data) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return true;
        }
    return false;
}

LPARAM selectedData(HWND combo, LPARAM fallback) {
    const LRESULT i = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return i < 0 ? fallback : static_cast<LPARAM>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
}

std::wstring khz(double r) {
    wchar_t b[32];
    swprintf(b, 32, r == static_cast<long>(r / 1000) * 1000.0 ? L"%.0f kHz" : L"%.1f kHz", r / 1000.0);
    return b;
}

} // namespace

AudioWindow::AudioWindow(Settings& settings, AudioIO& audio, Changed onChange)
    : settings_(settings), audio_(audio), onChange_(std::move(onChange)) {}

AudioWindow::~AudioWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool AudioWindow::asio() const { return settings_.api == "ASIO"; }

bool AudioWindow::mixed() const { return asio() && settings_.asioInputs != "ASIO"; }

void AudioWindow::show(HWND owner) {
    owner_ = owner;
    if (!hwnd_) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.lpszClassName = kClass;
            registered = RegisterClassExW(&wc) != 0;
        }
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        hwnd_ = CreateWindowExW(0, kClass, L"DJAP setup", style, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, owner, nullptr, inst, this);
        if (!hwnd_) return;
        RECT r{0, 0, px(800), px(776)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        layout();
        placement::remember(hwnd_, "Setup", owner); // after layout(): the window fits its content
    }
    fillAll();
    refresh();
    ShowWindow(hwnd_, placement::showCommand(hwnd_));
    SetForegroundWindow(hwnd_);
}

LRESULT CALLBACK AudioWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AudioWindow* self = reinterpret_cast<AudioWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<AudioWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
    }
    return r;
}

LRESULT AudioWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, themed)) return themed; // brushed aluminium, 3D buttons
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        createControls();
        setFonts();
        return 0;
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
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (font_) DeleteObject(font_);
        font_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void AudioWindow::createControls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    const wchar_t* labels[14] = {L"ASIO driver", L"Deck A input", L"Deck A output", L"Deck B input", L"Deck B output",
                                 L"Sample rate", L"Buffer size",  L"Health",        L"Audio API",    L"Deck inputs",
                                 L"Processing",  L"Look-ahead",   L"Updates",       L"Oversampling"};
    for (int i = 0; i < 14; ++i) labels_[i] = make(L"STATIC", labels[i], SS_LEFT | SS_CENTERIMAGE, 0);
    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    decks_ = make(WC_COMBOBOXW, L"", combo, idDecks);
    api_ = make(WC_COMBOBOXW, L"", combo, idApi);
    inputs_ = make(WC_COMBOBOXW, L"", combo, idInputs);
    exclusive_ = make(L"BUTTON", L"Exclusive (WASAPI: lower latency)",
                      BS_AUTOCHECKBOX | WS_TABSTOP, idExclusive);
    drift_ = make(WC_COMBOBOXW, L"", combo, idDrift);
    inPeriod_ = make(WC_COMBOBOXW, L"", combo, idInPeriod);
    driver_ = make(WC_COMBOBOXW, L"", combo, idDriver);
    open_ = make(L"BUTTON", L"Open driver", BS_OWNERDRAW | WS_TABSTOP, idOpen);
    close_ = make(L"BUTTON", L"Close driver", BS_OWNERDRAW | WS_TABSTOP, idClose);
    panel_ = make(L"BUTTON", L"Control panel", BS_OWNERDRAW | WS_TABSTOP, idPanel);
    info_ = make(L"STATIC", L"", SS_LEFT, 0);
    inA_ = make(WC_COMBOBOXW, L"", combo, idInA);
    outA_ = make(WC_COMBOBOXW, L"", combo, idOutA);
    inB_ = make(WC_COMBOBOXW, L"", combo, idInB);
    outB_ = make(WC_COMBOBOXW, L"", combo, idOutB);
    rate_ = make(WC_COMBOBOXW, L"", combo, idRate);
    buffer_ = make(WC_COMBOBOXW, L"", combo, idBuffer);
    link_ = make(L"BUTTON", L"Deck A and deck B use the same loudness preset and colour (a choice on either deck sets both)",
                 BS_AUTOCHECKBOX | WS_TABSTOP, idLink);
    follow_ = make(L"BUTTON", L"The outputs follow the Windows volume (volume keys, taskbar) and mute",
                   BS_AUTOCHECKBOX | WS_TABSTOP, idFollow);
    start_ = make(L"BUTTON", L"Start audio", BS_OWNERDRAW | WS_TABSTOP, idStart);
    stop_ = make(L"BUTTON", L"Stop audio", BS_OWNERDRAW | WS_TABSTOP, idStop);
    state_ = make(L"STATIC", L"", SS_LEFT | SS_NOTIFY, 0);
    stateShown_.clear();
    health_ = make(L"STATIC", L"", SS_LEFT | SS_NOTIFY, 0);
    resetHealth_ = make(L"BUTTON", L"Reset counters", BS_OWNERDRAW | WS_TABSTOP, idResetHealth);
    look_ = make(WC_COMBOBOXW, L"", combo, idLook);
    oversampling_ = make(WC_COMBOBOXW, L"", combo, idOversampling);
    startup_ = make(L"BUTTON", L"Start DJAP when Windows starts (in the notification area, with the audio of the last "
                               L"session)",
                    BS_AUTOCHECKBOX | WS_TABSTOP, idStartup);
    tooltips_ = make(L"BUTTON", L"Show tooltips (what a control does, when the mouse rests on it)",
                     BS_AUTOCHECKBOX | WS_TABSTOP, idTooltips);
    checkUpdates_ = make(L"BUTTON", L"Check GitHub for a new version once a day (installed only when you click Install "
                                    L"update)",
                         BS_AUTOCHECKBOX | WS_TABSTOP, idCheckUpdates);
    note_ = make(L"STATIC",
                 L"Opening a driver or an exclusive device takes it from other programs: DJ software on the same "
                 L"interface may lose its audio. Test first with outputs nobody hears - routing may still send them "
                 L"to speakers.",
                 SS_LEFT, 0);

    for (HWND b : {open_, close_, panel_, start_, stop_, resetHealth_}) theme::makeButton(b);
    for (HWND c : {decks_, api_, inputs_, drift_, inPeriod_, driver_, inA_, outA_, inB_, outB_, rate_, buffer_, look_, oversampling_}) theme::makeCombo(c);
    tips_.create(hwnd_);
    tips_.add(decks_, L"Two decks: for DJs - deck A and deck B, each with its own input, output and loudness preset (two "
                      L"players or a mixer's channels).\nOne channel: one stereo processor, for example for everything "
                      L"Windows plays (through a virtual cable or a loopback input) to one output. Changing it while "
                      L"the audio runs starts the audio again.");
    tips_.add(api_, L"The audio system for the whole program.\nASIO: one driver for the interface, the lowest "
                    L"latency; a channel pair per deck.\nWASAPI: Windows' own audio, shared with other programs (or "
                    L"exclusive); an input and an output device per deck.\nDirectSound, MME: older, more latency; "
                    L"for devices without a good driver.\nChoosing another one stops the audio.");
    tips_.add(exclusive_, L"WASAPI only: take the devices for this program alone - lower latency and no Windows "
                          L"mixing, but other programs cannot use them while the audio runs (asks at Start).");
    tips_.add(drift_, L"WASAPI, DirectSound, MME: each deck's input and output device usually run on clocks of their "
                      L"own, which drift apart - without correction a glitch now and then. Auto (the default): the "
                      L"program reads the input a little faster or slower (at most 0.2 %, inaudible) so they never "
                      L"drift apart, at the cost of a buffer of about 8 ms (128-frame periods) to 23 ms (10 ms "
                      L"periods), shown while running - except for an input and output that WASAPI finds on one "
                      L"device (one clock): one stream, no buffer. Always: corrected also then (if a device glitches "
                      L"now and then on Auto). Off: always one stream from input to output, no buffer.");
    tips_.add(inPeriod_, L"With ASIO and WASAPI inputs: how often Windows hands over the input's sound. Default: every "
                         L"10 ms - the largest part of the latency (it counts twice: the input's own and in the drift "
                         L"buffer). 8, 6, 5 or 4 ms through Windows' low-latency mode save about 4, 8, 10 or 12 ms. It "
                         L"needs a device that has the mode, set in Windows' sound settings to the same sample rate as "
                         L"the ASIO interface; otherwise the input keeps 10 ms (Health then says so). A virtual cable's "
                         L"own timer must tick at least 3-4 times per period (measured: its timer at 3 ms - 8 ms or "
                         L"more; at 1 ms - 4 ms or more); shorter gives clicks and distorted sound. Try it while you "
                         L"listen, and watch the underruns.");
    tips_.add(inputs_, L"With ASIO: where the decks' inputs come from - the interface's ASIO input pairs, or per "
                       L"deck an input device of WASAPI, DirectSound or MME (for example a player sending into a "
                       L"virtual cable). The outputs stay ASIO.\nSuch an input runs on its own clock: the program "
                       L"reads it a little faster or slower (at most 0.2 %, inaudible) so the two never drift apart, "
                       L"at the cost of a buffer of about 15-20 ms with a 10 ms input (shown while running).");
    tips_.add(driver_, L"The ASIO driver of the interface, from the list Windows keeps. Nothing is opened until "
                       L"Open driver or Start audio.");
    tips_.add(open_, L"Loads and opens the driver (asks first). This takes the interface: other programs on the same "
                     L"driver, such as DJ software, may lose their audio. Nothing plays until Start audio.");
    tips_.add(close_, L"Stops the audio and releases the driver for other programs.");
    tips_.add(panel_, L"Opens the driver's own settings (buffer size, clock source, the interface's routing).");
    const wchar_t* inTip = L"Where this deck's input comes from: an input channel pair of the interface (ASIO), or "
                           L"an input device. \"(none)\": the deck does not run (WASAPI / DirectSound / MME). While "
                           L"the audio runs a new choice is taken at once (the audio starts again).";
    const wchar_t* outTip = L"Where this deck's processed audio goes: an output channel pair of the interface "
                            L"(ASIO), or an output device. Deck A and deck B need different pairs. While the audio "
                            L"runs a new choice is taken at once.";
    tips_.add(inA_, inTip);
    tips_.add(inB_, inTip);
    tips_.add(outA_, outTip);
    tips_.add(outB_, outTip);
    tips_.add(rate_, L"Sample rate of the decks and the device. ASIO switches the interface to it at Start when "
                     L"the driver accepts it, else it keeps the interface's own rate. Other APIs convert when a "
                     L"device runs at another rate.");
    tips_.add(buffer_, L"Frames per period. Smaller = lower latency, but more risk of dropouts when the PC is busy "
                       L"(watch Health: late periods are dropouts). ASIO: the driver's preferred size or a size it "
                       L"allows. Other APIs: the period asked of Windows.");
    tips_.add(link_, L"On: deck B always uses deck A's loudness preset and colour - the main window then shows one set of "
                     L"controls (deck A's), and its Bypass counts for both decks.");
    tips_.add(follow_, L"On: the volume and mute of Windows' default playback device - what the keyboard's volume "
                       L"keys and the taskbar change - turn the outputs down after the processing (never up: the "
                       L"ceiling holds). The meters show the processing before this volume. When the default playback "
                       L"device is the virtual cable the program reads and Windows itself turns that cable down, the "
                       L"program undoes that at its input, so the processing still sees the music's own level.\nOne "
                       L"choice per mode: on by default with one channel, off with two decks (a key on the keyboard "
                       L"should not move a DJ's outputs).");
    tips_.add(start_, L"Starts the audio with the choices above (opens the driver first when needed). If the audio "
                      L"runs when you close the program, it starts again by itself next time.");
    tips_.add(stop_, L"Stops the audio. An ASIO driver stays open until Close driver.");
    tips_.add(state_, L"What runs now: sample rate and period, and per deck the latency from input to output - the "
                      L"device's input and output buffers plus the processing (the limiter look-ahead). With an "
                      L"input of another API also its drift buffer, the measured clock drift in ppm, and underruns "
                      L"(the input fell behind: a moment of silence) and overruns (input blocks dropped).");
    tips_.add(health_, L"Periods: device periods processed. Heaviest / last: processing time in % of a period. Over "
                       L"half: periods that needed more than half their time (a warning). Late: periods not ready "
                       L"in time - a dropout. Answered with silence: the engine was not ready. Driver resyncs and "
                       L"overloads: reported by the ASIO driver.");
    tips_.add(resetHealth_, L"Sets the health counters back to zero.");
    tips_.add(look_, L"How far the limiter looks ahead, for both decks: the only delay the processing adds. "
                     L"Longer catches peaks more gently; every choice above 0 limits the true peak. 0 ms = no delay, "
                     L"the limiter then works on the sample peak only. Changed at once with a short fade.");
    tips_.add(oversampling_, L"How many times the sample rate the soft clipper and the exciter work at, for both decks. "
                             L"Their overtones above the audio band would fold back as harsh tones that do not belong to "
                             L"the music: 2x leaves a rest on hard-driven high tones (about 40 dB under a 15 kHz tone), "
                             L"4x and 8x take it away (more than 100 dB under) at a little more CPU. Adds no latency. "
                             L"Changed with a short fade of those two stages.");
    tips_.add(startup_, L"On: DJAP starts when you log on to Windows, hidden in the notification area, and starts "
                        L"the audio of the last session (it retries for 2 minutes while the devices come up). "
                        L"Saved in Windows' list of startup programs, not in the settings file.");
    tips_.add(tooltips_, L"On: every control of DJAP explains itself when the mouse rests on it (as this one). Off: no "
                         L"tooltips in any window. The plug-in follows the next time it is loaded.");
    tips_.add(checkUpdates_, L"On: once a day (and a little after the program starts) DJAP asks GitHub whether a newer "
                             L"version has been published, and tells you (a message at the icon in the notification "
                             L"area, and in the About window). Nothing is downloaded or installed until you click "
                             L"Install update in the About window (right-click the main window: About DJAP...). Only "
                             L"the program's public GitHub page is asked; it learns no more than any web page.");
}

void AudioWindow::setFonts() {
    if (font_) DeleteObject(font_);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : labels_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (HWND h : {decks_, api_, inputs_, exclusive_, drift_, inPeriod_, link_, follow_, driver_, open_, close_, panel_, info_, inA_, outA_, inB_, outB_, rate_, buffer_,
                   start_, stop_, state_, health_, resetHealth_, note_, look_, oversampling_, startup_, checkUpdates_, tooltips_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void AudioWindow::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int m = px(12), h = px(28), g = px(6), labelW = px(100), W = rc.right - 2 * m;
    // the height of a line of text, for the text blocks (each as high as its lines, not a multiple of a row)
    int lh = px(17);
    if (HDC dc = GetDC(hwnd_)) {
        HGDIOBJ old = SelectObject(dc, font_);
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm)) lh = tm.tmHeight + tm.tmExternalLeading;
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }
    int y = m;
    const int bw = px(112);
    MoveWindow(labels_[10], m, y, labelW, h, TRUE);
    MoveWindow(decks_, m + labelW + g, y, px(420), px(200), TRUE);
    y += h + g;
    MoveWindow(labels_[8], m, y, labelW, h, TRUE);
    MoveWindow(api_, m + labelW + g, y, px(300), px(200), TRUE);
    MoveWindow(exclusive_, m + labelW + px(300) + 2 * g, y, W - labelW - px(300) - 2 * g, h, TRUE);
    y += h + g;
    MoveWindow(labels_[9], m, y, labelW, h, TRUE);
    MoveWindow(inputs_, m + labelW + g, y, px(300), px(200), TRUE);
    MoveWindow(drift_, m + labelW + px(300) + 2 * g, y, W - labelW - px(300) - 2 * g, px(200), TRUE);
    MoveWindow(inPeriod_, m + labelW + px(300) + 2 * g, y, W - labelW - px(300) - 2 * g, px(200), TRUE); // (one shows)
    y += h + g;
    MoveWindow(labels_[0], m, y, labelW, h, TRUE);
    const int comboW = W - labelW - 3 * (bw + g) - g;
    MoveWindow(driver_, m + labelW + g, y, comboW, px(300), TRUE);
    int x = m + labelW + comboW + 2 * g;
    for (HWND b : {open_, close_, panel_}) {
        MoveWindow(b, x, y, bw, h, TRUE);
        x += bw + g;
    }
    y += h + g;
    MoveWindow(info_, m + labelW + g, y, W - labelW - g, 2 * lh, TRUE);
    y += 2 * lh + 2 * g;
    const int colW = (W - g) / 2, cw = colW - labelW - g;
    // deck A's row, deck B's row (two decks only), rate and buffer
    const bool two = settings_.decks == 2;
    HWND pairs[6] = {inA_, outA_, inB_, outB_, rate_, buffer_};
    int row = 0;
    for (int i = 0; i < 6; ++i) {
        const bool shown = two || (i != 2 && i != 3);
        ShowWindow(labels_[1 + i], shown ? SW_SHOW : SW_HIDE);
        ShowWindow(pairs[i], shown ? SW_SHOW : SW_HIDE);
        if (!shown) continue;
        const int cx = m + (i % 2) * (colW + g), cy = y + row * (h + g);
        MoveWindow(labels_[1 + i], cx, cy, labelW, h, TRUE);
        MoveWindow(pairs[i], cx + labelW + g, cy, cw, px(300), TRUE);
        if (i % 2) ++row;
    }
    y += row * (h + g);
    MoveWindow(labels_[11], m, y, labelW, h, TRUE);
    MoveWindow(look_, m + labelW + g, y, cw, px(300), TRUE);
    MoveWindow(labels_[13], m + colW + g, y, labelW, h, TRUE);
    MoveWindow(oversampling_, m + colW + g + labelW + g, y, cw, px(300), TRUE);
    y += h + g;
    SetWindowTextW(labels_[9], two ? L"Deck inputs" : L"Input from");
    SetWindowTextW(labels_[1], two ? L"Deck A input" : L"Input");
    SetWindowTextW(labels_[2], two ? L"Deck A output" : L"Output");
    // Start / Stop right under the audio choices (client, 2026-10-27)
    MoveWindow(start_, m + labelW + g, y, bw, h, TRUE);
    MoveWindow(stop_, m + labelW + bw + 2 * g, y, bw, h, TRUE);
    y += h + 2 * g;
    // the switches, one under the other (the daily update check among them: no heading of its own - client,
    // 2026-10-27; Check now / Install update are in the About window)
    ShowWindow(link_, two ? SW_SHOW : SW_HIDE);
    if (two) {
        MoveWindow(link_, m + labelW + g, y, W - labelW - g, h, TRUE);
        y += h + g;
    }
    for (HWND box : {follow_, startup_, tooltips_, checkUpdates_}) {
        MoveWindow(box, m + labelW + g, y, W - labelW - g, h, TRUE);
        y += h + g;
    }
    ShowWindow(labels_[12], SW_HIDE); // (the former "Updates" heading)
    y += g;
    // one section, Health: what runs (a line for the stream, then one per deck that may wrap to two, and a line for
    // a message - as many lines as the text takes, refresh()), then the counters with Reset counters beside them
    // (client, 2026-10-27: one heading)
    const int stateLines = stateLines_;
    MoveWindow(labels_[7], m, y, labelW, lh, TRUE);
    MoveWindow(state_, m + labelW + g, y, W - labelW - g, stateLines * lh, TRUE);
    y += stateLines * lh + g;
    MoveWindow(health_, m + labelW + g, y, W - labelW - bw - 2 * g, 3 * lh, TRUE);
    MoveWindow(resetHealth_, m + W - bw, y, bw, h, TRUE);
    y += std::max(3 * lh, h) + 2 * g;
    MoveWindow(note_, m, y, W, 2 * lh, TRUE);
    y += 2 * lh + m;
    // the window is as high as its content (one channel needs less than two decks)
    if (rc.bottom != y) {
        RECT r{0, 0, rc.right, y};
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)), dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void AudioWindow::fillAll() {
    filling_ = true;
    // program-wide: limiter look-ahead, start with Windows (read from Windows' list each time: it can change there)
    SendMessageW(look_, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < static_cast<int>(std::size(kLookChoicesMs)); ++i) {
        wchar_t t[48];
        const double l = kLookChoicesMs[i];
        swprintf(t, 48, l == 0.0 ? L"0 ms (sample peak, no delay)" : l == 1.5 ? L"%.1f ms (default)" : L"%.1f ms", l);
        addItem(look_, t, i);
        if (std::fabs(kLookChoicesMs[i] - settings_.lookAheadMs) < 1e-9) SendMessageW(look_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    }
    SendMessageW(oversampling_, CB_RESETCONTENT, 0, 0);
    for (int f : {2, 4, 8}) {
        addItem(oversampling_, f == 4 ? L"4x (default)" : f == 2 ? L"2x" : L"8x", f);
        if (f == settings_.oversampling)
            SendMessageW(oversampling_, CB_SETCURSEL, static_cast<WPARAM>(SendMessageW(oversampling_, CB_GETCOUNT, 0, 0) - 1), 0);
    }
    SendMessageW(startup_, BM_SETCHECK, !testRun_ && startsWithWindows() ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(startup_, !testRun_);
    SendMessageW(checkUpdates_, BM_SETCHECK, settings_.checkUpdates ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tooltips_, BM_SETCHECK, settings_.showTooltips ? BST_CHECKED : BST_UNCHECKED, 0);
    // two decks or one channel
    SendMessageW(decks_, CB_RESETCONTENT, 0, 0);
    addItem(decks_, L"Two decks (DJ: deck A and deck B, each its own input, output and loudness preset)", 2);
    addItem(decks_, L"One channel (one stereo processor, e.g. for what Windows plays)", 1);
    selectData(decks_, settings_.decks);
    // API
    SendMessageW(api_, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < 4; ++i) addItem(api_, fromUtf8(apiLabel(kApis[i])), i);
    AudioApi current = AudioApi::Asio;
    apiFromName(settings_.api, current);
    for (int i = 0; i < 4; ++i)
        if (kApis[i] == current) SendMessageW(api_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    SendMessageW(exclusive_, BM_SETCHECK, settings_.exclusive ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(drift_, CB_RESETCONTENT, 0, 0);
    addItem(drift_, L"Drift correction: auto (not for one device)", Settings::kDriftAuto);
    addItem(drift_, L"Drift correction: always", Settings::kDriftAlways);
    addItem(drift_, L"Drift correction: off", Settings::kDriftOff);
    selectData(drift_, settings_.driftCorrection);
    SendMessageW(link_, BM_SETCHECK, settings_.linkPresets ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(follow_, BM_SETCHECK, settings_.followsVolume() ? BST_CHECKED : BST_UNCHECKED, 0);
    // ASIO: where the decks' inputs come from
    SendMessageW(inputs_, CB_RESETCONTENT, 0, 0);
    const wchar_t* inputLabels[4] = {L"ASIO input pairs", L"WASAPI devices (drift-corrected)",
                                     L"DirectSound devices (drift-corrected)", L"MME devices (drift-corrected)"};
    AudioApi inApi = AudioApi::Asio;
    apiFromName(settings_.asioInputs, inApi);
    for (int i = 0; i < 4; ++i) {
        addItem(inputs_, inputLabels[i], i);
        if (kApis[i] == inApi) SendMessageW(inputs_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    }
    EnableWindow(inputs_, current == AudioApi::Asio);
    // ASIO: one clock, its inputs of other APIs always corrected - the drift choice's place holds the input period
    SendMessageW(inPeriod_, CB_RESETCONTENT, 0, 0);
    addItem(inPeriod_, L"Input period: Windows' default (10 ms)", 0);
    for (int ms : {8, 6, 5, 4}) addItem(inPeriod_, L"Input period: " + std::to_wstring(ms) + L" ms", ms);
    selectData(inPeriod_, settings_.inputPeriodMs);
    ShowWindow(drift_, current != AudioApi::Asio ? SW_SHOW : SW_HIDE);
    ShowWindow(inPeriod_, current == AudioApi::Asio ? SW_SHOW : SW_HIDE);
    EnableWindow(inPeriod_, inApi == AudioApi::Wasapi);
    EnableWindow(exclusive_, current == AudioApi::Wasapi || (current == AudioApi::Asio && inApi == AudioApi::Wasapi));
    // devices of an API (lists only; nothing is opened); the saved one stays listed when it is not there
    auto devices = [&](HWND combo, std::vector<SysDevice>& list, size_t listed, const std::string& value,
                       const std::wstring& noneLabel) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        addItem(combo, noneLabel, kNone);
        if (!value.empty() && std::none_of(list.begin(), list.end(), [&](const SysDevice& d) { return d.name == value; }))
            list.push_back({value, false}); // saved, but not among this API's devices
        for (size_t i = 0; i < list.size(); ++i)
            addItem(combo,
                    fromUtf8(list[i].name) + (list[i].isDefault ? L"  (default)" : L"") +
                        (i >= listed ? L"  (not found)" : L""),
                    static_cast<LPARAM>(i));
        LPARAM sel = kNone;
        for (size_t i = 0; i < list.size(); ++i)
            if (!value.empty() && list[i].name == value) sel = static_cast<LPARAM>(i);
        selectData(combo, sel);
    };

    // ASIO drivers: from the registry; the saved one stays listed even when it is not installed
    SendMessageW(driver_, CB_RESETCONTENT, 0, 0);
    std::vector<std::string> names;
#if AP_HAVE_ASIO
    for (const AsioDriverEntry& d : AsioHost::drivers()) names.push_back(d.name);
#endif
    if (!settings_.driver.empty() && std::find(names.begin(), names.end(), settings_.driver) == names.end())
        names.push_back(settings_.driver);
    for (size_t i = 0; i < names.size(); ++i) {
        std::wstring t = fromUtf8(names[i]);
#if AP_HAVE_ASIO
        if (audio_.driverOpen() && names[i] == audio_.host().state().driver) t += L"  (open)";
#endif
        addItem(driver_, t, static_cast<LPARAM>(i));
        if (names[i] == settings_.driver) SendMessageW(driver_, CB_SETCURSEL, i, 0);
    }
    if (SendMessageW(driver_, CB_GETCURSEL, 0, 0) < 0 && !names.empty()) SendMessageW(driver_, CB_SETCURSEL, 0, 0);
    EnableWindow(driver_, asio());

    rates_.assign(std::begin(kRates), std::end(kRates));
    bufferSizes_ = {0};
    if (asio()) {
        // channel pairs: the open driver's channels with their names, else 1-2 .. 15-16
        long ins = 16, outs = 16;
        const std::vector<std::string>* inNames = nullptr;
        const std::vector<std::string>* outNames = nullptr;
#if AP_HAVE_ASIO
        if (audio_.driverOpen()) {
            const AsioDeviceState& st = audio_.host().state();
            ins = st.inputs;
            outs = st.outputs;
            inNames = &st.inputNames;
            outNames = &st.outputNames;
            if (!st.rates.empty()) rates_ = st.rates;
            // valid sizes besides the preferred one: granularity -1 = powers of two; > 0 = steps of it from the
            // minimum (offered: the minimum, the maximum and the powers of two on a step); 0 = preferred only
            auto offer = [&](long s) {
                if (s > 0 && s != st.preferredBuffer &&
                    std::find(bufferSizes_.begin(), bufferSizes_.end(), s) == bufferSizes_.end())
                    bufferSizes_.push_back(s);
            };
            if (st.granularity != 0) {
                if (st.granularity > 0) offer(st.minBuffer);
                // at most 8192 frames (the largest the settings keep); a driver reporting a huge maximum made the
                // doubling overflow a 32-bit long
                for (long s = 16; s <= std::min(st.maxBuffer, 8192L); s *= 2)
                    if (s >= st.minBuffer && (st.granularity == -1 || (s - st.minBuffer) % st.granularity == 0))
                        offer(s);
                if (st.granularity > 0) offer(st.maxBuffer);
                std::sort(bufferSizes_.begin() + 1, bufferSizes_.end());
            }
        }
#endif
        auto pairs = [&](HWND combo, long count, const std::vector<std::string>* nm, int value) {
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            for (long c = 1; c + 1 <= count; c += 2) {
                wchar_t t[160];
                if (nm && static_cast<size_t>(c) < nm->size())
                    swprintf(t, 160, L"%ld-%ld   %s / %s", c, c + 1,
                             fromUtf8((*nm)[static_cast<size_t>(c - 1)]).c_str(),
                             fromUtf8((*nm)[static_cast<size_t>(c)]).c_str());
                else
                    swprintf(t, 160, L"%ld-%ld", c, c + 1);
                addItem(combo, t, c);
            }
            if (!selectData(combo, value)) {
                wchar_t t[80];
                swprintf(t, 80, L"%d-%d (not on this interface)", value, value + 1);
                addItem(combo, t, value);
                selectData(combo, value);
            }
        };
        pairs(inA_, ins, inNames, settings_.inputA);
        pairs(inB_, ins, inNames, settings_.inputB);
        pairs(outA_, outs, outNames, settings_.outputA);
        pairs(outB_, outs, outNames, settings_.outputB);
        if (mixed()) {
            // the inputs: devices of the other API; "ASIO input x-y" = the deck keeps its pair
            listError_.clear();
            if (!SysAudio::devices(inApi, sysInputs_, sysOutputs_, listError_)) sysInputs_.clear(), sysOutputs_.clear();
            const size_t listedIn = sysInputs_.size();
            wchar_t a[64], b[64];
            swprintf(a, 64, L"ASIO input %d-%d", settings_.inputA, settings_.inputA + 1);
            swprintf(b, 64, L"ASIO input %d-%d", settings_.inputB, settings_.inputB + 1);
            devices(inA_, sysInputs_, listedIn, settings_.asioInputDevice[0], a);
            devices(inB_, sysInputs_, listedIn, settings_.asioInputDevice[1], b);
        }
    } else {
        listError_.clear();
        if (!SysAudio::devices(current, sysInputs_, sysOutputs_, listError_)) sysInputs_.clear(), sysOutputs_.clear();
        const size_t listedIn = sysInputs_.size(), listedOut = sysOutputs_.size();
        devices(inA_, sysInputs_, listedIn, settings_.inDevice[0], L"(none)");
        devices(inB_, sysInputs_, listedIn, settings_.inDevice[1], L"(none)");
        devices(outA_, sysOutputs_, listedOut, settings_.outDevice[0], L"(none)");
        devices(outB_, sysOutputs_, listedOut, settings_.outDevice[1], L"(none)");
        bufferSizes_.assign(std::begin(kPeriods), std::end(kPeriods));
        bufferSizes_.insert(bufferSizes_.begin(), 0);
    }

    SendMessageW(rate_, CB_RESETCONTENT, 0, 0);
    for (double r : rates_) addItem(rate_, khz(r), static_cast<LPARAM>(r));
    if (!selectData(rate_, settings_.sampleRate)) {
        addItem(rate_, khz(settings_.sampleRate) + L" (not offered by the driver)", settings_.sampleRate);
        selectData(rate_, settings_.sampleRate);
    }

    SendMessageW(buffer_, CB_RESETCONTENT, 0, 0);
    for (long b : bufferSizes_) {
        std::wstring t;
#if AP_HAVE_ASIO
        if (b == 0 && asio() && audio_.driverOpen())
            t = L"driver's preferred (" + std::to_wstring(audio_.host().state().preferredBuffer) + L" frames)";
#endif
        if (b == 0 && t.empty()) t = asio() ? L"driver's preferred" : L"low-latency default of the API";
        if (b > 0) t = std::to_wstring(b) + L" frames";
        addItem(buffer_, t, b);
    }
    if (!selectData(buffer_, settings_.bufferSize)) {
        addItem(buffer_, std::to_wstring(settings_.bufferSize) + L" frames (saved)", settings_.bufferSize);
        selectData(buffer_, settings_.bufferSize);
    }
    filling_ = false;
}

void AudioWindow::readChoices() {
    // the API is read in command(): the device / pair lists still belong to the API they were filled for
    settings_.exclusive = SendMessageW(exclusive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.driftCorrection = static_cast<int>(selectedData(drift_, settings_.driftCorrection));
    settings_.inputPeriodMs = static_cast<int>(selectedData(inPeriod_, settings_.inputPeriodMs));
    const LRESULT d = SendMessageW(driver_, CB_GETCURSEL, 0, 0);
    if (d >= 0) {
        const int n = static_cast<int>(SendMessageW(driver_, CB_GETLBTEXTLEN, static_cast<WPARAM>(d), 0));
        std::wstring t(static_cast<size_t>(n), L'\0');
        SendMessageW(driver_, CB_GETLBTEXT, static_cast<WPARAM>(d), reinterpret_cast<LPARAM>(t.data()));
        const size_t open = t.find(L"  (open)");
        if (open != std::wstring::npos) t.resize(open);
        settings_.driver = toUtf8(t);
    }
    auto name = [](HWND combo, const std::vector<SysDevice>& list) {
        const LPARAM i = selectedData(combo, kNone);
        return i >= 0 && static_cast<size_t>(i) < list.size() ? list[static_cast<size_t>(i)].name : std::string();
    };
    if (asio()) {
        if (mixed()) { // the input lists hold the other API's devices
            settings_.asioInputDevice[0] = name(inA_, sysInputs_);
            settings_.asioInputDevice[1] = name(inB_, sysInputs_);
        } else {
            settings_.inputA = static_cast<int>(selectedData(inA_, settings_.inputA));
            settings_.inputB = static_cast<int>(selectedData(inB_, settings_.inputB));
        }
        settings_.outputA = static_cast<int>(selectedData(outA_, settings_.outputA));
        settings_.outputB = static_cast<int>(selectedData(outB_, settings_.outputB));
    } else {
        settings_.inDevice[0] = name(inA_, sysInputs_);
        settings_.inDevice[1] = name(inB_, sysInputs_);
        settings_.outDevice[0] = name(outA_, sysOutputs_);
        settings_.outDevice[1] = name(outB_, sysOutputs_);
    }
    settings_.sampleRate = static_cast<int>(selectedData(rate_, settings_.sampleRate));
    settings_.bufferSize = static_cast<int>(selectedData(buffer_, settings_.bufferSize));
}

bool AudioWindow::openDriver() {
    if (settings_.driver.empty()) {
        placement::messageBox(hwnd_, L"No ASIO driver is installed.", L"Audio device", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    const std::wstring q = L"Open the ASIO driver \"" + fromUtf8(settings_.driver) +
                           L"\"?\n\nThis takes the interface: other programs using the same driver (DJ software) may "
                           L"lose their audio. Nothing plays until Start audio.";
    if (placement::messageBox(hwnd_, q.c_str(), L"Open driver", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return false;
    audio_.stopDevice(); // another API may be running
    std::string err;
    if (!audio_.openDriver(settings_.driver, owner_, err)) {
        placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Open driver", MB_OK | MB_ICONERROR);
        return false;
    }
    fillAll(); // the driver's own channels, rates and buffer sizes
    readChoices();
    return true;
}

bool AudioWindow::startAudio(bool askExclusive) {
    std::string err;
    bool ok = false;
    if (asio()) {
        const SysInputs inputs = sysInputsFrom(settings_);
        if (askExclusive && inputs.any() && inputs.exclusive && inputs.api == AudioApi::Wasapi) {
            const std::wstring q = L"Open the decks' input devices in WASAPI exclusive mode?\n\nThey are taken "
                                   L"from other programs while the audio runs.";
            if (placement::messageBox(hwnd_, q.c_str(), L"Start audio", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return false;
        }
        // the driver chosen in the list: an open one of another name is closed first (it played on the old one
        // while the new name was saved)
        if (audio_.driverOpen() && audio_.driverName() != settings_.driver) audio_.closeDriver();
        if (!audio_.driverOpen() && !openDriver()) return false;
        ok = audio_.startDevice(settings_.sampleRate, settings_.bufferSize, asioChannelsFrom(settings_), inputs, err);
    } else {
        AudioApi api = AudioApi::Wasapi;
        apiFromName(settings_.api, api);
        if (askExclusive && settings_.exclusive && api == AudioApi::Wasapi) {
            const std::wstring q = L"Start in WASAPI exclusive mode?\n\nThe chosen devices are taken from other "
                                   L"programs while the audio runs.";
            if (placement::messageBox(hwnd_, q.c_str(), L"Start audio", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return false;
        }
        SysRoute routes[2];
        sysRoutesFrom(settings_, routes);
        ok = audio_.startSystem(api, routes, settings_.sampleRate, static_cast<unsigned>(settings_.bufferSize),
                                settings_.exclusive, settings_.driftCorrection, err);
    }
    settings_.startAudio = ok;
    if (!ok) placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Start audio", MB_OK | MB_ICONERROR);
    return ok;
}

void AudioWindow::command(int id, int code) {
    // program-wide settings first (they never restart the audio: the decks change look-ahead with a short fade)
    if (id == idLook && code == CBN_SELCHANGE && !filling_) {
        const LPARAM i = selectedData(look_, -1);
        if (i >= 0 && i < static_cast<LPARAM>(std::size(kLookChoicesMs))) settings_.lookAheadMs = kLookChoicesMs[i];
        onChange_();
        refresh();
        return;
    }
    if (id == idOversampling && code == CBN_SELCHANGE && !filling_) {
        const LPARAM f = selectedData(oversampling_, settings_.oversampling);
        if (f == 2 || f == 4 || f == 8) settings_.oversampling = static_cast<int>(f);
        onChange_(); // the decks take it with a short fade of the clipper and the exciter
        return;
    }
    if (id == idStartup && code == BN_CLICKED) {
        std::string err;
        if (!testRun_ && !setStartWithWindows(SendMessageW(startup_, BM_GETCHECK, 0, 0) == BST_CHECKED, err))
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Start with Windows", MB_OK | MB_ICONWARNING);
        SendMessageW(startup_, BM_SETCHECK, !testRun_ && startsWithWindows() ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    if (id == idTooltips && code == BN_CLICKED) {
        settings_.showTooltips = SendMessageW(tooltips_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        Tooltips::setEnabled(settings_.showTooltips);
        onChange_(); // saved
        return;
    }
    if (id == idCheckUpdates && code == BN_CLICKED) {
        settings_.checkUpdates = SendMessageW(checkUpdates_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        onChange_();
        refresh();
        return;
    }
    if (id == idApi && code == CBN_SELCHANGE && !filling_) {
        readChoices(); // the old API's choices first, while the lists still hold them
        const LRESULT a = SendMessageW(api_, CB_GETCURSEL, 0, 0);
        const std::string chosen = a >= 0 && a < 4 ? apiName(kApis[a]) : settings_.api;
        if (chosen != settings_.api && audio_.deviceRunning()) { // another API: what runs stops
            audio_.stopDevice();
            settings_.startAudio = false;
        }
        settings_.api = chosen;
        fillAll();
        onChange_();
        refresh();
        return;
    }
    if (id == idDecks && code == CBN_SELCHANGE && !filling_) {
        readChoices();
        const int chosen = static_cast<int>(selectedData(decks_, settings_.decks)) == 1 ? 1 : 2;
        const bool restart = chosen != settings_.decks && audio_.deviceRunning();
        settings_.decks = chosen;
        layout();
        if (restart) startAudio(false); // deck B joins or leaves at once
        onChange_();
        refresh();
        return;
    }
    if (id == idInputs && code == CBN_SELCHANGE && !filling_) {
        readChoices(); // the old choice's lists first
        const LRESULT a = SendMessageW(inputs_, CB_GETCURSEL, 0, 0);
        const std::string chosen = a >= 0 && a < 4 ? apiName(kApis[a]) : settings_.asioInputs;
        const bool restart = chosen != settings_.asioInputs && audio_.deviceRunning();
        settings_.asioInputs = chosen;
        fillAll();
        if (restart) startAudio(true); // the decks' inputs change at once
        onChange_();
        refresh();
        return;
    }
    if (id == idFollow && code == BN_CLICKED) { // the choice of the mode in use
        settings_.followVolume[settings_.decks == 1 ? 0 : 1] = SendMessageW(follow_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        onChange_();
        return;
    }
    if (id == idLink && code == BN_CLICKED) {
        settings_.linkPresets = SendMessageW(link_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (settings_.linkPresets) settings_.deck[1].preset = settings_.deck[0].preset; // deck B follows deck A
        onChange_();
        return;
    }
    if ((code == CBN_SELCHANGE && !filling_) || (id == idExclusive && code == BN_CLICKED)) {
        readChoices();
        // while the audio runs a new device, pair, rate or buffer size is taken at once: the audio starts again
        if (id != idDriver && audio_.deviceRunning()) startAudio(id == idExclusive);
        onChange_();
        refresh();
        return;
    }
    if (code != BN_CLICKED) return;
    switch (id) {
    case idOpen:
        readChoices();
        if (audio_.driverOpen()) audio_.closeDriver();
        openDriver();
        break;
    case idClose:
        audio_.closeDriver();
        settings_.startAudio = false;
        fillAll();
        break;
    case idPanel:
#if AP_HAVE_ASIO
        if (audio_.driverOpen()) audio_.controlPanel();
#endif
        break;
    case idStart:
        readChoices();
        startAudio(true);
        break;
    case idStop:
        audio_.stopDevice();
        settings_.startAudio = false;
        break;
    case idResetHealth:
        audio_.resetHealth();
        break;
    default:
        return;
    }
    onChange_();
    refresh();
}

// Updates: this version and what the last check found; installing while the audio runs asks first.
UpdateView AudioWindow::updateView() const {
    UpdateView v;
    if (!updater_) return v;
    const bool running = audio_.deviceRunning();
    {
        const Updater::State st = updater_->state();
        const std::wstring me = fromUtf8(AP_VERSION_STRING), next = fromUtf8(updater_->release().version);
        const std::wstring when = updater_->checkedAt().empty() ? L"" : L", checked at " + fromUtf8(updater_->checkedAt());
        std::wstring text;
        switch (st) {
        case Updater::State::Checking: text = L"DJAP " + me + L" - asking GitHub..."; break;
        case Updater::State::UpToDate:
            text = L"DJAP " + me + L" is up to date" + when +
                   (updater_->error().empty() ? L"" : L" (" + fromUtf8(updater_->error()) + L")");
            break;
        case Updater::State::Available:
            text = L"DJAP " + next + L" is available (this is " + me + L")" +
                   (running ? L" - the audio pauses while it installs" : L"");
            break;
        case Updater::State::Downloading:
            text = L"Downloading DJAP " + next + L": " + std::to_wstring(updater_->percent()) + L" %";
            break;
        case Updater::State::Ready: text = L"Starting the setup of DJAP " + next + L"..."; break;
        case Updater::State::Failed: text = L"Update: " + fromUtf8(updater_->error()); break;
        default:
            text = L"DJAP " + me + (settings_.checkUpdates ? L" - GitHub is asked once a day" : L" - the daily check is off");
            break;
        }
        const bool newer = !next.empty() && compareVersions(updater_->release().version, AP_VERSION_STRING) > 0;
        v.text = text;
        v.canCheck = !updater_->busy() && st != Updater::State::Ready;
        v.canInstall = newer && !audio_.simulating() && (st == Updater::State::Available || st == Updater::State::Failed);
    }
    return v;
}

void AudioWindow::refresh() {
    if (!hwnd_) return;
    const bool open = audio_.driverOpen(), running = audio_.deviceRunning();
    EnableWindow(close_, asio() && open);
    EnableWindow(panel_, asio() && open);
    EnableWindow(stop_, running);
    EnableWindow(start_, !running);
    EnableWindow(open_, asio() && !running);
    std::wstring info, state = audio_.simulating() ? L"Not running (a simulation runs: starting the audio ends it)."
                                                   : L"Not running.";
    if (asio()) {
        info = L"The driver is not open: the lists show the saved choices (pairs up to 16 channels).";
#if AP_HAVE_ASIO
        if (open) {
            const AsioDeviceState& s = audio_.host().state();
            wchar_t b[320];
            std::wstring rates;
            for (double r : s.rates) rates += (rates.empty() ? L"" : L", ") + khz(r);
            swprintf(b, 320,
                     L"%ld inputs, %ld outputs; buffer %ld..%ld frames (preferred %ld); sample rates: %s.\nThe "
                     L"interface runs at %s now (Start switches it when another rate is chosen).",
                     s.inputs, s.outputs, s.minBuffer, s.maxBuffer, s.preferredBuffer,
                     rates.empty() ? L"none of 44.1-96 kHz" : rates.c_str(), khz(s.sampleRate).c_str());
            info = b;
        }
        if (running && audio_.host().running()) {
            const AsioDeviceState& s = audio_.host().state();
            const double ms = 1000.0 / s.sampleRate, look = lookFrames(settings_.lookAheadMs, s.sampleRate) * ms;
            wchar_t b[480];
            swprintf(b, 480, L"Running: %s, %ld frames (%.2f ms) per period, %hs.", khz(s.sampleRate).c_str(),
                     s.bufferSize, s.bufferSize * ms, formatName(s.formats[4]));
            state = b;
            for (int d = 0; d < settings_.decks; ++d) {
                const wchar_t* deck = settings_.decks == 1 ? L"Channel" : d == 0 ? L"Deck A" : L"Deck B";
                if (audio_.asyncInput(d)) {
                    // an input of another API: its period, the drift buffer, the measured drift
                    const AsyncInputState a = audio_.asyncState(d);
                    const SysStreamInfo in = audio_.sys().info(d);
                    // the input's part: the blocks it really hands over, as measured (eighth outside review: without
                    // Windows' low-latency mode the device's reported period can be shorter than its packets)
                    const double inMs = a.blockMs > 0.0 ? a.blockMs : in.latencyMs;
                    swprintf(b, 480,
                             L"\n%s: %hs \x201C%s\x201D \x2192 ASIO. Latency \x2248 %.1f ms (input %.1f + drift buffer "
                             L"%.1f + output %.2f + processing %.2f); drift %+.1f ppm%s; underruns %lld, overruns %lld",
                             deck, apiName(audio_.sys().api()), fromUtf8(in.input).c_str(),
                             inMs + a.targetMs + s.outputLatency * ms + look, inMs, a.targetMs, s.outputLatency * ms,
                             look, a.driftPpm, a.active ? L"" : L" (filling up)", a.underruns, a.overruns);
                    // a short input period asked for (Setup) but the input still comes in Windows' 10 ms blocks: say
                    // what that mode needs
                    if (settings_.inputPeriodMs > 0 && a.blockMs >= 9.0) {
                        state += b;
                        swprintf(b, 480, L"; the input still comes in %.0f ms blocks - Windows' short periods need a "
                                         L"device that has them, set to %s in Windows' sound settings",
                                 a.blockMs, khz(s.sampleRate).c_str());
                    }
                } else {
                    swprintf(b, 480, L"\n%s: ASIO \x2192 ASIO. Latency %.2f ms (input %.2f + output %.2f + processing %.2f)",
                             deck, (s.inputLatency + s.outputLatency) * ms + look, s.inputLatency * ms,
                             s.outputLatency * ms, look);
                }
                state += b;
            }
        } else if (open) {
            state = L"Driver open, not running.";
        }
#endif
        if (mixed() && !listError_.empty()) info += L"\nNo input device list: " + fromUtf8(listError_);
    } else {
        wchar_t b[200];
        swprintf(b, 200, L"%zu input and %zu output devices. Each deck runs from its input device to its output "
                         L"device; choose (none) for a deck that is not used.",
                 sysInputs_.size(), sysOutputs_.size());
        info = listError_.empty() ? b : L"No device list: " + fromUtf8(listError_);
        if (audio_.systemRunning()) {
            state = L"Running (" + fromUtf8(apiName(audio_.sys().api())) + L", decks at " + khz(audio_.sampleRate()) + L"):";
            for (int d = 0; d < 2; ++d) {
                const SysStreamInfo s = audio_.sys().info(d);
                if (!s.running) continue;
                wchar_t line[480];
                const double look = lookFrames(settings_.lookAheadMs, audio_.sampleRate()) * 1000.0 / audio_.sampleRate();
                if (s.driftCorrected) { // the drift buffer between the two streams, the measured drift
                    const AsyncInputState a = audio_.asyncState(d);
                    swprintf(line, 480,
                             L"\n%s: %s \x2192 %s; device %s, %u x %u frames; latency \x2248 %.1f ms (devices %.1f + "
                             L"drift buffer %.1f + processing %.2f); drift %+.1f ppm%s; underruns %lld, overruns %lld",
                             d == 0 ? L"Deck A" : L"Deck B", fromUtf8(s.input).c_str(), fromUtf8(s.output).c_str(),
                             khz(s.deviceRate).c_str(), s.periodFrames, s.periods, s.latencyMs + a.targetMs + look,
                             s.latencyMs, a.targetMs, look, a.driftPpm, a.active ? L"" : L" (filling up)", a.underruns,
                             a.overruns);
                } else {
                    swprintf(line, 480, L"\n%s: %s \x2192 %s; device %s, %u x %u frames; latency \x2248 %.1f ms + %.2f ms%s",
                             d == 0 ? L"Deck A" : L"Deck B", fromUtf8(s.input).c_str(), fromUtf8(s.output).c_str(),
                             khz(s.deviceRate).c_str(), s.periodFrames, s.periods, s.latencyMs, look,
                             s.sameDevice ? L" (one device, one clock: no drift correction needed)" : L"");
                }
                state += line;
                // shared WASAPI takes a short period only in Windows' low-latency mode (a multiple of the device's
                // own step, at its own rate); else Windows runs longer ones - say so (fifth outside review). Other
                // APIs and exclusive mode round to the device's own sizes: said without the reason (sixth review)
                const double asked = s.deviceRate > 0 ? s.askedFrames * s.deviceRate / audio_.sampleRate() : s.askedFrames;
                if (s.askedFrames && std::fabs(s.periodFrames - asked) > 1.0) {
                    const bool shared = audio_.sys().api() == AudioApi::Wasapi && !settings_.exclusive;
                    swprintf(line, 480, shared ? L"; Windows runs %u-frame periods, not the %u asked for (its "
                                                 L"low-latency mode does not take this size or rate)"
                                               : L"; the device runs %u-frame periods, not the %u asked for",
                             s.periodFrames, s.askedFrames);
                    state += line;
                }
            }
        }
    }
    if (!audio_.deviceError().empty()) state += L"\n" + fromUtf8(audio_.deviceError());
    if (!notes_.empty()) state += L"\n" + notes_;
    const EngineHealth h = audio_.health();
    wchar_t hb[400];
    long long resyncs = 0, overloads = 0;
#if AP_HAVE_ASIO
    resyncs = audio_.host().resyncs();
    overloads = audio_.host().overloads();
#endif
    swprintf(hb, 400,
             L"Periods %lld   heaviest %.1f %% of its time   last %.1f %%\nOver half their time %lld   late "
             L"(dropout) %lld   answered with silence %lld\nDriver resyncs %lld   overloads %lld",
             running ? h.blocks : 0LL, running ? h.heaviestPercent : 0.0, running ? h.lastPercent : 0.0,
             running ? h.overHalf : 0LL, running ? h.late : 0LL, running ? h.refused : 0LL, resyncs, overloads);
    SetWindowTextW(info_, info.c_str());
    if (state != stateShown_) {
        stateShown_ = state;
        SetWindowTextW(state_, state.c_str());
        // the counters right under the text: lay out again when it takes another number of lines
        RECT sr;
        GetClientRect(state_, &sr);
        int lines = stateLines_;
        if (HDC dc = GetDC(state_)) {
            HGDIOBJ old = SelectObject(dc, font_);
            TEXTMETRICW tm;
            GetTextMetricsW(dc, &tm);
            RECT r{0, 0, sr.right, 0};
            DrawTextW(dc, state.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
            const int lh = std::max(1, static_cast<int>(tm.tmHeight + tm.tmExternalLeading));
            lines = std::max(1, static_cast<int>((r.bottom + lh - 1) / lh));
            SelectObject(dc, old);
            ReleaseDC(state_, dc);
        }
        if (lines != stateLines_ && sr.right > 0) {
            stateLines_ = lines;
            layout();
        }
    }
    SetWindowTextW(health_, hb);
}

} // namespace ap
