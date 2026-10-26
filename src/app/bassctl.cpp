#include "bassctl.h"

#include <commctrl.h>

#include <cmath>
#include <cwchar>

namespace ap::bass {

namespace {
const int kMin[kSliders] = {-24, 0, 0, 8}, kMax[kSliders] = {24, 20, 20, 50}; // 0.5 dB / 5 % / 5 Hz
} // namespace

const wchar_t* const kName[kSliders] = {L"Bass level", L"Bass enhancer", L"Bass width", L"Bass frequency"};

const wchar_t* const kTip[kSliders] = {
    L"More or less bass, -12 .. +12 dB: the colour's bass shelf below the bass frequency. The leveller keeps the "
    L"loudness on target, so this changes the balance, not the volume; the limiter keeps the ceiling. 0 dB = off.\n"
    L"Belongs to the colour.",
    L"Strength of the bass enhancer, 0 .. 100 %: overtones of the bass below its frequency, so the bass is heard on "
    L"small speakers too. 0 % switches the enhancer off, more switches it on.\nBelongs to the loudness preset.",
    L"Stereo width of the bass below the bass frequency, 0 .. 100 %: 0 % = bass in mono (tighter in a club, safe "
    L"for mono systems), 100 % = as recorded. Moving it switches the width stage on.\nBelongs to the loudness preset.",
    L"Where 'bass' ends, 40 .. 250 Hz: the colour's bass corner, and in the loudness preset the enhancer's frequency (60 .. "
    L"160 Hz) and the width stage's bass split, all at once (each can be set apart in the colour and loudness windows).",
};

void setup(HWND bar, int k) {
    SendMessageW(bar, TBM_SETRANGEMIN, FALSE, kMin[k]);
    SendMessageW(bar, TBM_SETRANGEMAX, FALSE, kMax[k]);
    SendMessageW(bar, TBM_SETTICFREQ, k == kLevel ? 4 : 2, 0);
    SendMessageW(bar, TBM_SETLINESIZE, 0, 1);
    SendMessageW(bar, TBM_SETPAGESIZE, 0, k == kLevel ? 4 : 2);
}

int position(const PresetValues& v, int k) {
    switch (k) {
    case kLevel:
        return static_cast<int>(std::lround(v[pToneBass] * 2.0));
    case kEnhancer:
        return v[pBassOn] >= 0.5 ? static_cast<int>(std::lround(v[pBassAmount] / 5.0)) : 0;
    case kWidth:
        return static_cast<int>(std::lround(v[pBassWidth] / 5.0));
    default:
        return static_cast<int>(std::lround(v[pToneBassFreq] / 5.0));
    }
}

std::wstring text(const PresetValues& v, int k) {
    wchar_t t[40];
    switch (k) {
    case kLevel:
        if (v[pToneBass] == 0.0) swprintf(t, 40, L"0 dB (off)");
        else swprintf(t, 40, L"%+.1f dB", v[pToneBass]);
        break;
    case kEnhancer:
        if (v[pBassOn] >= 0.5) swprintf(t, 40, L"%.0f %%", v[pBassAmount]);
        else swprintf(t, 40, L"off");
        break;
    case kWidth:
        if (v[pWidthOn] >= 0.5) swprintf(t, 40, v[pBassWidth] == 0.0 ? L"mono" : L"%.0f %%", v[pBassWidth]);
        else swprintf(t, 40, L"%.0f %% (off)", v[pBassWidth]);
        break;
    default:
        swprintf(t, 40, L"%.0f Hz", v[pToneBassFreq]);
        break;
    }
    return t;
}

bool apply(Settings& s, int deck, int k, int pos) {
    // bass level in the colour, enhancer and width in the loudness preset, the frequency in both: every deck using
    // them changes
    Preset* p = s.find(s.deck[deck].preset);
    Preset* c = s.find(s.deck[deck].colour, PresetKind::Colour);
    if (!p || !c) return false;
    PresetValues& v = p->values;
    PresetValues& cv = c->values;
    switch (k) {
    case kLevel:
        cv[pToneBass] = pos * 0.5;
        break;
    case kEnhancer:
        v[pBassAmount] = pos * 5.0;
        v[pBassOn] = pos > 0 ? 1.0 : 0.0;
        break;
    case kWidth:
        v[pBassWidth] = pos * 5.0;
        v[pWidthOn] = 1.0;
        break;
    default: // the bass frequency of all three
        cv[pToneBassFreq] = pos * 5.0;
        v[pBassFreq] = pos * 5.0; // clamped to 60 .. 160 Hz
        v[pWidthSplit] = pos * 5.0;
        break;
    }
    normaliseValues(v);
    normaliseValues(cv);
    return true;
}

} // namespace ap::bass
