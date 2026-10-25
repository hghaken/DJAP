#include "editwnd.h"

#include "appstate.h"
#include "placement.h"
#include "resource.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace ap {

namespace {

const wchar_t* const kClass = L"APEditWindow";
enum Id { idPreset = 10, idSaveAs, idRename, idDelete, idResetPreset, idTabs, idRow0 = 100 };
// the controls of row r have the ids idRow0 + kRowIds * r + part
enum RowPart { rpSlider, rpCheck, rpValue, rpReset, kRowIds };
constexpr int kLogPositions = 300; // slider positions of a log-scaled parameter

std::string number(double v, int decimals) {
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

// The text a value is shown (and typed) as.
std::string valueText(int p, double v) {
    const ParamInfo& in = paramInfo(p);
    if (in.isSwitch) return v >= 0.5 ? "on" : "off";
    if (p == pCompMakeup && v < 0) return "auto";
    if (p == pCompSideHpf && v <= 0) return "off";
    return number(v, in.decimals);
}

std::string rangeText(int p) {
    const ParamInfo& in = paramInfo(p);
    if (in.isSwitch) return "on / off";
    std::string r = number(in.min, in.decimals) + " .. " + number(in.max, in.decimals);
    if (p == pCompMakeup) r = "auto, 0 .. " + number(in.max, in.decimals);
    if (p == pCompSideHpf) r = "off, 40 .. " + number(in.max, in.decimals);
    return r;
}

// Typed text -> value: a comma counts as the decimal sign; words for switches and the special values.
bool parseValue(int p, std::wstring t, double& out) {
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    for (wchar_t& c : t) c = static_cast<wchar_t>(towlower(c));
    const ParamInfo& in = paramInfo(p);
    if (in.isSwitch) {
        if (t == L"on" || t == L"aan" || t == L"1" || t == L"yes") return out = 1, true;
        if (t == L"off" || t == L"uit" || t == L"0" || t == L"no") return out = 0, true;
        return false;
    }
    if (p == pCompMakeup && t == L"auto") return out = -1, true;
    if (p == pCompSideHpf && t == L"off") return out = 0, true;
    for (wchar_t& c : t)
        if (c == L',') c = L'.';
    if (t.empty()) return false;
    wchar_t* end = nullptr;
    out = std::wcstod(t.c_str(), &end);
    return end && *end == L'\0' && std::isfinite(out);
}

bool visible(int p, const PresetValues& v, PresetKind kind) {
    if (kindOf(p) != kind) return false; // a preset shows its dynamics values, a colour preset its colour values
    const int bands = static_cast<int>(v[pDensBands]);
    if (p >= pDensSplit1 && p <= pDensSplit6) return p - pDensSplit1 < bands - 1;
    if (p >= pDensGain1 && p <= pDensGain7) return p - pDensGain1 < bands;
    return true;
}

// The stage switch of a stage (its "On" button in the bar above the tabs), or -1.
int stageSwitch(const std::string& stage) {
    for (int k = 0; k < kStageCount; ++k)
        if (stage == paramInfo(kStageParam[k]).stage) return kStageParam[k];
    return -1;
}

bool isStageSwitch(int p) {
    for (int k = 0; k < kStageCount; ++k)
        if (kStageParam[k] == p) return true;
    return false;
}

// What "reset" goes back to: the shipped values (or the generic defaults for an own preset), with the default
// splits of the preset's band count when that differs (the same rule as the settings file).
PresetValues referenceValues(const std::string& preset, const PresetValues& now, PresetKind kind) {
    PresetValues ref = Settings::shippedValues(preset, kind);
    if (kind == PresetKind::Colour) return ref;
    // the band count itself keeps its own reset value; only the splits follow the band count in use
    if (ref[pDensBands] != now[pDensBands]) loadDefaultSplits(ref, static_cast<int>(now[pDensBands]));
    return ref;
}

// What a stage does to the sound, shown under its tab's sliders: what you hear, and what more or less of its
// values sounds like.
struct StageSound {
    const char* stage;
    const wchar_t* text;
};
const StageSound kStageSound[] = {
    {"General",
     L"Target loudness sets how loud the deck plays: every song is brought to it, so a higher target (-10) sounds "
     L"louder and denser, a lower one (-18) quieter with more of the music's dynamics left. Input gain drives the "
     L"compressor, clipper and limiter harder: more punch and density, but high values squash the sound and make it "
     L"flat. Output gain only turns the result down - to match another source without changing the sound."},
    {"Leveller",
     L"The slow hand on the volume: quiet and loud songs come out at the same loudness, while the sound within a song "
     L"is left alone. A larger range evens out bigger differences (a quiet old record comes up, a hot master comes "
     L"down). A higher speed reaches a new song's level sooner but can be heard as the volume drifting within a "
     L"song; a lower one is inaudible but takes longer. Fast down only acts when a song comes in clearly too loud "
     L"(after a quiet intro or a break): higher brings it down sooner, lower lets the loud start through longer. "
     L"Hold below keeps fades and breakdowns from being pumped up; catch-up brings a song to level within a second "
     L"after a pause."},
    {"Compressor",
     L"Evens out the loud parts within a song. A lower threshold and a higher ratio make the sound more even and "
     L"more 'in your face'; too much flattens the dynamics and sounds tired. A short attack catches drum hits (less "
     L"punch), a longer one lets them through (more punch). A short release sounds lively but can pump, a long one "
     L"is smooth. Bass under the side-chain high-pass does not make it work, so a heavy kick does not pull the whole "
     L"mix down."},
    {"Density",
     L"Works per frequency band: a band that sticks out is turned down, one that falls away is lifted, so the sound "
     L"gets denser, fuller and more even over the whole spectrum - the typical radio sound. More amount and more "
     L"lift give more density and loudness but less natural dynamics, and at extremes a flat, breathing sound. More "
     L"bands act more selectively (the bass no longer pushes the voice around). The band gains are a tone control: "
     L"more low end, less harshness."},
    {"Bass enhancer",
     L"Adds overtones to the bass, so it is heard on small speakers and laptops without more low-frequency energy. "
     L"A higher frequency makes the bass more audible but also boomier; more amount gives more audible bass and "
     L"warmth, too much sounds muddy or distorted."},
    {"Exciter",
     L"Adds overtones to the highs: shine, air and presence. A lower frequency also brightens voices and snares (it "
     L"can turn harsh), a higher one only adds air. Too much sounds hissy and brittle, especially on old or "
     L"low-bitrate recordings."},
    {"De-esser",
     L"Turns sharp 's', 'sh' and 't' sounds down when they stand out, so bright mixes and the exciter do not get "
     L"piercing. A lower threshold acts more often, a higher maximum takes more off. Too much makes voices lisp and "
     L"sound dull."},
    {"Stereo width",
     L"Makes the stereo image wider or narrower. Above 100 % it sounds wider and more spacious but can sound hollow "
     L"and loses power in mono; below 100 % the sound moves to the centre. Bass width keeps the bass below the split "
     L"narrower or mono: a tighter, punchier low end in a club, and safe on mono sound systems."},
    {"Soft clipper",
     L"Rounds off the highest peaks before the limiter, without delay. More drive gives more loudness and density "
     L"but takes punch off the drums, and pushed hard it sounds rough or distorted."},
    {"Limiter",
     L"The safety net: the output never passes the ceiling (true peak), so nothing clips in the next device or in a "
     L"stream encoder. A lower ceiling leaves more room for converters and lossy encoding and plays a little "
     L"quieter. A short release recovers fast (louder, but it can distort the bass); a long one is cleaner but can "
     L"duck the sound after a peak."},
    {"Tone",
     L"A fixed EQ on top of the preset. Bass and treble are shelves: more bass gives warmth and weight, less tightens "
     L"a boomy mix; more treble gives clarity and air, less softens a harsh or noisy recording. Low-mid is the boom "
     L"and mud area (take some out for a cleaner sound); presence is where voices and leads sit (up = forward and "
     L"clear, down = softer, further back). The leveller keeps the loudness, so only the balance changes."},
    {"Auto tone",
     L"Listens to each song and pulls its balance towards that of a well-balanced production: a thin old record gets "
     L"more bass, a dull one more highs, a boomy one less low end. Amount sets how far it pulls, maximum how much it "
     L"may change a band, time how quickly it settles on a new song. It holds during breakdowns and fades, so a "
     L"quiet part does not change the sound."},
    {"Sub bass",
     L"Adds a tone an octave below the bass: deep bass a recording does not have (many records from the 60s and 70s "
     L"were cut without it). It is felt more than heard on large speakers and is nearly inaudible on small ones. Use "
     L"little: too much makes the low end rumble and takes headroom."},
    {"Loudness",
     L"For quiet listening: ears hear less bass and treble at a low volume, so this adds deep bass (up to +9 dB) and "
     L"highs (up to +4 dB) to keep the sound full when the volume is low. At a normal or loud volume, leave it off - "
     L"it would sound boomy."},
};

const wchar_t* stageSound(const std::string& stage) {
    for (const StageSound& s : kStageSound)
        if (stage == s.stage) return s.text;
    return L"";
}

// What each shipped preset and colour does to the sound, shown under the choice (values: params.cpp, docs/design.md
// 3.1 and 3.3).
const StageSound kShippedSound[] = {
    {"Gentle",
     L"Easy listening at -18 LUFS: songs are brought to one loudness slowly and gently, and most of the music's "
     L"dynamics stay. Light compression, 3 density bands, no clipping. For background music and quiet rooms."},
    {"Light",
     L"-16 LUFS: a little more even and present than Gentle, still natural. Gentle compression, 4 density bands, the "
     L"de-esser and a touch of clipping."},
    {"Medium",
     L"-14 LUFS, the usual loudness of streaming services: even and full without sounding squashed. 5 density bands, "
     L"a little exciter, the de-esser. A good start for most music."},
    {"Loud",
     L"-12 LUFS: dense and punchy. Earlier and harder compression, 6 density bands, bass enhancer and exciter, more "
     L"clipping. For parties and noisy rooms."},
    {"Very Loud",
     L"-10 LUFS, close to modern club masters: very dense and loud with little dynamics left. 7 density bands, the "
     L"strongest compression and clipping. For loud venues; on long listening it can tire."},
    {"FM Station",
     L"The sound of an FM station: processed like a loud one - a fast, dense 5-band, a full low end, a slightly wider "
     L"image, hard clipping, the highs held down firmly (the pre-emphasis leaves little room up there) - then turned "
     L"down 4 dB to about -13 LUFS, as the transmitter's power limit does: dense and present, peaks only about 8 dB "
     L"over the loudness. It sounds like FM; it is not a signal ready for a transmitter (no 15 kHz low-pass, no "
     L"stereo coder)."},
    {"DAB+",
     L"Digital radio the EBU R 128 way, -23 LUFS and at most -1 dBTP: the music keeps its dynamics - songs are "
     L"shifted to the target slowly instead of squashed, only passages over -18 LUFS are compressed, no density, no "
     L"clipping and no exciter (a low bit-rate codec smears both). Quiet next to the other presets: turn the "
     L"receiver up."},
    {"Web Stream",
     L"Internet radio, podcasts, streams and MP3 / AAC files, -16 LUFS: even and full like Light, but without clipping and with 2 dB "
     L"of room below full scale (ceiling -2 dBTP) - MP3 and AAC coding raise the peaks a little and turn clipping "
     L"into audible distortion. Music services turn louder streams down anyway."},
    {"Neutral", L"Leaves the tone as it is: no EQ, no auto tone, no sub bass, no loudness contour."},
    {"Warm",
     L"Fuller and softer: +3 dB of bass below 100 Hz, a little less low-mid (-1 dB around 350 Hz) and less treble "
     L"(-1.5 dB above 8 kHz). For bright or thin mixes and long listening."},
    {"Bass+",
     L"More bass: +5 dB below 100 Hz, the rest unchanged. The leveller keeps the overall loudness, so the rest of the "
     L"music sits a little lower against the bass."},
    {"Less mid",
     L"Cleaner and less boxy: -3 dB of low-mid around 350 Hz and -1.5 dB of presence around 3 kHz. Takes the boom, mud "
     L"and harshness out of live and older recordings."},
    {"Bright",
     L"Clearer and more open: +2 dB of presence around 3 kHz and +3 dB of treble above 8 kHz. For dull recordings; on "
     L"bright ones it gets sharp."},
    {"Vocal",
     L"Voices forward: -1 dB of bass, -2 dB of low-mid and +2.5 dB of presence. For speech, vocals and karaoke rather "
     L"than bass-heavy dance music."},
    {"Oldies",
     L"For thin, flat old recordings (60s, 70s): the auto tone at 70 % pulls each song towards a modern balance (more "
     L"bass and highs where they are missing), a little sub bass (20 %) adds the deep octave old records lack, and "
     L"bass and treble get +1 dB."},
    {"Auto",
     L"Only the auto tone, at 100 %: each song is pulled towards a well-balanced production - a thin record gets more "
     L"bass, a dull one more highs, a boomy one less low end - by up to 6 dB (treble 3 dB)."},
    {"Quiet",
     L"For listening at a low volume: the loudness contour at 60 % adds deep bass (about +5 dB) and highs (about "
     L"+2.5 dB), so the music stays full when it plays softly."},
};

std::wstring signedDb(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%+.1f dB", v);
    return b;
}

// The line under the choice: a shipped preset's or colour's description (and whether values were changed), or a
// summary of an own one's values.
std::wstring describe(const Preset& p, PresetKind kind) {
    const PresetValues& v = p.values;
    if (p.shipped) {
        std::wstring s;
        for (const StageSound& x : kShippedSound)
            if (p.name == x.stage) s = x.text;
        const PresetValues ref = Settings::shippedValues(p.name, kind);
        for (int i = 0; i < kParamCount; ++i)
            if (kindOf(i) == kind && std::fabs(v[static_cast<size_t>(i)] - ref[static_cast<size_t>(i)]) > 1e-9 &&
                (kind == PresetKind::Colour || i < pDensSplit1 || i > pDensSplit6 || v[pDensBands] == ref[pDensBands])) {
                s += L" Some values are changed (bold).";
                break;
            }
        return s;
    }
    std::wstring s;
    auto add = [&](const std::wstring& part) { s += (s.empty() ? L"" : L", ") + part; };
    if (kind == PresetKind::Colour) {
        const struct {
            Param p;
            const wchar_t* name;
        } tone[] = {{pToneBass, L"bass "}, {pToneLowMid, L"low-mid "}, {pTonePresence, L"presence "}, {pToneTreble, L"treble "}};
        for (const auto& t : tone)
            if (v[t.p] != 0.0) add(t.name + signedDb(v[t.p]));
        wchar_t b[64];
        if (v[pAutoAmount] > 0) swprintf(b, 64, L"auto tone %.0f %%", v[pAutoAmount]), add(b);
        if (v[pSubAmount] > 0) swprintf(b, 64, L"sub bass %.0f %%", v[pSubAmount]), add(b);
        if (v[pLoudness] > 0) swprintf(b, 64, L"loudness %.0f %%", v[pLoudness]), add(b);
        return L"Your own colour: " + (s.empty() ? std::wstring(L"it changes nothing (as Neutral).") : s + L".");
    }
    wchar_t b[96];
    swprintf(b, 96, L"target %.1f LUFS", v[pTarget]);
    add(b);
    std::wstring on;
    for (int k = 0; k < kStageCount; ++k)
        if (v[kStageParam[k]] >= 0.5) {
            std::wstring stage = fromUtf8(paramInfo(kStageParam[k]).stage);
            stage[0] = static_cast<wchar_t>(towlower(stage[0]));
            if (kStageParam[k] == pDensOn) swprintf(b, 96, L" (%d bands)", static_cast<int>(v[pDensBands])), stage += b;
            if (kStageParam[k] == pLimOn) swprintf(b, 96, L" (ceiling %.1f dBTP)", v[pLimCeiling]), stage += b;
            on += (on.empty() ? L"" : L", ") + stage;
        }
    return L"Your own preset: " + s + L"; on: " + (on.empty() ? std::wstring(L"nothing") : on) + L".";
}

// How a parameter's slider maps positions to values: linear in the parameter's steps; a log scale for times and
// frequencies that span a factor of 8 or more (density splits 20 Hz - 16 kHz, releases); position 0 is "auto" for
// the make-up and "off" for the side-chain high-pass.
struct Scale {
    bool log = false, special = false;
    double lo = 0.0, hi = 1.0;
    int positions = 1; // the highest position
};

Scale scaleOf(int p) {
    const ParamInfo& in = paramInfo(p);
    Scale s;
    s.lo = in.min;
    s.hi = in.max;
    if (p == pCompMakeup) s.special = true, s.lo = 0.0;
    if (p == pCompSideHpf) s.special = true, s.lo = 40.0;
    const std::string unit = in.unit;
    s.log = (unit == "Hz" || unit == "ms" || unit == "s") && s.lo > 0.0 && s.hi / s.lo >= 8.0;
    s.positions = s.log ? kLogPositions : static_cast<int>(std::lround((s.hi - s.lo) / in.step));
    if (s.special) ++s.positions;
    return s;
}

double valueAt(int p, int pos) {
    const ParamInfo& in = paramInfo(p);
    const Scale s = scaleOf(p);
    if (s.special) {
        if (pos <= 0) return p == pCompMakeup ? -1.0 : 0.0;
        --pos;
    }
    const int n = s.positions - (s.special ? 1 : 0);
    double v = s.log ? std::round(s.lo * std::pow(s.hi / s.lo, static_cast<double>(pos) / n) / in.step) * in.step
                     : s.lo + pos * in.step;
    const double scale = std::pow(10.0, in.decimals);
    v = std::round(v * scale) / scale; // no 0.30000000000000004
    return std::min(s.hi, std::max(s.lo, v));
}

int positionOf(int p, double v) {
    const Scale s = scaleOf(p);
    int off = 0;
    if (s.special) {
        if ((p == pCompMakeup && v < 0.0) || (p == pCompSideHpf && v <= 0.0)) return 0;
        off = 1;
    }
    const int n = s.positions - off;
    const double t = s.log ? std::log(std::max(v, s.lo) / s.lo) / std::log(s.hi / s.lo) * n
                           : (v - s.lo) / paramInfo(p).step;
    return off + static_cast<int>(std::min<long>(n, std::max<long>(0, std::lround(t))));
}

// ---- name prompt ----

struct Ask {
    HWND edit = nullptr;
    std::wstring text;
    bool ok = false, done = false;
};

LRESULT CALLBACK askProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Ask* a = reinterpret_cast<Ask*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_COMMAND:
        if (a && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
            if (LOWORD(wp) == IDOK) {
                const int n = GetWindowTextLengthW(a->edit);
                a->text.assign(static_cast<size_t>(n), L'\0');
                GetWindowTextW(a->edit, a->text.data(), n + 1);
                a->ok = true;
            }
            a->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (a) a->done = true;
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool askName(HWND owner, const std::wstring& title, const std::wstring& prompt, std::string& name) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = askProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"APAskName";
        registered = RegisterClassW(&wc) != 0;
    }
    const UINT dpi = GetDpiForWindow(owner);
    auto px = [dpi](int x) { return MulDiv(x, static_cast<int>(dpi), 96); };
    Ask ask;
    RECT r{0, 0, px(420), px(130)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"APAskName", title.c_str(), style, 0, 0, r.right - r.left,
                               r.bottom - r.top, owner, nullptr, inst, &ask);
    if (!wnd) return false;
    placement::centreOn(wnd, owner);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cx, int cy, int id) {
        HWND c = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text,
                                 WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(cx), px(cy), wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    make(L"STATIC", prompt.c_str(), SS_LEFT, 12, 12, 396, 22, 0);
    ask.edit = make(L"EDIT", fromUtf8(name).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 12, 38, 396, 26, 100);
    make(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 220, 84, 90, 30, IDOK);
    make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 318, 84, 90, 30, IDCANCEL);
    ShowWindow(wnd, SW_SHOW);
    SetFocus(ask.edit);
    SendMessageW(ask.edit, EM_SETSEL, 0, -1);
    EnableWindow(owner, FALSE);
    MSG msg;
    while (!ask.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (!ask.done) PostQuitMessage(0); // WM_QUIT arrived: pass it on
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DeleteObject(font);
    if (ask.ok) name = toUtf8(ask.text);
    return ask.ok;
}

EditWindow::EditWindow(Settings& settings, Changed onChange, PresetKind kind)
    : settings_(settings), onChange_(std::move(onChange)), kind_(kind) {}

EditWindow::~EditWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void EditWindow::show(HWND owner, const std::string& preset, int deck, const std::string& tab) {
    deck_ = deck;
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
        hwnd_ = CreateWindowExW(0, kClass, L"Preset", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, inst, this);
        if (!hwnd_) return;
        RECT r{0, 0, px(1080), px(colour() ? 600 : 780)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        placement::centreOn(hwnd_, owner);
    }
    select(preset);
    if (!tab.empty() && std::find(tabStages_.begin(), tabStages_.end(), tab) != tabStages_.end()) selectTab(tab);
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void EditWindow::refresh() {
    if (!hwnd_) return;
    if (!settings_.find(current_, kind_)) current_ = used(deck_);
    fillPresets();
    showRows();
    updateRows();
    updateButtons();
}

LRESULT CALLBACK EditWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditWindow* self = reinterpret_cast<EditWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<EditWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    const LRESULT r = self->handle(msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->rows_.clear();
        self->tabStages_.clear(); // the next window gets its own tab control: its tabs are added again
        self->tabs_ = nullptr;
    }
    return r;
}

LRESULT EditWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT themed = 0;
    if (metal_.handle(hwnd_, msg, wp, lp, themed)) return themed; // brushed aluminium, 3D buttons
    switch (msg) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(hwnd_);
        createControls();
        setFonts();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {px(820), px(colour() ? 420 : 560)};
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
    case WM_HSCROLL:
        if (lp) slider(reinterpret_cast<HWND>(lp), LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        const NMHDR* h = reinterpret_cast<NMHDR*>(lp);
        if (h->hwndFrom == tabs_ && h->code == TCN_SELCHANGE) {
            const int t = TabCtrl_GetCurSel(tabs_);
            if (t >= 0 && static_cast<size_t>(t) < tabStages_.size()) selectTab(tabStages_[static_cast<size_t>(t)]);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT line = soundLine_;
        DrawEdge(dc, &line, EDGE_ETCHED, BF_TOP);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        for (HFONT f : {font_, bold_, symbol_})
            if (f) DeleteObject(f);
        font_ = bold_ = symbol_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void EditWindow::createControls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    presetLabel_ = make(L"STATIC", colour() ? L"Colour" : L"Preset", SS_LEFT | SS_CENTERIMAGE, 0);
    preset_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, idPreset);
    saveAs_ = make(L"BUTTON", L"Save as...", BS_OWNERDRAW | WS_TABSTOP, idSaveAs);
    rename_ = make(L"BUTTON", L"Rename...", BS_OWNERDRAW | WS_TABSTOP, idRename);
    delete_ = make(L"BUTTON", L"Delete", BS_OWNERDRAW | WS_TABSTOP, idDelete);
    resetPreset_ = make(L"BUTTON", L"Reset preset", BS_OWNERDRAW | WS_TABSTOP, idResetPreset);
    note_ = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOTIFY, 0);
    about_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0); // what the chosen one does to the sound (wraps)
    tabs_ = make(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, idTabs);
    soundTitle_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0);
    sound_ = make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0);

    for (HWND b : {saveAs_, rename_, delete_, resetPreset_}) theme::makeButton(b);
    metal_.makeTabs(tabs_);
    theme::makeCombo(preset_);
    tips_.create(hwnd_);
    tips_.add(tabs_, L"One tab per stage: every value of the stage as a slider, with its value (type one in and press "
                     L"Enter), its unit, a reset button and what it does.");
    if (colour()) {
        tips_.add(preset_, L"The colour preset shown in this window (opened on the deck's own): tone, auto tone, sub "
                           L"bass, loudness. Every change is saved at once and heard on every deck that uses it.");
        tips_.add(saveAs_, L"Saves these values as a colour under a new name (or over one of your own) and moves the "
                           L"deck this window was opened from to it. Shipped colours cannot be saved over.");
        tips_.add(rename_, L"Renames your own colour; the decks that use it follow.");
        tips_.add(delete_, L"Deletes your own colour (asks first); a deck that used it goes to Neutral.");
        tips_.add(resetPreset_, L"Shipped colour: back to its shipped values. Own colour: back to neutral (asks first).");
        tips_.add(note_, L"Whether this is a shipped or an own colour and which decks use it.");
    } else {
        tips_.add(preset_, L"The preset shown in this window (opened on the deck's own). Every change is saved at "
                           L"once and heard on every deck that uses this preset.");
        tips_.add(saveAs_, L"Saves these values under a new name (or over one of your own presets) and moves the "
                           L"deck this window was opened from to it. Shipped presets cannot be saved over.");
        tips_.add(rename_, L"Renames your own preset; the decks that use it follow. Shipped presets keep their names.");
        tips_.add(delete_, L"Deletes your own preset (asks first); a deck that used it goes to Medium. Shipped "
                           L"presets cannot be deleted.");
        tips_.add(resetPreset_, L"Shipped preset: back to its shipped values. Own preset: back to the generic "
                                L"defaults. Every value at once (asks first).");
        tips_.add(note_, L"Whether this is a shipped or an own preset and which decks use it.");
    }
    createRows();
}

// A row for every parameter of the kind; switches (a stage's on / off first in its tab) are check boxes.
void EditWindow::createRows() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    rows_.clear();
    for (int p = 0; p < kParamCount; ++p) {
        if (kindOf(p) != kind_) continue;
        const ParamInfo& in = paramInfo(p);
        const int id = idRow0 + kRowIds * static_cast<int>(rows_.size());
        Row r;
        r.param = p;
        // a stage switch is the first row of its tab, named after the stage
        const std::string name = isStageSwitch(p) ? std::string(in.stage) : std::string(in.name);
        r.name = make(L"STATIC", fromUtf8(name).c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY | SS_ENDELLIPSIS, 0);
        if (in.isSwitch) {
            r.check = make(L"BUTTON", L"on", BS_AUTOCHECKBOX | WS_TABSTOP, id + rpCheck); // "on" / "off"
        } else {
            r.slider = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, id + rpSlider);
            const Scale s = scaleOf(p);
            SendMessageW(r.slider, TBM_SETRANGEMIN, FALSE, 0);
            SendMessageW(r.slider, TBM_SETRANGEMAX, FALSE, s.positions);
            SendMessageW(r.slider, TBM_SETLINESIZE, 0, 1);
            SendMessageW(r.slider, TBM_SETPAGESIZE, 0, std::max(1, s.positions / 10));
            r.value = make(L"EDIT", L"", ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP, id + rpValue, WS_EX_CLIENTEDGE);
            SetWindowSubclass(r.value, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
            r.unit = make(L"STATIC", fromUtf8(in.unit).c_str(), SS_LEFT | SS_CENTERIMAGE, 0);
        }
        r.reset = make(L"BUTTON", L"\x21BA", BS_OWNERDRAW | WS_TABSTOP, id + rpReset);
        theme::makeButton(r.reset);
        r.help = make(L"STATIC", fromUtf8(in.help).c_str(), SS_LEFT | SS_NOTIFY | SS_NOPREFIX, 0); // wraps
        std::wstring tip = fromUtf8(name + ": " + in.help + ".\n\nRange " + rangeText(p) +
                                    (in.unit[0] ? std::string(" ") + in.unit : "") + "." +
                                    (in.isSwitch ? "" : " Drag the slider, use the arrow keys, or type a value and "
                                                        "press Enter."));
        for (int k = 0; k < kStageCount; ++k)
            if (kStageParam[k] == p) tip = std::wstring(kStageTip[k]) + kStageTipEnd;
        for (HWND h : {r.name, r.slider, r.check, r.value, r.help}) tips_.add(h, tip.c_str());
        rows_.push_back(r);
    }
}

void EditWindow::setFonts() {
    for (HFONT f : {font_, bold_, symbol_})
        if (f) DeleteObject(f);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    bold_ = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 3 / 2;
    wcscpy_s(lf.lfFaceName, L"Segoe UI Symbol");
    symbol_ = CreateFontIndirectW(&lf);
    auto set = [&](HWND h, HFONT f) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    for (HWND h : {presetLabel_, preset_, saveAs_, rename_, delete_, resetPreset_, note_, about_, tabs_, sound_})
        set(h, font_);
    set(soundTitle_, bold_);
    for (const Row& r : rows_) {
        for (HWND h : {r.name, r.check, r.value, r.unit, r.help}) set(h, font_);
        set(r.reset, symbol_);
    }
    updateRows(); // the bold font of changed values
}

void EditWindow::layout() {
    if (!tabs_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int m = px(12), h = px(28), g = px(6), bw = px(110);
    int x = m;
    MoveWindow(presetLabel_, x, m, px(48), h, TRUE);
    x += px(48) + g;
    const int comboW = std::max(px(160), static_cast<int>(rc.right - x - 4 * (bw + g) - m));
    MoveWindow(preset_, x, m, comboW, px(300), TRUE);
    x += comboW + g;
    for (HWND b : {saveAs_, rename_, delete_, resetPreset_}) {
        MoveWindow(b, x, m, bw, h, TRUE);
        x += bw + g;
    }
    // under the choice: what the chosen preset or colour does (it wraps), then the note
    const int W = rc.right - 2 * m;
    aboutH_ = textHeight(about_, aboutText_, W);
    int y = m + h + g;
    MoveWindow(about_, m, y, W, aboutH_, TRUE);
    y += aboutH_ + px(4);
    MoveWindow(note_, m, y, W, h, TRUE);
    y += h + g;
    // the tab strip; the rows sit below it on the window's own background
    RECT strip{0, 0, rc.right - 2 * m, px(200)};
    TabCtrl_AdjustRect(tabs_, FALSE, &strip);
    const int tabH = std::max(px(26), static_cast<int>(strip.top) + px(2));
    MoveWindow(tabs_, m, y, rc.right - 2 * m, tabH, TRUE);
    y += tabH + px(10);

    // under the rows: what the stage does to the sound (its height decides how much room the rows get)
    const std::wstring sound = stageSound(tab_);
    RECT tr{0, 0, W, 0};
    if (HDC dc = GetDC(sound_)) {
        HGDIOBJ old = SelectObject(dc, font_);
        DrawTextW(dc, sound.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(sound_, dc);
    }
    const int soundH = static_cast<int>(tr.bottom) + px(4), soundTitleH = px(22);
    const int below = px(10) + px(2) + px(8) + soundTitleH + soundH;

    // rows: name, slider, value, unit, reset, what it does (wrapped: a long text makes its row taller)
    const int nameW = px(170), valueW = px(70), unitW = px(44), resetW = px(30);
    const int helpW = std::max(0, W * 34 / 100);
    const int sliderW = std::max(px(120), W - nameW - valueW - unitW - resetW - helpW - 5 * g);
    const int helpX = m + nameW + sliderW + valueW + unitW + resetW + 5 * g, helpTextW = std::max(px(40), m + W - helpX);
    std::vector<int> textH(rows_.size(), 0);
    size_t n = 0;
    int extra = 0; // what wrapped texts need above the smallest row height
    if (HDC dc = GetDC(sound_)) {
        HGDIOBJ old = SelectObject(dc, font_);
        for (size_t r = 0; r < rows_.size(); ++r) {
            if (!shown(r)) continue;
            ++n;
            RECT t{0, 0, helpTextW, 0};
            DrawTextW(dc, fromUtf8(paramInfo(rows_[r].param).help).c_str(), -1, &t,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            textH[r] = static_cast<int>(t.bottom);
            extra += std::max(0, textH[r] + px(8) - px(24));
        }
        SelectObject(dc, old);
        ReleaseDC(sound_, dc);
    }
    const int avail = rc.bottom - y - m - below - extra;
    const int rowH = n ? std::min(px(36), std::max(px(24), avail / static_cast<int>(n))) : px(36);
    const int ch = std::min(rowH - px(4), px(26));
    for (size_t r = 0; r < rows_.size(); ++r) {
        const Row& row = rows_[r];
        if (!shown(r)) continue;
        const int rh = std::max(rowH, textH[r] + px(8));
        const int cy = y + (rh - ch) / 2;
        int cx = m;
        MoveWindow(row.name, cx, cy, nameW, ch, FALSE);
        cx += nameW + g;
        if (row.slider) MoveWindow(row.slider, cx, cy, sliderW, ch, FALSE);
        if (row.check) MoveWindow(row.check, cx + px(6), cy, sliderW, ch, FALSE);
        cx += sliderW + g;
        if (row.value) MoveWindow(row.value, cx, cy + (ch - px(24)) / 2, valueW, px(24), FALSE);
        cx += valueW + g;
        if (row.unit) MoveWindow(row.unit, cx, cy, unitW, ch, FALSE);
        cx += unitW + g;
        MoveWindow(row.reset, cx, cy + (ch - px(24)) / 2, resetW, px(24), FALSE);
        cx += resetW + g;
        MoveWindow(row.help, cx, y + (rh - textH[r]) / 2, std::max(0, m + W - cx), textH[r], FALSE);
        y += rh;
    }
    y += px(10);
    soundLine_ = {m, y, m + W, y + px(2)}; // drawn in WM_PAINT
    y += px(2) + px(8);
    MoveWindow(soundTitle_, m, y, W, soundTitleH, FALSE);
    y += soundTitleH;
    MoveWindow(sound_, m, y, W, soundH, FALSE);
    SetWindowTextW(sound_, sound.c_str());
    const std::wstring title = fromUtf8(tab_) + L": effect on the sound";
    SetWindowTextW(soundTitle_, title.c_str());
    // the rows were moved without painting (one repaint for all); WS_CLIPCHILDREN keeps the parent's invalidation
    // off the children, so they are invalidated too - else a resize leaves pieces of old rows
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void EditWindow::fillPresets() {
    filling_ = true;
    SendMessageW(preset_, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < settings_.list(kind_).size(); ++i) {
        const Preset& p = settings_.list(kind_)[i];
        SendMessageW(preset_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fromUtf8(p.name).c_str()));
        if (p.name == current_) SendMessageW(preset_, CB_SETCURSEL, i, 0);
    }
    filling_ = false;
}

void EditWindow::fillTabs() {
    const Preset* p = settings_.find(current_, kind_);
    if (!p || !tabs_) return;
    if (tabStages_.empty()) { // the stages of the kind, in the order of the parameter table
        for (int i = 0; i < kParamCount; ++i)
            if (kindOf(i) == kind_ &&
                std::find(tabStages_.begin(), tabStages_.end(), paramInfo(i).stage) == tabStages_.end())
                tabStages_.push_back(paramInfo(i).stage);
        for (size_t t = 0; t < tabStages_.size(); ++t) {
            TCITEMW it{};
            it.mask = TCIF_TEXT;
            const std::wstring label = fromUtf8(tabStages_[t]);
            it.pszText = const_cast<wchar_t*>(label.c_str());
            TabCtrl_InsertItem(tabs_, static_cast<int>(t), &it);
        }
        if (tab_.empty()) tab_ = tabStages_.front();
    }
    for (size_t t = 0; t < tabStages_.size(); ++t) { // a stage that is off says so
        const int sw = stageSwitch(tabStages_[t]);
        const std::wstring label = fromUtf8(tabStages_[t]) + (sw >= 0 && p->values[static_cast<size_t>(sw)] < 0.5 ? L" (off)" : L"");
        TCITEMW it{};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(label.c_str());
        TabCtrl_SetItem(tabs_, static_cast<int>(t), &it);
        if (tabStages_[t] == tab_) TabCtrl_SetCurSel(tabs_, static_cast<int>(t));
    }
}

bool EditWindow::shown(size_t r) const {
    const Preset* p = const_cast<Settings&>(settings_).find(current_, kind_);
    const int i = rows_[r].param;
    return p && paramInfo(i).stage == tab_ && visible(i, p->values, kind_);
}

void EditWindow::showRows() {
    fillTabs();
    layout();
    for (size_t r = 0; r < rows_.size(); ++r) {
        const int show = shown(r) ? SW_SHOW : SW_HIDE;
        const Row& row = rows_[r];
        for (HWND h : {row.name, row.slider, row.check, row.value, row.unit, row.reset, row.help})
            if (h) ShowWindow(h, show);
    }
}

void EditWindow::selectTab(const std::string& stage) {
    if (stage == tab_) return;
    tab_ = stage;
    showRows();
}

void EditWindow::select(const std::string& preset) {
    current_ = settings_.find(preset, kind_) ? preset : used(deck_);
    fillPresets();
    showRows();
    updateRows();
    const std::wstring title = (colour() ? L"Colour: " : L"Preset: ") + fromUtf8(current_);
    SetWindowTextW(hwnd_, title.c_str());
    updateButtons();
}

void EditWindow::updateRows() {
    for (size_t r = 0; r < rows_.size(); ++r) updateRow(r);
}

// Slider, value, bold when it differs from its reset value, the reset button only then.
void EditWindow::updateRow(size_t r, bool moveSlider) {
    const Preset* p = settings_.find(current_, kind_);
    if (!p || r >= rows_.size()) return;
    const Row& row = rows_[r];
    const int i = row.param;
    const double v = p->values[static_cast<size_t>(i)];
    const double ref = referenceValues(current_, p->values, kind_)[static_cast<size_t>(i)];
    const bool changed = std::fabs(v - ref) > 1e-9;
    if (row.slider && moveSlider) SendMessageW(row.slider, TBM_SETPOS, TRUE, positionOf(i, v));
    if (row.check) {
        SendMessageW(row.check, BM_SETCHECK, v >= 0.5 ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(row.check, v >= 0.5 ? L"on" : L"off");
    }
    if (row.value) {
        SetWindowTextW(row.value, fromUtf8(valueText(i, v)).c_str());
        SendMessageW(row.value, WM_SETFONT, reinterpret_cast<WPARAM>(changed ? bold_ : font_), TRUE);
    }
    if (row.check) SendMessageW(row.check, WM_SETFONT, reinterpret_cast<WPARAM>(changed ? bold_ : font_), TRUE);
    EnableWindow(row.reset, changed);
    const ParamInfo& in = paramInfo(i);
    const std::wstring tip = L"Back to its reset value: " + fromUtf8(valueText(i, ref)) +
                             (in.unit[0] && !in.isSwitch ? L" " + fromUtf8(in.unit) : L"") +
                             (changed ? L"" : L" (it has that value now)");
    tips_.add(row.reset, tip.c_str());
}

int EditWindow::textHeight(HWND h, const std::wstring& text, int width) const {
    RECT r{0, 0, width, 0};
    if (HDC dc = GetDC(h)) {
        HGDIOBJ old = SelectObject(dc, font_);
        DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(h, dc);
    }
    return static_cast<int>(r.bottom);
}

// The description under the choice; a new line count lays the window out again.
void EditWindow::updateAbout() {
    const Preset* p = settings_.find(current_, kind_);
    aboutText_ = p ? describe(*p, kind_) : L"";
    SetWindowTextW(about_, aboutText_.c_str());
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (textHeight(about_, aboutText_, rc.right - px(24)) != aboutH_) layout();
}

void EditWindow::updateButtons() {
    updateAbout();
    const Preset* p = settings_.find(current_, kind_);
    const bool own = p && !p->shipped;
    EnableWindow(rename_, own);
    EnableWindow(delete_, own);
    SetWindowTextW(resetPreset_, own ? L"Reset to defaults" : colour() ? L"Reset colour" : L"Reset preset");
    std::wstring users;
    for (int d = 0; d < 2; ++d)
        if (d < settings_.decks && used(d) == current_)
            users += users.empty() ? (d == 0 ? L"deck A" : L"deck B") : L" and deck B";
    const std::wstring what = colour() ? L"colour" : L"preset";
    std::wstring note = own ? L"Own " + what + L"." : L"Shipped " + what + L": values can be changed and reset; it cannot "
                                                                         L"be renamed or deleted.";
    note += users.empty() ? L" Not used by a deck right now." : L" Changes are heard at once on " + users + L".";
    note += L"  Bold = differs from its reset value; \x21BA sets it back.";
    SetWindowTextW(note_, note.c_str());
}

bool EditWindow::setValue(int param, double value, bool save) {
    Preset* p = settings_.find(current_, kind_);
    if (!p) return false;
    PresetValues& v = p->values;
    const int oldBands = static_cast<int>(v[pDensBands]);
    v[static_cast<size_t>(param)] = value;
    if (param == pDensBands) {
        normaliseValues(v);
        if (static_cast<int>(v[pDensBands]) != oldBands) loadDefaultSplits(v, static_cast<int>(v[pDensBands]));
    }
    if (param >= pDensSplit1 && param <= pDensSplit6) enforceSplits(v, param - pDensSplit1);
    normaliseValues(v);
    onChange_(false, save);
    updateAbout(); // "some values are changed"
    if (param == pDensBands) showRows(); // split and band-gain rows appear or go
    if (isStageSwitch(param)) fillTabs();
    return true;
}

int EditWindow::rowOf(HWND control) const {
    const int id = GetDlgCtrlID(control) - idRow0;
    return id >= 0 && id / kRowIds < static_cast<int>(rows_.size()) ? id / kRowIds : -1;
}

void EditWindow::slider(HWND bar, int code) {
    const int r = rowOf(bar);
    const Preset* p = settings_.find(current_, kind_);
    if (r < 0 || !p) return;
    const int i = rows_[static_cast<size_t>(r)].param;
    const double v = valueAt(i, static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0)));
    const bool save = code != TB_THUMBTRACK;
    if (v != p->values[static_cast<size_t>(i)]) {
        setValue(i, v, save);
    } else if (code == TB_THUMBPOSITION) {
        onChange_(false, true); // a drag ended on a value that was already heard: save it now
        return;
    } else {
        return;
    }
    // the slider stays where it was put unless a rule moved the value (splits keep their order, ranges)
    const double stored = settings_.find(current_, kind_)->values[static_cast<size_t>(i)];
    if (i >= pDensSplit1 && i <= pDensSplit6) { // a split may push its neighbours
        for (size_t k = 0; k < rows_.size(); ++k) updateRow(k, static_cast<int>(k) != r || stored != v);
    } else {
        updateRow(static_cast<size_t>(r), stored != v);
    }
}

void EditWindow::commitValue(size_t r) {
    const Preset* p = settings_.find(current_, kind_);
    if (!p || r >= rows_.size() || !rows_[r].value) return;
    const Row& row = rows_[r];
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(row.value)), L'\0');
    GetWindowTextW(row.value, text.data(), static_cast<int>(text.size()) + 1);
    double v;
    if (!parseValue(row.param, text, v)) {
        const std::wstring msg = L"\"" + text + L"\" is not a value for " + fromUtf8(paramInfo(row.param).name) +
                                 L" (" + fromUtf8(rangeText(row.param)) + L").";
        updateRow(r);
        placement::messageBox(hwnd_, msg.c_str(), colour() ? L"Colour" : L"Preset", MB_OK | MB_ICONWARNING);
        return;
    }
    if (v != p->values[static_cast<size_t>(row.param)]) setValue(row.param, v); // clamped by normaliseValues
    updateRows();
}

LRESULT CALLBACK EditWindow::editProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    EditWindow* self = reinterpret_cast<EditWindow*>(data);
    switch (msg) {
    case WM_GETDLGCODE:
        if (lp && (reinterpret_cast<MSG*>(lp)->wParam == VK_RETURN || reinterpret_cast<MSG*>(lp)->wParam == VK_ESCAPE))
            return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wp, lp);
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE) {
            const int r = self->rowOf(hwnd);
            if (r >= 0) {
                if (wp == VK_RETURN) self->commitValue(static_cast<size_t>(r));
                else self->updateRow(static_cast<size_t>(r)); // back to the value it has
            }
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == 27) return 0; // no beep
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, editProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void EditWindow::command(int id, int code) {
    if (id >= idRow0) {
        const int r = (id - idRow0) / kRowIds, part = (id - idRow0) % kRowIds;
        const Preset* p = settings_.find(current_, kind_);
        if (!p || r >= static_cast<int>(rows_.size())) return;
        const Row& row = rows_[static_cast<size_t>(r)];
        if (part == rpValue && code == EN_KILLFOCUS) {
            commitValue(static_cast<size_t>(r));
        } else if (part == rpReset && code == BN_CLICKED) {
            setValue(row.param, referenceValues(current_, p->values, kind_)[static_cast<size_t>(row.param)]);
            updateRows();
        } else if (part == rpCheck && code == BN_CLICKED) {
            setValue(row.param, SendMessageW(row.check, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1.0 : 0.0);
            updateRows();
        }
        return;
    }
    if (id == idPreset && code == CBN_SELCHANGE && !filling_) {
        const LRESULT sel = SendMessageW(preset_, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && static_cast<size_t>(sel) < settings_.list(kind_).size()) select(settings_.list(kind_)[static_cast<size_t>(sel)].name);
        return;
    }
    if (code != BN_CLICKED) return;
    Preset* p = settings_.find(current_, kind_);
    if (!p) return;
    std::string err;
    if (id == idSaveAs) {
        std::string name = current_ + " (copy)";
        if (!askName(hwnd_, colour() ? L"Save colour as" : L"Save preset as",
                     colour() ? L"Name of the new colour:" : L"Name of the new preset:", name))
            return;
        if (const Preset* existing = settings_.find(name, kind_)) {
            if (!existing->shipped) {
                const std::wstring q = L"Replace the preset \"" + fromUtf8(name) + L"\"?";
                if (placement::messageBox(hwnd_, q.c_str(), L"Save preset as", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            }
        }
        if (!settings_.saveAs(name, p->values, err, kind_)) {
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Save preset as", MB_OK | MB_ICONWARNING);
            return;
        }
        if (used(deck_) == current_) used(deck_) = name;
        if (settings_.linkPresets && settings_.decks == 2) used(1 - deck_) = used(deck_); // both decks
        current_ = name;
        onChange_(true, true);
        select(current_);
    } else if (id == idRename) {
        std::string name = current_;
        if (!askName(hwnd_, L"Rename preset", L"New name:", name) || name == current_) return;
        if (!settings_.rename(current_, name, err, kind_)) {
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Rename preset", MB_OK | MB_ICONWARNING);
            return;
        }
        current_ = name;
        onChange_(true, true);
        select(current_);
    } else if (id == idDelete) {
        const std::wstring q = (colour() ? L"Delete the colour \"" : L"Delete the preset \"") + fromUtf8(current_) +
                               (colour() ? L"\"? A deck that uses it changes to Neutral." :
                                           L"\"? A deck that uses it changes to Medium.");
        if (placement::messageBox(hwnd_, q.c_str(), L"Delete preset", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) !=
            IDYES)
            return;
        if (!settings_.remove(current_, err, kind_)) {
            placement::messageBox(hwnd_, fromUtf8(err).c_str(), L"Delete preset", MB_OK | MB_ICONWARNING);
            return;
        }
        onChange_(true, true);
        select(used(deck_));
    } else if (id == idResetPreset) {
        const std::wstring q = L"Set every value of \"" + fromUtf8(current_) + L"\" back to " +
                               (p->shipped ? L"its shipped values?" : L"the generic defaults?");
        if (placement::messageBox(hwnd_, q.c_str(), colour() ? L"Reset colour" : L"Reset preset",
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        settings_.resetPreset(current_, kind_);
        onChange_(false, true);
        showRows();
        updateRows();
    }
}

} // namespace ap
