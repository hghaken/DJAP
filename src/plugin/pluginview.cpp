#include "pluginview.h"

#include "meterview.h"
#include "plugin.h"
#include "pluginids.h"
#include "plugindiag.h"
#include "stages.h"
#include "version.h"

#include "pluginterfaces/gui/iplugview.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

extern HINSTANCE ghInst; // the plug-in DLL (public.sdk/source/main/dllmain.cpp)

using namespace Steinberg;

namespace ap::plugin {

namespace {

const wchar_t* const kClass = L"DJAPPluginView";
const double kLooks[] = {0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0}; // the Setup window's choices (ms)
enum Id { idPreset = 1, idColour, idLook, idBypass, idSlider0 = 10, idMeter = 20 };
enum Slider { slLevel, slEnhancer, slWidth, slFreq, kSliders };
const wchar_t* const kSliderName[kSliders] = {L"Bass level", L"Bass enhancer", L"Bass width", L"Bass frequency"};
const int kSliderMin[kSliders] = {-24, 0, 0, 8}, kSliderMax[kSliders] = {18, 20, 20, 50}; // 0.5 dB / 5 % / 5 Hz
const wchar_t* const kSliderTip[kSliders] = {
    L"More or less bass, -12 .. +9 dB: the colour's bass shelf below the bass frequency. The leveller keeps the "
    L"loudness on target, so this changes the balance, not the volume. 0 dB = off.",
    L"Strength of the bass enhancer, 0 .. 100 %: overtones of the bass below its frequency, so the bass is heard on "
    L"small speakers too. 0 % switches the enhancer off.",
    L"Stereo width of the bass below the bass frequency, 0 .. 100 %: 0 % = bass in mono (tighter in a club, safe "
    L"for mono systems), 100 % = as recorded. Moving it switches the width stage on.",
    L"Where 'bass' ends, 40 .. 250 Hz: the colour's bass corner and, within their ranges, the enhancer's frequency "
    L"and the width stage's bass split, all at once.",
};

int g_views = 0; // open windows of this DLL: the classes and GDI+ live while one is open

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

ViewRect PluginView::sizeFor(UINT dpi) {
    return ViewRect(0, 0, MulDiv(740, static_cast<int>(dpi), 96), MulDiv(640, static_cast<int>(dpi), 96));
}

PluginView::PluginView(Plugin* plugin) : plugin_(plugin) {
    const ViewRect r = sizeFor(GetDpiForSystem());
    setRect(r);
}

PluginView::~PluginView() {
    if (plugin_) plugin_->viewClosed(this);
}

void PluginView::registerClasses(HINSTANCE instance) {
    theme::init();
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    registerMeterView(instance);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
}

void PluginView::unregisterClasses() {
    unregisterMeterView();
    UnregisterClassW(kClass, ghInst);
    theme::shutdown();
}

tresult PLUGIN_API PluginView::isPlatformTypeSupported(FIDString type) {
    return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API PluginView::attached(void* parent, FIDString type) {
    if (isPlatformTypeSupported(type) != kResultTrue || !parent) return kResultFalse;
    if (g_views++ == 0) registerClasses(ghInst);
    const tresult r = CPluginView::attached(parent, type);
    dpi_ = GetDpiForWindow(static_cast<HWND>(parent));
    if (dpi_ == 0) dpi_ = 96;
    ViewRect want = sizeFor(dpi_);
    if (want.getWidth() != rect.getWidth() || want.getHeight() != rect.getHeight()) {
        setRect(want);
        if (plugFrame) plugFrame->resizeView(this, &want);
    }
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"DJAP", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                            rect.getWidth(), rect.getHeight(), static_cast<HWND>(parent), nullptr, ghInst, this);
    if (!hwnd_) return kResultFalse;
    create();
    layout();
    fill();
    SetTimer(hwnd_, 1, 50, nullptr);
    return r;
}

tresult PLUGIN_API PluginView::removed() {
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    font_ = bold_ = nullptr;
    if (--g_views == 0) unregisterClasses();
    return CPluginView::removed();
}

LRESULT CALLBACK PluginView::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* v = reinterpret_cast<PluginView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (v && v->hwnd_ == hwnd) return v->handle(msg, wp, lp);
    if (v && !v->hwnd_ && msg != WM_NCDESTROY) { // during CreateWindowExW: the handle is not stored yet
        LRESULT result = 0;
        if (v->metal_.handle(hwnd, msg, wp, lp, result)) return result;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PluginView::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT result = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, result)) return result;
    switch (msg) {
    case WM_TIMER:
        if (dirty_) fill();
        meters();
        if (++ticks_ % 10 == 0) status(); // twice a second
        return 0;
    case WM_HSCROLL:
        if (lp) slider(reinterpret_cast<HWND>(lp), LOWORD(wp));
        return 0;
    case WM_COMMAND:
        command(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_DPICHANGED_AFTERPARENT:
        dpi_ = GetDpiForWindow(hwnd_);
        layout();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void PluginView::create() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ghInst, nullptr);
    };
    title_ = make(L"STATIC", L"DJAP", SS_LEFT | SS_CENTERIMAGE, 0);
    version_ = make(L"STATIC", (L"DJ Audio Processor " + wide(AP_VERSION_STRING)).c_str(),
                    SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0);
    lookLabel_ = make(L"STATIC", L"Look-ahead", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0);
    look_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idLook);
    bypass_ = make(L"BUTTON", L"Bypass", BS_OWNERDRAW | WS_TABSTOP, idBypass);
    presetLabel_ = make(L"STATIC", L"Preset", SS_LEFT | SS_CENTERIMAGE, 0);
    preset_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idPreset);
    colourLabel_ = make(L"STATIC", L"Colour", SS_LEFT | SS_CENTERIMAGE, 0);
    colour_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idColour);
    for (int k = 0; k < kSliders; ++k) {
        sliderLabel_[k] = make(L"STATIC", kSliderName[k], SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY, 0);
        slider_[k] = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | TBS_BOTTOM | WS_TABSTOP, idSlider0 + k);
        SendMessageW(slider_[k], TBM_SETRANGEMIN, FALSE, kSliderMin[k]);
        SendMessageW(slider_[k], TBM_SETRANGEMAX, FALSE, kSliderMax[k]);
        SendMessageW(slider_[k], TBM_SETTICFREQ, k == slLevel ? 4 : 2, 0);
        SendMessageW(slider_[k], TBM_SETLINESIZE, 0, 1);
        SendMessageW(slider_[k], TBM_SETPAGESIZE, 0, k == slLevel ? 4 : 2);
        sliderValue_[k] = make(L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE | SS_NOTIFY, 0);
    }
    meter_ = createMeterView(hwnd_, idMeter);
    status_ = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY | SS_ENDELLIPSIS, 0);
    theme::makeButton(bypass_);
    for (HWND c : {preset_, colour_, look_}) theme::makeCombo(c);

    // the lists: the program's presets and colours (its settings file), and one for values that match none
    for (const Preset& p : plugin_->presets())
        SendMessageW(preset_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(p.name).c_str()));
    SendMessageW(preset_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(other values)"));
    for (const Preset& p : plugin_->colours())
        SendMessageW(colour_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(p.name).c_str()));
    SendMessageW(colour_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(other values)"));
    for (double l : kLooks) {
        wchar_t t[48];
        swprintf(t, 48, l == 1.5 ? L"%.1f ms (default)" : L"%.1f ms", l);
        SendMessageW(look_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    }

    tips_.create(hwnd_);
    tips_.add(status_, L"What the host does with this plug-in: which instance this is of how many, the host's sample "
                       L"rate and block size, whether it hands over the same buffers for input and output, how often it "
                       L"calls the processing, how many parameter changes reached it, and the peak level going in and "
                       L"coming out. Every host call is also written to DJAP-plugin.log in the TEMP folder.");
    tips_.add(version_, L"DJAP processes this channel like one deck of the program, with the same presets and "
                        L"colours. Every value is also a parameter in the host's own list, and can be automated.");
    tips_.add(preset_, L"The preset: the target loudness and every stage's values. The list holds the program's "
                       L"presets (also your own, from its settings file). The project keeps the values themselves, "
                       L"so it sounds the same on a computer without that preset; \"(other values)\" = values that "
                       L"match no preset in the list.");
    tips_.add(colour_, L"The colour, on top of the preset: tone, auto tone, sub bass and a loudness contour. "
                       L"Neutral changes nothing.");
    tips_.add(look_, L"The limiter's look-ahead: the only delay the processing adds (the host is told, so it can "
                     L"keep this channel in time with the others). Longer handles peaks more gently; true-peak "
                     L"limiting needs at least 1.0 ms; 0 ms = no delay, sample peaks only.");
    tips_.add(lookLabel_, L"The limiter's look-ahead: the only delay the processing adds.");
    tips_.add(bypass_, L"Bypass (lit): the input goes out unchanged, delayed by the same look-ahead, so switching "
                       L"never shifts the sound in time.");
    for (int k = 0; k < kSliders; ++k)
        for (HWND h : {sliderLabel_[k], slider_[k], sliderValue_[k]}) tips_.add(h, kSliderTip[k]);
}

void PluginView::layout() {
    if (!hwnd_) return;
    if (font_) DeleteObject(font_);
    if (bold_) DeleteObject(bold_);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    bold_ = CreateFontIndirectW(&lf);
    for (HWND h = GetWindow(hwnd_, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(h == title_ ? bold_ : font_), FALSE);
    tips_.setDpi(dpi_);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int m = px(12), gap = px(6), rowH = px(28), W = rc.right - 2 * m;
    int y = m;
    // DJAP  DJ Audio Processor 0.1.x ........ Look-ahead [....] [Bypass]
    const int bypassW = px(90), lookW = px(150), lookLabelW = px(78);
    MoveWindow(title_, m, y, px(70), rowH, TRUE);
    MoveWindow(version_, m + px(74), y + px(2), W - px(74) - bypassW - lookW - lookLabelW - 3 * gap, rowH, TRUE);
    MoveWindow(lookLabel_, m + W - bypassW - lookW - lookLabelW - 2 * gap, y, lookLabelW, rowH, TRUE);
    MoveWindow(look_, m + W - bypassW - lookW - gap, y, lookW, px(300), TRUE);
    MoveWindow(bypass_, m + W - bypassW, y, bypassW, rowH, TRUE);
    y += rowH + gap;
    // Preset [....]  Colour [....]
    const int labelW = px(48), colourLabelW = px(50);
    const int combos = W - labelW - colourLabelW - 3 * gap - px(10);
    const int presetW = combos / 2, colourW = combos - presetW;
    int x = m;
    MoveWindow(presetLabel_, x, y, labelW, rowH, TRUE);
    x += labelW + gap;
    MoveWindow(preset_, x, y, presetW, px(300), TRUE);
    x += presetW + gap + px(10);
    MoveWindow(colourLabel_, x, y, colourLabelW, rowH, TRUE);
    x += colourLabelW + gap;
    MoveWindow(colour_, x, y, colourW, px(300), TRUE);
    y += rowH + gap;
    // bass sliders: two columns of label, slider, value
    const int colW = (W - px(16)) / 2, nameW = px(96), valueW = px(70);
    for (int k = 0; k < kSliders; ++k) {
        const int cx = m + (k % 2) * (colW + px(16)), cy = y + (k / 2) * (rowH + px(4));
        MoveWindow(sliderLabel_[k], cx, cy, nameW, rowH, TRUE);
        MoveWindow(slider_[k], cx + nameW, cy, colW - nameW - valueW, rowH, TRUE);
        MoveWindow(sliderValue_[k], cx + colW - valueW, cy, valueW, rowH, TRUE);
    }
    y += 2 * (rowH + px(4)) + px(2);
    MoveWindow(meter_, m, y, W, rc.bottom - m - y - px(22), TRUE);
    MoveWindow(status_, m, rc.bottom - m - px(20), W, px(20), TRUE);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void PluginView::fill() {
    dirty_ = false;
    if (!hwnd_) return;
    const int pi = plugin_->presetIndex(), ci = plugin_->colourIndex();
    if (SendMessageW(preset_, CB_GETCURSEL, 0, 0) != pi) SendMessageW(preset_, CB_SETCURSEL, static_cast<WPARAM>(pi), 0);
    if (SendMessageW(colour_, CB_GETCURSEL, 0, 0) != ci) SendMessageW(colour_, CB_SETCURSEL, static_cast<WPARAM>(ci), 0);
    const double look = plugin_->plainOf(kLookId);
    int li = 0;
    for (int i = 0; i < static_cast<int>(std::size(kLooks)); ++i)
        if (std::fabs(kLooks[i] - look) < std::fabs(kLooks[li] - look)) li = i;
    if (SendMessageW(look_, CB_GETCURSEL, 0, 0) != li) SendMessageW(look_, CB_SETCURSEL, static_cast<WPARAM>(li), 0);
    theme::setChecked(bypass_, plugin_->plainOf(kBypassId) >= 0.5);

    const PresetValues v = plugin_->currentValues();
    const int pos[kSliders] = {static_cast<int>(std::lround(v[pToneBass] * 2.0)),
                               v[pBassOn] >= 0.5 ? static_cast<int>(std::lround(v[pBassAmount] / 5.0)) : 0,
                               static_cast<int>(std::lround(v[pBassWidth] / 5.0)),
                               static_cast<int>(std::lround(v[pToneBassFreq] / 5.0))};
    wchar_t t[kSliders][40];
    if (v[pToneBass] == 0.0) swprintf(t[slLevel], 40, L"0 dB (off)");
    else swprintf(t[slLevel], 40, L"%+.1f dB", v[pToneBass]);
    if (v[pBassOn] >= 0.5) swprintf(t[slEnhancer], 40, L"%.0f %%", v[pBassAmount]);
    else swprintf(t[slEnhancer], 40, L"off");
    if (v[pWidthOn] >= 0.5) swprintf(t[slWidth], 40, v[pBassWidth] == 0.0 ? L"mono" : L"%.0f %%", v[pBassWidth]);
    else swprintf(t[slWidth], 40, L"%.0f %% (off)", v[pBassWidth]);
    swprintf(t[slFreq], 40, L"%.0f Hz", v[pToneBassFreq]);
    for (int k = 0; k < kSliders; ++k) {
        if (SendMessageW(slider_[k], TBM_GETPOS, 0, 0) != pos[k]) SendMessageW(slider_[k], TBM_SETPOS, TRUE, pos[k]);
        SetWindowTextW(sliderValue_[k], t[k]);
    }
}

void PluginView::meters() {
    if (!meter_) return;
    monitor_.poll(plugin_->deck(), plugin_->active() ? plugin_->sampleRate() : 0.0, 0.05);
    const PresetValues v = plugin_->currentValues();
    const bool processing = plugin_->plainOf(kBypassId) < 0.5;
    MeterViewData data;
    data.m = monitor_.meters();
    data.target = v[pTarget];
    data.ceiling = v[pLimCeiling];
    data.levRange = v[pLevRange];
    data.inputGain = processing ? v[pInputGain] : 0.0;
    if (data.m.bands == 0) data.m.bands = static_cast<int>(v[pDensBands]);
    for (int k = 0; k < kStageCount; ++k) data.stageOn[k] = v[kStageParam[k]] >= 0.5;
    data.processing = processing;
    data.audio = plugin_->active();
    const DeckAnalysis& an = monitor_.analysis();
    data.spectrum = data.audio && data.m.live && an.specOut.frames() > 0;
    data.specIn = an.specIn.points();
    data.specOut = an.specOut.points();
    for (int k = 0; k < kMaxSplits; ++k) data.splits[k] = v[static_cast<size_t>(pDensSplit1 + k)];
    setMeterView(meter_, data);
}

void PluginView::status() {
    if (!status_) return;
    const ProcessStats& s = plugin_->stats();
    const uint64_t calls = s.calls.load(), changes = s.changes.load();
    auto db = [](float x) { return x > 0 ? 20.0 * std::log10(static_cast<double>(x)) : -150.0; };
    wchar_t t[512];
    swprintf(t, 512,
             L"Instance #%d of %d   |   %hs   |   %.1f kHz, %d frames, %hs, %hs   |   %.0f calls/s   |   changes: %llu from the "
             L"host, %llu from the window   |"
             L"   in %.1f / out %.1f dBFS",
             plugin_->id(), diagLiveInstances(), plugin_->hostName().empty() ? "host?" : plugin_->hostName().c_str(),
             plugin_->sampleRate() / 1000.0, s.frames.load(), sampleSizeName(s.sampleSize.load()),
             s.inPlace.load() == 1 ? "in place" : s.inPlace.load() == 0 ? "separate buffers" : "no audio yet",
             static_cast<double>(calls - lastCalls_) / 0.5, static_cast<unsigned long long>(changes),
             static_cast<unsigned long long>(s.updates.load()),
             db(s.inPeak.load()), db(s.outPeak.load()));
    lastCalls_ = calls;
    SetWindowTextW(status_, t);
}

void PluginView::slider(HWND bar, int code) {
    (void)code; // every move is sent to the host (it records automation while the slider is dragged)
    for (int k = 0; k < kSliders; ++k) {
        if (slider_[k] != bar) continue;
        const int pos = static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0));
        switch (k) {
        case slLevel:
            plugin_->editPlain(paramId(pToneBass), pos * 0.5);
            break;
        case slEnhancer:
            plugin_->editPlain(paramId(pBassAmount), pos * 5.0);
            plugin_->editPlain(paramId(pBassOn), pos > 0 ? 1.0 : 0.0);
            break;
        case slWidth:
            plugin_->editPlain(paramId(pBassWidth), pos * 5.0);
            plugin_->editPlain(paramId(pWidthOn), 1.0);
            break;
        default: { // the bass frequency of all three, each within its own range
            const double f = pos * 5.0;
            plugin_->editPlain(paramId(pToneBassFreq), f);
            plugin_->editPlain(paramId(pBassFreq), std::min(paramInfo(pBassFreq).max, std::max(paramInfo(pBassFreq).min, f)));
            plugin_->editPlain(paramId(pWidthSplit), std::min(paramInfo(pWidthSplit).max, std::max(paramInfo(pWidthSplit).min, f)));
            break;
        }
        }
        fill();
        return;
    }
}

void PluginView::command(int id, int code) {
    if ((id == idPreset || id == idColour) && code == CBN_SELCHANGE) {
        const bool colour = id == idColour;
        const int sel = static_cast<int>(SendMessageW(colour ? colour_ : preset_, CB_GETCURSEL, 0, 0));
        plugin_->choose(colour, sel);
        fill();
    } else if (id == idLook && code == CBN_SELCHANGE) {
        const int sel = static_cast<int>(SendMessageW(look_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(std::size(kLooks))) plugin_->edit(kLookId, kLooks[sel] / 5.0);
    } else if (id == idBypass && code == BN_CLICKED) {
        plugin_->edit(kBypassId, plugin_->plainOf(kBypassId) >= 0.5 ? 0.0 : 1.0);
        fill();
    }
}

} // namespace ap::plugin
