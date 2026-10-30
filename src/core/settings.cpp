// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <io.h> // _commit (the C runtime, not Windows' headers)
#else
#include <unistd.h>
#endif

namespace ap {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string number(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// Shortest form with at most three decimals ("5", "0.5", "-1.25").
std::string compact(double v) {
    std::string s = number(v, 3);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

// A number as written by hand too: "1,5" (a Dutch decimal comma) is 1.5; "nan" and "inf" are refused
// (a "nan" look-ahead became 0 and switched the true-peak limiting off). Text after the number ("-14 LUFS") is ignored.
bool parseDouble(const std::string& s, double& out) {
    std::string t = s;
    if (t.find('.') == std::string::npos) std::replace(t.begin(), t.end(), ',', '.');
    try {
        size_t used = 0;
        const double d = std::stod(t, &used);
        if (used == 0 || !std::isfinite(d)) return false;
        out = d;
        return true;
    } catch (...) {
        return false;
    }
}

int toInt(const std::string& s, int fallback) {
    double d;
    if (!parseDouble(s, d) || std::fabs(d) > 1e9) return fallback;
    return static_cast<int>(std::lround(d));
}

// The text of a settings file: UTF-8 (with or without its BOM, which fromText skips), or UTF-16 with its BOM - what an
// editor writes when the file is saved as "Unicode". Taken as UTF-8 that read as an empty file, and the next save
// wrote the file without the user's own presets.
std::string decodeText(const std::string& raw) {
    if (raw.size() < 2) return raw;
    const unsigned char b0 = static_cast<unsigned char>(raw[0]), b1 = static_cast<unsigned char>(raw[1]);
    const bool le = b0 == 0xFF && b1 == 0xFE, be = b0 == 0xFE && b1 == 0xFF;
    if (!le && !be) return raw;
    std::string out;
    auto unit = [&](size_t i) {
        const unsigned a = static_cast<unsigned char>(raw[i]), b = static_cast<unsigned char>(raw[i + 1]);
        return le ? (a | (b << 8)) : ((a << 8) | b);
    };
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        unsigned c = unit(i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) { // a surrogate pair
            const unsigned lo = unit(i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c >= 0xD800 && c < 0xE000) c = 0xFFFD; // a lone surrogate
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

const char* const kPresetPrefix = "Preset ";
const char* const kColourPrefix = "Colour ";

bool validName(const std::string& name, std::string& error) {
    if (trim(name).empty()) {
        error = "A preset needs a name.";
        return false;
    }
    if (name != trim(name) || name.find_first_of("[]=;\r\n") != std::string::npos) {
        error = "A preset name may not contain [ ] = ; or start / end with a space.";
        return false;
    }
    return true;
}

} // namespace

Settings::Settings() {
    for (int i = 0; i < kShippedPresetCount; ++i) {
        Preset p;
        p.name = shippedPresetName(i);
        p.values = shippedPresetValues(i);
        p.shipped = true;
        presets.push_back(p);
    }
    for (int i = 0; i < kShippedColourCount; ++i) {
        Preset p;
        p.name = shippedColourName(i);
        p.values = shippedColourValues(i);
        p.shipped = true;
        colours.push_back(p);
    }
    sortPresets();
}

// The list from the quietest to the loudest output (target + output gain; DAB+ -23 .. Very Loud -10), own presets
// among the shipped ones; equal loudness keeps its order. Not re-sorted while a value is edited: a list that moves
// under the cursor would change what a list index means.
void Settings::sortPresets() {
    std::stable_sort(presets.begin(), presets.end(), [](const Preset& a, const Preset& b) {
        return a.values[pTarget] + a.values[pOutputGain] < b.values[pTarget] + b.values[pOutputGain];
    });
}

bool Settings::audioConfigured() const {
    if (api == "ASIO") return !driver.empty();
    for (int d = 0; d < 2; ++d)
        if (!inDevice[d].empty() || !outDevice[d].empty()) return true;
    return false;
}

Preset* Settings::find(const std::string& name, PresetKind k) {
    for (Preset& p : list(k))
        if (p.name == name) return &p;
    return nullptr;
}

const Preset* Settings::find(const std::string& name, PresetKind k) const {
    for (const Preset& p : list(k))
        if (p.name == name) return &p;
    return nullptr;
}

PresetValues Settings::valuesOf(const std::string& name) const {
    const Preset* p = find(name);
    return p ? p->values : shippedPresetValues(shippedPresetIndex("Medium"));
}

PresetValues Settings::colourValuesOf(const std::string& name) const {
    const Preset* p = find(name, PresetKind::Colour);
    return p ? p->values : shippedColourValues(shippedColourIndex("Neutral"));
}

PresetValues Settings::shippedValues(const std::string& name, PresetKind k) {
    if (k == PresetKind::Colour) {
        const int i = shippedColourIndex(name);
        return i >= 0 ? shippedColourValues(i) : defaultValues();
    }
    const int i = shippedPresetIndex(name);
    return i >= 0 ? shippedPresetValues(i) : defaultValues();
}

bool Settings::saveAs(const std::string& name, const PresetValues& values, std::string& error, PresetKind k) {
    if (!validName(name, error)) return false;
    if (Preset* p = find(name, k)) {
        if (p->shipped) {
            error = "\"" + name + "\" is a shipped preset; choose another name.";
            return false;
        }
        p->values = values;
        normaliseValues(p->values); // as for a new preset
        return true;
    }
    Preset p;
    p.name = name;
    p.values = values;
    normaliseValues(p.values);
    list(k).push_back(p);
    if (k == PresetKind::Dynamics) sortPresets(); // in its place by loudness
    return true;
}

bool Settings::rename(const std::string& from, const std::string& to, std::string& error, PresetKind k) {
    Preset* p = find(from, k);
    if (!p) {
        error = "There is no preset \"" + from + "\".";
        return false;
    }
    if (p->shipped) {
        error = "Shipped presets cannot be renamed.";
        return false;
    }
    if (!validName(to, error)) return false;
    if (from == to) return true;
    if (find(to, k)) {
        error = "A preset \"" + to + "\" already exists.";
        return false;
    }
    p->name = to;
    for (DeckSettings& d : deck) {
        std::string& used = k == PresetKind::Colour ? d.colour : d.preset;
        if (used == from) used = to;
    }
    return true;
}

bool Settings::remove(const std::string& name, std::string& error, PresetKind k) {
    std::vector<Preset>& l = list(k);
    for (size_t i = 0; i < l.size(); ++i) {
        if (l[i].name != name) continue;
        if (l[i].shipped) {
            error = "Shipped presets cannot be deleted (they can be reset).";
            return false;
        }
        l.erase(l.begin() + static_cast<long long>(i));
        for (DeckSettings& d : deck) {
            std::string& used = k == PresetKind::Colour ? d.colour : d.preset;
            if (used == name) used = k == PresetKind::Colour ? "Neutral" : "Medium";
        }
        return true;
    }
    error = "There is no preset \"" + name + "\".";
    return false;
}

double Settings::resetValueOf(const std::string& presetName, int param) {
    return shippedValues(presetName, kindOf(param))[static_cast<size_t>(param)];
}

void Settings::resetPreset(const std::string& name, PresetKind k) {
    Preset* p = find(name, k);
    if (!p) return;
    p->values = shippedValues(name, k);
}

void Settings::resetValue(const std::string& name, int param) {
    if (param < 0 || param >= kParamCount) return;
    Preset* p = find(name, kindOf(param));
    if (!p) return;
    p->values[static_cast<size_t>(param)] = resetValueOf(name, param);
    if (param == pDensBands) loadDefaultSplits(p->values, static_cast<int>(p->values[pDensBands]));
    normaliseValues(p->values);
}

std::string Settings::toText() const {
    std::ostringstream o;
    o << "; DJAP settings (UTF-8). Saved by the program after every change; edit by hand only while the\n"
         "; program is closed. Unknown keys are ignored and dropped on the next save.\n\n";
    o << "[Audio]\n";
    o << "; Processing channels: 2 = two decks (DJ: deck A and deck B), 1 = one channel (deck A only)\nDecks="
      << decks << "\n";
    o << "; Audio API: ASIO, WASAPI, DirectSound or MME\nApi=" << api << "\n";
    o << "; ASIO driver name (as listed in the Setup window)\nDriver=" << driver << "\n";
    o << "; WASAPI / DirectSound / MME: input and output device of each deck (empty = the deck does not run)\n";
    o << "InputDeviceA=" << inDevice[0] << "\nOutputDeviceA=" << outDevice[0] << "\nInputDeviceB=" << inDevice[1]
      << "\nOutputDeviceB=" << outDevice[1] << "\n";
    o << "; WASAPI exclusive mode (1 = lower latency, the devices are taken from other programs)\nExclusive="
      << (exclusive ? 1 : 0) << "\n";
    o << "; WASAPI / DirectSound / MME: 1 = each deck's input is read with drift correction (input and output device "
         "on\n; different clocks never drift apart; adds a buffer of about 8-23 ms) unless WASAPI finds them on one "
         "device,\n; 2 = always, 0 = never (one duplex stream)\n"
         "DriftCorrection="
      << driftCorrection << "\n";
    o << "; Sample rate in Hz: 44100, 48000, 88200 or 96000\nSampleRate=" << sampleRate << "\n";
    o << "; First channel (1-based) of the input / output pair of each deck\n";
    o << "InputA=" << inputA << "\nInputB=" << inputB << "\nOutputA=" << outputA << "\nOutputB=" << outputB << "\n";
    o << "; ASIO: where the decks' inputs come from - ASIO (the input pairs above), or WASAPI, DirectSound or MME (a\n"
         "; device of that API per deck, drift-corrected; adds a buffer of about 10-20 ms)\nAsioInputs="
      << asioInputs << "\n";
    o << "; ASIO with WASAPI inputs: their period in ms - 0 = Windows' 10 ms default, or 8, 6, 5 or 4 (Windows'\n"
         "; low-latency mode, when the device has it)\nInputPeriodMs="
      << inputPeriodMs << "\n";
    o << "; ... the input device of each deck then (empty = the deck's ASIO input pair)\n";
    o << "AsioInputDeviceA=" << asioInputDevice[0] << "\nAsioInputDeviceB=" << asioInputDevice[1] << "\n";
    o << "; Limiter look-ahead in ms (0..5), the same for both decks; the only delay the processing adds\n";
    o << "LookAheadMs=" << compact(lookAheadMs) << "\n";
    o << "; Oversampling of the soft clipper and the exciter (2, 4 or 8), both decks: more keeps their overtones from\n"
         "; folding back into the audio band, at more CPU\nOversampling=" << oversampling << "\n";
    o << "; 1 = mono: every deck's left and right mixed, (L + R) / 2, on both outputs (a mono sound system)\nMono="
      << (mono ? 1 : 0) << "\n";
    o << "; Driver buffer size in frames (0 = the driver's preferred size)\nBufferSize=" << bufferSize << "\n";
    o << "; Start the audio when the program starts (1 = it was started last and not stopped since)\nStartAudio="
      << (startAudio ? 1 : 0) << "\n";
    o << "; The outputs follow the volume and mute of Windows' default playback device (the volume keys), with one\n"
         "; channel and with two decks (1 = on)\nFollowWindowsVolumeOneChannel="
      << (followVolume[0] ? 1 : 0) << "\nFollowWindowsVolumeTwoDecks=" << (followVolume[1] ? 1 : 0) << "\n\n";
    o << "[Decks]\n; Loudness preset and processing switch (1 = on, 0 = bypass) of each deck\n";
    o << "PresetA=" << deck[0].preset << "\nProcessingA=" << (deck[0].processing ? 1 : 0) << "\n";
    o << "PresetB=" << deck[1].preset << "\nProcessingB=" << (deck[1].processing ? 1 : 0) << "\n";
    o << "; EQ of each deck (tone, auto tone, sub bass, contour; the keys keep their old name Colour)\nColourA=" << deck[0].colour
      << "\nColourB=" << deck[1].colour << "\n";
    o << "; 1 = both decks use the same loudness preset and EQ (deck B follows deck A)\nLinkPresets=" << (linkPresets ? 1 : 0)
      << "\n\n";
    o << "[Window]\n; Where each window was last: x, y, width, height (screen pixels), 1 = maximised; a window without a\n"
         "; line opens centred on the screen\n";
    for (const auto& [name, p] : windows)
        o << name << "=" << p.x << "," << p.y << "," << p.w << "," << p.h << "," << (p.maximised ? 1 : 0) << "\n";
    o << "SnapToBands=" << (snapToBands ? 1 : 0) << "\nTooltips=" << (showTooltips ? 1 : 0) << "\n";
    o << "; 1 = the hint that the program keeps running in the notification area was shown\nTrayHintShown="
      << (trayHintShown ? 1 : 0) << "\n\n";
    o << "[Updates]\n; 1 = ask GitHub once a day for a newer version (installed only after a click)\nCheckUpdates="
      << (checkUpdates ? 1 : 0) << "\n";
    if (!updateUrl.empty()) o << "; where to ask instead of the public repository (tests)\nUrl=" << updateUrl << "\n";
    if (!pluginSlots.empty() || pluginLink) {
        o << "\n[Plugin]\n; 1 = the plug-ins keep the same settings (the first one open leads)\nLink=" << (pluginLink ? 1 : 0)
          << "\n";
        o << "; What each plug-in instance last chose - 1 = the first one open (deck 1 in a DJ program), 2 = the\n"
             "; second, ...: loudness preset, EQ (Colour), look-ahead (ms), oversampling, 1 = mono; taken at its next start\n";
        for (const auto& [n, c] : pluginSlots)
            o << "Loudness" << n << "=" << c.preset << "\nColour" << n << "=" << c.colour << "\nLookAhead" << n << "="
              << compact(c.lookAheadMs) << "\nOversampling" << n << "=" << c.oversampling << "\nMono" << n << "="
              << (c.mono ? 1 : 0) << "\n";
    }

    for (const Preset& p : presets) {
        const int shipped = shippedPresetIndex(p.name);
        PresetValues base = shipped >= 0 ? shippedPresetValues(shipped) : PresetValues{};
        const double shippedBands = base[pDensBands];
        if (shipped >= 0 && p.values[pDensBands] != shippedBands) {
            // the same base as fromText(): a changed band count brings its own default splits, so a split equal to
            // the shipped value but not to the new count's default is written too
            base[pDensBands] = p.values[pDensBands];
            loadDefaultSplits(base, static_cast<int>(base[pDensBands]));
        }
        o << "\n[" << kPresetPrefix << p.name << "]\n";
        if (shipped >= 0) o << "; Shipped preset: only values that differ from the shipped ones are listed\n";
        for (int i = 0; i < kFirstColourParam; ++i) {
            const ParamInfo& info = paramInfo(i);
            const double v = p.values[static_cast<size_t>(i)];
            const double ref = i == pDensBands ? shippedBands : base[static_cast<size_t>(i)];
            if (shipped >= 0 && std::fabs(v - ref) < 1e-9) continue;
            if (i >= pDensSplit1 && i <= pDensSplit6 && i - pDensSplit1 + 1 >= static_cast<int>(p.values[pDensBands]) &&
                shipped < 0)
                continue; // unused split
            o << "; " << info.stage << " - " << info.name << ": " << info.help;
            if (info.isSwitch)
                o << " (1 = on, 0 = off";
            else
                o << " (" << info.unit << (info.unit[0] ? ", " : "") << number(info.min, info.decimals) << ".."
                  << number(info.max, info.decimals);
            o << ", reset value " << number(resetValueOf(p.name, i), info.decimals) << ")\n";
            o << info.key << "=" << compact(v) << "\n";
        }
    }
    for (const Preset& p : colours) {
        const bool shipped = shippedColourIndex(p.name) >= 0;
        const PresetValues base = shippedValues(p.name, PresetKind::Colour);
        o << "\n[" << kColourPrefix << p.name << "]\n";
        if (shipped) o << "; Shipped EQ: only values that differ from the shipped ones are listed\n";
        for (int i = kFirstColourParam; i < kParamCount; ++i) {
            const ParamInfo& info = paramInfo(i);
            const double v = p.values[static_cast<size_t>(i)];
            if (shipped && std::fabs(v - base[static_cast<size_t>(i)]) < 1e-9) continue;
            o << "; " << info.stage << " - " << info.name << ": " << info.help << " (" << info.unit
              << (info.unit[0] ? ", " : "") << number(info.min, info.decimals) << ".." << number(info.max, info.decimals)
              << ", reset value " << number(base[static_cast<size_t>(i)], info.decimals) << ")\n";
            o << info.key << "=" << compact(v) << "\n";
        }
    }
    return o.str();
}

void Settings::fromText(const std::string& text) {
    *this = Settings();
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::vector<std::string> order;
    std::istringstream in(text);
    std::string line, section;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3); // BOM
        first = false;
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (!sections.count(section)) order.push_back(section);
            sections[section];
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        sections[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    auto get = [&](const char* s, const char* k) -> const std::string* {
        auto a = sections.find(s);
        if (a == sections.end()) return nullptr;
        auto b = a->second.find(k);
        return b == a->second.end() ? nullptr : &b->second;
    };
    if (auto v = get("Audio", "Api"))
        if (*v == "ASIO" || *v == "WASAPI" || *v == "DirectSound" || *v == "MME") api = *v;
    if (auto v = get("Audio", "Decks")) decks = toInt(*v, 2) == 1 ? 1 : 2;
    if (auto v = get("Audio", "Driver")) driver = *v;
    if (auto v = get("Audio", "InputDeviceA")) inDevice[0] = *v;
    if (auto v = get("Audio", "OutputDeviceA")) outDevice[0] = *v;
    if (auto v = get("Audio", "InputDeviceB")) inDevice[1] = *v;
    if (auto v = get("Audio", "OutputDeviceB")) outDevice[1] = *v;
    if (auto v = get("Audio", "Exclusive")) exclusive = toInt(*v, 0) != 0;
    if (auto v = get("Audio", "DriftCorrection")) { // 0 / 1 / 2; anything else is the default (sixth outside review:
        const int m = toInt(*v, kDriftAuto);         // a clamp made 7 "always" and -3 "off")
        driftCorrection = m >= kDriftOff && m <= kDriftAlways ? m : kDriftAuto;
    }
    if (auto v = get("Audio", "AsioInputs"))
        if (*v == "ASIO" || *v == "WASAPI" || *v == "DirectSound" || *v == "MME") asioInputs = *v;
    // (the former ShortInputPeriod=1 asked for the device's shortest period and is not taken over: back to 10 ms)
    if (auto v = get("Audio", "InputPeriodMs")) {
        const int ms = toInt(*v, 0);
        inputPeriodMs = ms == 8 || ms == 6 || ms == 5 || ms == 4 ? ms : 0;
    }
    if (auto v = get("Audio", "AsioInputDeviceA")) asioInputDevice[0] = *v;
    if (auto v = get("Audio", "AsioInputDeviceB")) asioInputDevice[1] = *v;
    if (auto v = get("Audio", "SampleRate")) sampleRate = toInt(*v, sampleRate);
    if (sampleRate != 44100 && sampleRate != 48000 && sampleRate != 88200 && sampleRate != 96000) sampleRate = 48000;
    if (auto v = get("Audio", "InputA")) inputA = std::max(1, toInt(*v, inputA));
    if (auto v = get("Audio", "InputB")) inputB = std::max(1, toInt(*v, inputB));
    if (auto v = get("Audio", "OutputA")) outputA = std::max(1, toInt(*v, outputA));
    if (auto v = get("Audio", "OutputB")) outputB = std::max(1, toInt(*v, outputB));
    if (auto v = get("Audio", "LookAheadMs")) {
        double d;
        if (parseDouble(*v, d)) lookAheadMs = std::min(5.0, std::max(0.0, d));
        // the old choices 0.5 and 1.0 ms: under the true-peak minimum, they become the default (deck.h, lookFrames)
        if (lookAheadMs > 0.0 && lookAheadMs < 1.5) lookAheadMs = 1.5;
    }
    if (auto v = get("Audio", "Oversampling")) {
        const int f = toInt(*v, 4);
        oversampling = f >= 6 ? 8 : f >= 3 ? 4 : 2; // 2, 4 or 8
    }
    if (auto v = get("Audio", "Mono")) mono = toInt(*v, 0) != 0;
    if (auto v = get("Audio", "BufferSize")) bufferSize = std::min(8192, std::max(0, toInt(*v, 0)));
    if (auto v = get("Audio", "StartAudio")) startAudio = toInt(*v, 0) != 0;
    if (auto v = get("Audio", "FollowWindowsVolumeOneChannel")) followVolume[0] = toInt(*v, 1) != 0;
    if (auto v = get("Audio", "FollowWindowsVolumeTwoDecks")) followVolume[1] = toInt(*v, 0) != 0;
    // every window's place: the program's (Main, Setup, Loudness, ...) and the plug-in's (PluginSpectrum, ...) - any
    // line of the section that reads as a place (was a fixed list of the program's windows: the plug-in's About and,
    // since 2026-10-29, its other windows were never read back)
    for (const auto& [name, value] : sections["Window"])
        if (name != "X" && name != "Y") { // (the switches in the section do not read as places)
            WindowPlace p;
            std::istringstream f(value);
            char c[4] = {};
            int max = 0;
            if ((f >> p.x >> c[0] >> p.y >> c[1] >> p.w >> c[2] >> p.h) && c[0] == ',' && c[1] == ',' && c[2] == ',' &&
                p.w >= 0 && p.h >= 0) {
                if ((f >> c[3] >> max) && c[3] == ',') p.maximised = max != 0;
                windows[name] = p;
            }
        }
    // versions up to 0.1.132 kept only the main window's position, as X and Y
    if (!windows.count("Main"))
        if (auto x = get("Window", "X"))
            if (auto y = get("Window", "Y"))
                if (toInt(*x, -1) >= 0) windows["Main"] = WindowPlace{toInt(*x, 0), toInt(*y, 0), 0, 0, false};
    if (auto v = get("Window", "SnapToBands")) snapToBands = toInt(*v, 0) != 0;
    if (auto v = get("Window", "Tooltips")) showTooltips = toInt(*v, 1) != 0;
    if (auto v = get("Window", "TrayHintShown")) trayHintShown = toInt(*v, 0) != 0;
    if (auto v = get("Updates", "CheckUpdates")) checkUpdates = toInt(*v, 1) != 0;
    if (auto v = get("Updates", "Url")) updateUrl = *v;
    // the plug-in's choices: "Loudness1", "Colour1", ... - a key and its instance number
    if (auto v = get("Plugin", "Link")) pluginLink = toInt(*v, 0) != 0;
    for (const auto& [key, value] : sections["Plugin"]) {
        const size_t digits = key.find_first_of("0123456789");
        if (digits == std::string::npos || digits == 0) continue;
        const int n = toInt(key.substr(digits), 0);
        if (n < 1 || n > 64) continue;
        const std::string what = key.substr(0, digits);
        PluginSlot& c = pluginSlots[n];
        double d = 0;
        if (what == "Loudness") c.preset = value;
        else if (what == "Colour") c.colour = value;
        else if (what == "LookAhead" && parseDouble(value, d)) c.lookAheadMs = std::min(5.0, std::max(0.0, d));
        else if (what == "Oversampling") c.oversampling = toInt(value, 4) >= 6 ? 8 : toInt(value, 4) >= 3 ? 4 : 2;
        else if (what == "Mono") c.mono = toInt(value, 0) != 0;
    }

    const std::string prefix = kPresetPrefix;
    for (const std::string& s : order) {
        if (s.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string name = s.substr(prefix.size());
        std::string error;
        if (!validName(name, error)) continue;
        const int shipped = shippedPresetIndex(name);
        PresetValues values = shipped >= 0 ? shippedPresetValues(shipped) : defaultValues();
        const auto& keys = sections[s];
        auto bandsIt = keys.find(paramInfo(pDensBands).key);
        if (bandsIt != keys.end()) { // a changed band count brings its own default splits
            double d;
            if (parseDouble(bandsIt->second, d)) {
                values[pDensBands] = std::round(d);
                normaliseValues(values);
                loadDefaultSplits(values, static_cast<int>(values[pDensBands]));
            }
        }
        for (int i = 0; i < kFirstColourParam; ++i) {
            auto it = keys.find(paramInfo(i).key);
            double d;
            if (it != keys.end() && parseDouble(it->second, d)) values[static_cast<size_t>(i)] = d;
        }
        // a preset of the versions before Attack / Release (2026-10-27): its "Fast down" in dB/s is the Attack in
        // seconds per 10 dB (0 was "the normal speed": Speed); "Catch up after a pause" is gone (the Release speed now)
        if (keys.find(paramInfo(pLevAttack).key) == keys.end()) {
            auto old = keys.find("LevellerFastDown");
            double d;
            if (old != keys.end() && parseDouble(old->second, d))
                values[pLevAttack] = attackFromFastDown(d, values[pLevSpeed]);
        }
        normaliseValues(values);
        if (Preset* p = find(name)) {
            p->values = values;
        } else {
            Preset p2;
            p2.name = name;
            p2.values = values;
            presets.push_back(p2);
        }
    }
    sortPresets();
    const std::string colourPrefix = kColourPrefix;
    for (const std::string& s : order) {
        if (s.compare(0, colourPrefix.size(), colourPrefix) != 0) continue;
        const std::string name = s.substr(colourPrefix.size());
        std::string error;
        if (!validName(name, error)) continue;
        PresetValues values = shippedValues(name, PresetKind::Colour);
        const auto& keys = sections[s];
        for (int i = kFirstColourParam; i < kParamCount; ++i) {
            auto it = keys.find(paramInfo(i).key);
            double d;
            if (it != keys.end() && parseDouble(it->second, d)) values[static_cast<size_t>(i)] = d;
        }
        normaliseValues(values);
        if (Preset* p = find(name, PresetKind::Colour)) {
            p->values = values;
        } else {
            Preset p2;
            p2.name = name;
            p2.values = values;
            colours.push_back(p2);
        }
    }
    for (int d = 0; d < 2; ++d) {
        const char* pk = d == 0 ? "PresetA" : "PresetB";
        const char* ok = d == 0 ? "ProcessingA" : "ProcessingB";
        if (auto v = get("Decks", pk))
            if (find(*v)) deck[d].preset = *v;
        if (auto v = get("Decks", ok)) deck[d].processing = toInt(*v, 1) != 0;
        if (auto v = get("Decks", d == 0 ? "ColourA" : "ColourB"))
            if (find(*v, PresetKind::Colour)) deck[d].colour = *v;
    }
    if (auto v = get("Decks", "LinkPresets")) linkPresets = toInt(*v, 0) != 0;
    if (linkPresets) { // a hand edit cannot leave them apart
        deck[1].preset = deck[0].preset;
        deck[1].colour = deck[0].colour;
    }
}

bool Settings::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return false;
    fromText(decodeText(ss.str()));
    return true;
}

bool Settings::save(const std::filesystem::path& file) const {
    // a temporary name of this writer's own: the program and a plug-in may save the same file at the same moment
    std::filesystem::path tmp = file;
    tmp += "." + std::to_string((reinterpret_cast<uintptr_t>(this) >> 4) ^
                                static_cast<uintptr_t>(std::chrono::steady_clock::now().time_since_epoch().count())) +
           ".tmp";
    std::error_code ec;
    {
        std::FILE* f = nullptr;
#ifdef _WIN32
        if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) f = nullptr;
#else
        f = std::fopen(tmp.c_str(), "wb");
#endif
        if (!f) return false;
        const std::string text = toText();
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() && std::fflush(f) == 0;
        // on the disk before the rename: after a power cut the file is the old one or the new one, never an empty
        // one - it holds the user's presets (fourth outside review)
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = std::fclose(f) == 0 && ok; // a full disk shows here at the latest
        if (!ok) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // a virus scanner or the plug-in reading the file can hold it for a moment: a few tries
    for (int attempt = 0;; ++attempt) {
        std::filesystem::rename(tmp, file, ec);
        if (!ec) return true;
        if (attempt == 4) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::filesystem::remove(tmp, ec);
    return false;
}

namespace {

Preset* named(std::vector<Preset>& list, const std::string& name) {
    for (Preset& p : list)
        if (p.name == name) return &p;
    return nullptr;
}

const Preset* named(const std::vector<Preset>& list, const std::string& name) {
    for (const Preset& p : list)
        if (p.name == name) return &p;
    return nullptr;
}

// `result` holds the file's list; this writer's changes since `base` (`ours` against `base`) are applied to it.
void mergeList(std::vector<Preset>& result, const std::vector<Preset>& ours, const std::vector<Preset>& base) {
    for (const Preset& b : base) // deleted or renamed away here (shipped ones are never deleted)
        if (!b.shipped && !named(ours, b.name))
            result.erase(std::remove_if(result.begin(), result.end(), [&](const Preset& r) { return r.name == b.name; }),
                         result.end());
    for (const Preset& p : ours) { // added or changed here
        const Preset* b = named(base, p.name);
        if (b && b->values == p.values) continue; // not touched here: the file's version stays
        if (Preset* r = named(result, p.name)) r->values = p.values;
        else result.push_back(p);
    }
}

} // namespace

bool Settings::saveMerged(const std::filesystem::path& file, Settings& base, SaveRole role) {
    Settings now; // the file as it is now (the shipped lists when there is none yet)
    std::error_code ec;
    const bool existed = std::filesystem::exists(file, ec);
    // a file that is there but cannot be read is never overwritten: merged against the shipped lists alone, it would
    // lose every own preset this writer did not change
    if (existed && !now.load(file)) return false;
    Settings out = role == SaveRole::Owner ? *this : now;
    out.presets = now.presets;
    out.colours = now.colours;
    mergeList(out.presets, presets, base.presets);
    mergeList(out.colours, colours, base.colours);
    // what this writer's decks use stays, even if the other deleted it meanwhile
    for (const DeckSettings& d : deck) {
        if (!named(out.presets, d.preset))
            if (const Preset* p = named(presets, d.preset)) out.presets.push_back(*p);
        if (!named(out.colours, d.colour))
            if (const Preset* c = named(colours, d.colour)) out.colours.push_back(*c);
    }
    out.sortPresets();
    // the switches both can change (the plug-in's Setup window, client 2026-10-29): three-way like the lists - a
    // change this writer made since `base` is written, else the file's value stays (the other may have changed it)
    for (bool Settings::*f : {&Settings::showTooltips, &Settings::checkUpdates, &Settings::pluginLink}) {
        out.*f = this->*f != base.*f ? this->*f : now.*f;
        this->*f = base.*f = out.*f;
    }
    // the windows' places, per window (client, 2026-10-29: the plug-in's windows remember theirs too): a window this
    // writer moved since `base` is written, every other one stays as the file has it
    out.windows = now.windows;
    for (const auto& [name, place] : windows) {
        const auto b = base.windows.find(name);
        if (b == base.windows.end() || !(b->second == place)) out.windows[name] = place;
    }
    windows = base.windows = out.windows;
    // the plug-in's choices, per instance, likewise (client, 2026-10-30)
    out.pluginSlots = now.pluginSlots;
    for (const auto& [n, slot] : pluginSlots) {
        const auto b = base.pluginSlots.find(n);
        if (b == base.pluginSlots.end() || !(b->second == slot)) out.pluginSlots[n] = slot;
    }
    pluginSlots = base.pluginSlots = out.pluginSlots;
    // nothing new (a guest only reading the other's presets): the file stays untouched
    const bool same = (existed || role == SaveRole::Guest) && out.toText() == now.toText();
    if (!same && !out.save(file)) return false;
    presets = out.presets;
    colours = out.colours;
    base.presets = presets;
    base.colours = colours;
    return true;
}

} // namespace ap
