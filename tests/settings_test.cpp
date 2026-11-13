// SPDX-License-Identifier: GPL-3.0-or-later
// Settings and presets: ini round trip, shipped presets stored as differences, rename / delete / reset rules,
// hand-edited and damaged files (docs/design.md 3.1, 8).
#include "check.h"
#include "settings.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace ap;
namespace fs = std::filesystem;

namespace {

// Value lines (key=value) in the section of a preset.
int valueLines(const std::string& text, const std::string& preset) {
    std::istringstream in(text);
    std::string line;
    bool inside = false;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            inside = line == "[Preset " + preset + "]" || line == "[" + preset + "]"; // "Colour X": its own section
            continue;
        }
        if (inside && !line.empty() && line[0] != ';' && line.find('=') != std::string::npos) ++n;
    }
    return n;
}

bool sameValues(const PresetValues& a, const PresetValues& b, int* firstDiff = nullptr) {
    for (int i = 0; i < kParamCount; ++i)
        if (std::fabs(a[static_cast<size_t>(i)] - b[static_cast<size_t>(i)]) > 1e-9) {
            if (firstDiff) *firstDiff = i;
            return false;
        }
    return true;
}

void checkSame(const Settings& a, const Settings& b) {
    CHECK(a.driver == b.driver);
    CHECK(a.sampleRate == b.sampleRate);
    CHECK(a.inputA == b.inputA && a.inputB == b.inputB && a.outputA == b.outputA && a.outputB == b.outputB);
    CHECK_NEAR(a.lookAheadMs, b.lookAheadMs, 1e-12);
    CHECK(a.oversampling == b.oversampling);
    CHECK(a.mono == b.mono);
    CHECK(a.bufferSize == b.bufferSize && a.startAudio == b.startAudio);
    CHECK(a.api == b.api && a.exclusive == b.exclusive && a.driftCorrection == b.driftCorrection);
    CHECK(a.inputPeriodMs == b.inputPeriodMs);
    for (int d = 0; d < 2; ++d) CHECK(a.inDevice[d] == b.inDevice[d] && a.outDevice[d] == b.outDevice[d]);
    CHECK(a.asioInputs == b.asioInputs && a.decks == b.decks);
    CHECK(a.followVolume[0] == b.followVolume[0] && a.followVolume[1] == b.followVolume[1]);
    for (int d = 0; d < 2; ++d) CHECK(a.asioInputDevice[d] == b.asioInputDevice[d]);
    for (int d = 0; d < 2; ++d) {
        CHECK(a.deck[d].preset == b.deck[d].preset);
        CHECK(a.deck[d].colour == b.deck[d].colour);
        CHECK(a.deck[d].processing == b.deck[d].processing);
        CHECK(a.linkPresets == b.linkPresets);
    }
    CHECK(a.colours.size() == b.colours.size());
    for (size_t i = 0; i < a.colours.size() && i < b.colours.size(); ++i) {
        CHECK(a.colours[i].name == b.colours[i].name && a.colours[i].shipped == b.colours[i].shipped);
        int diff = -1;
        const bool same = sameValues(a.colours[i].values, b.colours[i].values, &diff);
        if (!same)
            std::printf("   colour \"%s\": %s %.4f -> %.4f\n", a.colours[i].name.c_str(), paramInfo(diff).key,
                        a.colours[i].values[static_cast<size_t>(diff)], b.colours[i].values[static_cast<size_t>(diff)]);
        CHECK(same);
    }
    CHECK(a.windows.size() == b.windows.size() && a.trayHintShown == b.trayHintShown);
    for (const auto& [name, p] : a.windows) {
        auto it = b.windows.find(name);
        CHECK(it != b.windows.end() && it->second.x == p.x && it->second.y == p.y && it->second.w == p.w &&
              it->second.h == p.h && it->second.maximised == p.maximised);
    }
    CHECK(a.snapToBands == b.snapToBands && a.showTooltips == b.showTooltips);
    CHECK(a.checkUpdates == b.checkUpdates && a.updateUrl == b.updateUrl);
    CHECK(a.fallbackOutput == b.fallbackOutput && a.retryMinutes == b.retryMinutes);
    for (int i = 0; i < 3; ++i) CHECK(a.scope[i] == b.scope[i]);
    for (int i = 0; i < 2; ++i) CHECK(a.spectrumAverageMs[i] == b.spectrumAverageMs[i]);
    CHECK(a.latencyProbe == b.latencyProbe);
    CHECK(a.presets.size() == b.presets.size());
    for (size_t i = 0; i < a.presets.size() && i < b.presets.size(); ++i) {
        CHECK(a.presets[i].name == b.presets[i].name);
        CHECK(a.presets[i].shipped == b.presets[i].shipped);
        int diff = -1;
        const bool same = sameValues(a.presets[i].values, b.presets[i].values, &diff);
        if (!same)
            std::printf("   preset \"%s\": %s %.4f -> %.4f\n", a.presets[i].name.c_str(), paramInfo(diff).key,
                        a.presets[i].values[static_cast<size_t>(diff)], b.presets[i].values[static_cast<size_t>(diff)]);
        CHECK(same);
        if (!a.presets[i].shipped) CHECK(sameValues(a.presets[i].saved, b.presets[i].saved)); // its reset values
    }
    for (size_t i = 0; i < a.colours.size() && i < b.colours.size(); ++i)
        if (!a.colours[i].shipped) CHECK(sameValues(a.colours[i].saved, b.colours[i].saved));
}

fs::path tempDir() {
    // a folder of this run's own (two runs at once, or one stopped half-way, do not meet)
    const fs::path d = fs::temp_directory_path() /
                       ("DJAP_settings_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

void defaults() {
    std::printf("-- defaults: the shipped presets, stored as differences only\n");
    Settings s;
    CHECK(s.presets.size() == static_cast<size_t>(kShippedPresetCount));
    for (int i = 0; i < kShippedPresetCount; ++i) {
        const Preset* p = s.find(shippedPresetName(i));
        CHECK(p && p->shipped && sameValues(p->values, shippedPresetValues(i)));
    }
    // listed from the quietest output to the loudest (target + output gain)
    std::string order;
    for (const Preset& p : s.presets) order += (order.empty() ? "" : ", ") + p.name;
    std::printf("   order: %s\n", order.c_str());
    CHECK(order == "DAB+, Gentle, Light, Web Stream, Medium, FM Station, Loud, Very Loud");
    Settings own = s;
    PresetValues v = shippedPresetValues(shippedPresetIndex("Medium"));
    v[pTarget] = -11;
    std::string err;
    CHECK(own.saveAs("Club", v, err));
    // an own preset at -11 LUFS takes its place between Loud (-12) and Very Loud (-10)
    CHECK(own.presets.size() == 9 && own.presets[6].name == "Loud" && own.presets[7].name == "Club" &&
          own.presets[8].name == "Very Loud");
    const std::string text = s.toText();
    int lines = 0;
    for (int i = 0; i < kShippedPresetCount; ++i) lines += valueLines(text, shippedPresetName(i));
    std::printf("   value lines in untouched shipped presets: %d\n", lines);
    CHECK(lines == 0);
    CHECK(s.deck[0].preset == "Medium" && s.deck[1].preset == "Medium");
    // first start: no device chosen (the program opens the Audio window); a driver, or a device of another API
    CHECK(!s.audioConfigured() && !s.trayHintShown);
    CHECK(s.fallbackOutput.empty() && s.retryMinutes == 5);
    CHECK(s.checkUpdates && s.updateUrl.empty()); // the daily update check is on unless switched off
    Settings a = s;
    a.driver = "Some ASIO Driver";
    CHECK(a.audioConfigured());
    a.api = "WASAPI";
    CHECK(!a.audioConfigured());
    a.outDevice[1] = "Speakers";
    CHECK(a.audioConfigured());

    Settings c = s;
    c.find("Loud")->values[pCompRatio] = 3.2;
    CHECK(valueLines(c.toText(), "Loud") == 1);

    // colours: nine shipped, none written untouched, a deck starts on Neutral, a dynamics preset writes no colour
    CHECK(s.colours.size() == static_cast<size_t>(kShippedColourCount));
    CHECK(s.deck[0].colour == "Neutral" && s.deck[1].colour == "Neutral");
    for (int i = 0; i < kShippedColourCount; ++i) CHECK(valueLines(text, std::string("Colour ") + shippedColourName(i)) == 0);
    CHECK(text.find("BassCorner") == std::string::npos); // no colour key in any section
    c.find("Warm", PresetKind::Colour)->values[pToneTreble] = -3;
    const std::string ct = c.toText();
    CHECK(valueLines(ct, "Colour Warm") == 1);
    CHECK(valueLines(ct, "Loud") == 1); // the colour change did not leak into a dynamics section
}

// A settings object with something changed in every part of the file.
Settings busy() {
    Settings s;
    std::string err;
    s.driver = "Some ASIO Driver (x64)";
    s.sampleRate = 44100;
    s.inputA = 5;
    s.inputB = 7;
    s.outputA = 3;
    s.outputB = 1;
    s.lookAheadMs = 2.25;
    s.oversampling = 8;
    s.mono = true; // (stereo by default)
    s.bufferSize = 192;
    s.startAudio = true;
    s.followVolume[0] = false; // both the other way round than the defaults
    s.followVolume[1] = true;
    s.api = "WASAPI";
    s.exclusive = true;
    s.driftCorrection = Settings::kDriftAlways; // not the default
    s.inputPeriodMs = 4;                        // not the default
    s.inDevice[0] = "FOC-Analogue 1 + 2 (2- Focusrite USB Audio)"; // device names with + ( ) - and UTF-8
    s.outDevice[0] = "ROD-Headphones (R\xC3\x98" "DE PodMic USB)";
    s.inDevice[1] = s.outDevice[1] = "";
    s.asioInputs = "DirectSound";
    s.decks = 1;
    s.asioInputDevice[0] = "";
    s.asioInputDevice[1] = "Line In (Realtek(R) Audio)";
    s.windows["Main"] = WindowPlace{120, -40, 1200, 700, false};
    s.windows["Spectrum"] = WindowPlace{-1500, 30, 1180, 640, true}; // a monitor left of the main one, maximised
    s.snapToBands = true;
    s.showTooltips = false;
    s.trayHintShown = true;
    s.fallbackOutput = "Speakers (USB Audio)";
    s.retryMinutes = 10;
    s.scope[0] = {true, 5};
    s.scope[2] = {false, 2000};
    s.spectrumAverageMs[0] = 0;
    s.spectrumAverageMs[1] = 1000;
    s.latencyProbe = false;
    s.checkUpdates = false;
    s.updateUrl = "http://127.0.0.1:8765/latest.json";
    // shipped preset with values changed, among them the band count
    PresetValues& med = s.find("Medium")->values;
    med[pTarget] = -13.5;
    med[pCompRatio] = 2.3;
    med[pDensBands] = 7;
    loadDefaultSplits(med, 7);
    med[pDensSplit2] = 200; // Medium's own 5-band value for split 2, but not the 7-band default (150)
    normaliseValues(med);
    // own presets: UTF-8 name, other band count, switches
    PresetValues v = shippedPresetValues(1);
    v[pDensBands] = 3;
    loadDefaultSplits(v, 3);
    v[pDensSplit1] = 180;
    v[pWidthOn] = 1;
    v[pWidth] = 135;
    v[pLimCeiling] = -1.5;
    CHECK(s.saveAs("Zachte avond \xC3\xA9\xC3\xA9n", v, err)); // "Zachte avond één"
    PresetValues w = shippedPresetValues(4);
    w[pClipDrive] = 5.5;
    w[pLevOn] = 0;
    CHECK(s.saveAs("Club", w, err));
    s.deck[0].preset = "Club";
    s.deck[0].processing = false;
    s.deck[1].preset = "Zachte avond \xC3\xA9\xC3\xA9n";
    // colours: a shipped one changed, an own one, a colour per deck
    s.find("Oldies", PresetKind::Colour)->values[pSubAmount] = 35;
    PresetValues col = shippedColourValues(shippedColourIndex("Warm"));
    col[pToneBass] = 4.5;
    col[pToneBassFreq] = 120;
    col[pLoudness] = 25;
    CHECK(s.saveAs("Jaren 60 \xC3\xA9\xC3\xA9n", col, err, PresetKind::Colour));
    s.deck[0].colour = "Oldies";
    s.deck[1].colour = "Jaren 60 \xC3\xA9\xC3\xA9n";
    return s;
}

void roundTrip(const fs::path& dir) {
    std::printf("-- ini round trip (file on disk, UTF-8 names, changed shipped preset, own presets)\n");
    const Settings a = busy();
    const fs::path file = dir / "DJAP.ini";
    CHECK(a.save(file));
    Settings b;
    CHECK(b.load(file));
    checkSame(a, b);
    const std::string t1 = a.toText(), t2 = b.toText();
    std::printf("   %zu bytes; saved again identical: %s\n", t1.size(), t1 == t2 ? "yes" : "NO");
    CHECK(t1 == t2);
    // saving over an existing file replaces it and leaves no temporary file behind
    Settings c = b;
    c.find("Club")->values[pTarget] = -9.5;
    CHECK(c.save(file));
    Settings d;
    CHECK(d.load(file));
    CHECK_NEAR(d.find("Club")->values[pTarget], -9.5, 1e-12);
    fs::path tmp = file;
    tmp += ".tmp";
    CHECK(!fs::exists(tmp));
    // a missing file: load says so and changes nothing
    Settings e = a;
    CHECK(!e.load(dir / "missing.ini"));
    checkSame(a, e);
}

void presetRules() {
    std::printf("-- save as, rename, delete, reset\n");
    Settings s;
    std::string err;
    PresetValues v = defaultValues();
    v[pTarget] = -40; // out of range: clamped
    CHECK(!s.saveAs("Medium", v, err));
    std::printf("   save as \"Medium\": %s\n", err.c_str());
    CHECK(s.saveAs("Mine", v, err));
    CHECK_NEAR(s.find("Mine")->values[pTarget], paramInfo(pTarget).min, 1e-12);
    v[pTarget] = 3; // over the range, saved over an existing own preset: clamped as well
    CHECK(s.saveAs("Mine", v, err));
    CHECK_NEAR(s.find("Mine")->values[pTarget], paramInfo(pTarget).max, 1e-12);
    for (const char* bad : {"", " Mine2", "Mine2 ", "a=b", "[x]", "semi;colon"}) {
        CHECK(!s.saveAs(bad, v, err));
    }
    CHECK(s.presets.size() == static_cast<size_t>(kShippedPresetCount + 1));

    // rename: own presets only, decks follow
    s.deck[0].preset = "Mine";
    CHECK(s.rename("Mine", "Mine (club)", err));
    CHECK(s.find("Mine") == nullptr && s.find("Mine (club)") != nullptr);
    CHECK(s.deck[0].preset == "Mine (club)");
    CHECK(!s.rename("Loud", "Louder", err));
    std::printf("   rename \"Loud\": %s\n", err.c_str());
    CHECK(!s.rename("Mine (club)", "Light", err));
    CHECK(!s.rename("Mine (club)", " x", err));
    CHECK(!s.rename("Nobody", "x", err));
    CHECK(s.rename("Mine (club)", "Mine (club)", err));

    // delete: own presets only, decks fall back to Medium
    s.deck[1].preset = "Mine (club)";
    CHECK(!s.remove("Gentle", err));
    std::printf("   delete \"Gentle\": %s\n", err.c_str());
    CHECK(!s.remove("Nobody", err));
    CHECK(s.remove("Mine (club)", err));
    CHECK(s.deck[0].preset == "Medium" && s.deck[1].preset == "Medium");
    CHECK(s.presets.size() == static_cast<size_t>(kShippedPresetCount));

    // reset: shipped preset to its shipped values, own preset to the generic defaults; one value at a time
    PresetValues& loud = s.find("Loud")->values;
    loud[pTarget] = -20;
    loud[pCompRatio] = 6;
    s.resetValue("Loud", pTarget);
    CHECK_NEAR(loud[pTarget], shippedPresetValues(3)[pTarget], 1e-12);
    CHECK_NEAR(loud[pCompRatio], 6, 1e-12);
    loud[pDensBands] = 2;
    loadDefaultSplits(loud, 2);
    s.resetValue("Loud", pDensBands); // back to 6 bands brings the 6-band splits
    CHECK(sameValues(loud, [] {
        PresetValues x = shippedPresetValues(3);
        x[pCompRatio] = 6;
        return x;
    }()));
    s.resetPreset("Loud");
    CHECK(sameValues(loud, shippedPresetValues(3)));
    CHECK(s.saveAs("Own", shippedPresetValues(4), err));
    s.find("Own")->values[pTarget] = -20;
    s.find("Own")->values[pCompRatio] = 6;
    s.resetPreset("Own"); // an own preset goes back to the values it was saved with (until 2026-11-12: the generic defaults)
    CHECK(sameValues(s.find("Own")->values, shippedPresetValues(4)));
    CHECK_NEAR(s.resetValueOf("Gentle", pLevRange), 12, 1e-12);
    CHECK_NEAR(s.resetValueOf("Own", pTarget), shippedPresetValues(4)[pTarget], 1e-12);
    CHECK_NEAR(s.resetValueOf("No such preset", pTarget), paramInfo(pTarget).def, 1e-12);

    // colour presets: the same rules, in their own list; decks fall back to Neutral
    const PresetKind C = PresetKind::Colour;
    CHECK(!s.saveAs("Warm", shippedColourValues(0), err, C)); // a shipped colour
    CHECK(s.saveAs("Warm", shippedColourValues(0), err));      // the same name as a dynamics preset is fine
    CHECK(s.saveAs("Mijn kleur", shippedColourValues(1), err, C));
    s.deck[0].colour = s.deck[1].colour = "Mijn kleur";
    CHECK(s.rename("Mijn kleur", "Club kleur", err, C));
    CHECK(s.deck[0].colour == "Club kleur" && s.deck[1].colour == "Club kleur");
    CHECK(!s.rename("Bass+", "x", err, C));
    s.find("Oldies", C)->values[pAutoAmount] = 10;
    s.resetValue("Oldies", pAutoAmount); // the kind follows the parameter
    CHECK_NEAR(s.find("Oldies", C)->values[pAutoAmount], 70, 1e-12);
    s.find("Club kleur", C)->values[pToneBass] = 7;
    s.resetPreset("Club kleur", C); // back to what it was saved with
    CHECK(sameValues(s.find("Club kleur", C)->values, shippedColourValues(1)));
    CHECK(s.remove("Club kleur", err, C));
    CHECK(s.deck[0].colour == "Neutral" && s.deck[1].colour == "Neutral");
    CHECK(s.colours.size() == static_cast<size_t>(kShippedColourCount));
    // what a deck runs: its preset with its colour
    s.deck[0].preset = "Loud";
    s.deck[0].colour = "Bass+";
    const PresetValues run = s.deckValues(0);
    CHECK_NEAR(run[pTarget], shippedPresetValues(3)[pTarget], 1e-12);
    CHECK_NEAR(run[pToneBass], shippedColourValues(shippedColourIndex("Bass+"))[pToneBass], 1e-12);
}

void handEdited() {
    std::printf("-- hand-edited and damaged files\n");
    const std::string text = "\xEF\xBB\xBF; edited by hand\r\n"
                             "[Audio]\r\n"
                             "Driver = My Card \r\n"
                             "Api=CoreAudio\r\n" // not a Windows API: stays ASIO
                             "AsioInputs=JACK\r\n" // the same
                             "Decks=3\r\n"          // only 1 or 2: two decks
                             "SampleRate=12345\r\n"
                             "LookAheadMs=9\r\n"
                             "BufferSize=-5\r\n"
                             "InputA=abc\r\n"
                             "Unknown=1\r\n"
                             "[Decks]\r\n"
                             "PresetA=Does not exist\r\n"
                             "PresetB=Loud\r\n"
                             "ProcessingB=0\r\n"
                             "[Preset Medium]\r\n"
                             "DensityBands=7\r\n"
                             "Target=-200\r\n"
                             "CompressorRatio=nan\r\n"
                             "CompressorKnee=1e999\r\n"
                             "LimiterCeiling=-2\r\n"
                             "LimiterCeiling=-3\r\n" // the last one counts
                             "[Preset Bad=Name]\r\n"
                             "Target=-10\r\n"
                             "[Preset Hand made]\r\n"
                             "Target=-11\r\n"
                             "DensitySplit1=900\r\n"
                             "DensitySplit2=300\r\n" // below split 1: pushed up
                             "[Something else]\r\n"
                             "x=y\r\n";
    Settings s;
    s.fromText(text);
    CHECK(s.driver == "My Card");
    CHECK(s.api == "ASIO" && s.asioInputs == "ASIO" && s.decks == 2);
    CHECK(s.sampleRate == 48000);
    CHECK_NEAR(s.lookAheadMs, 5.0, 1e-12);
    CHECK(s.bufferSize == 0 && !s.startAudio);
    CHECK(s.followVolume[0] && !s.followVolume[1] && !s.followsVolume()); // defaults; two decks: off
    CHECK(s.inputA == 1);
    CHECK(s.deck[0].preset == "Medium");
    CHECK(s.deck[1].preset == "Loud" && !s.deck[1].processing);
    const PresetValues& m = s.find("Medium")->values;
    PresetValues want = shippedPresetValues(2);
    want[pDensBands] = 7;
    loadDefaultSplits(want, 7);
    want[pTarget] = paramInfo(pTarget).min;
    want[pLimCeiling] = -3;
    CHECK(sameValues(m, want));
    CHECK(s.find("Bad=Name") == nullptr);
    const Preset* h = s.find("Hand made");
    CHECK(h != nullptr && !h->shipped);
    if (h) {
        CHECK_NEAR(h->values[pTarget], -11, 1e-12);
        CHECK(h->values[pDensSplit2] >= h->values[pDensSplit1] * 1.25 - 1e-9);
        std::printf("   hand-made splits 900 / 300 -> %.1f / %.1f\n", h->values[pDensSplit1], h->values[pDensSplit2]);
    }
    CHECK(s.presets.size() == static_cast<size_t>(kShippedPresetCount + 1));

    Settings e;
    e.fromText("");
    checkSame(Settings(), e);
    CHECK(e.driftCorrection == Settings::kDriftAuto); // the default

    // a loudness preset of the versions before Attack / Release: "Fast down" (dB/s) becomes the Attack (s per 10 dB);
    // 0 was "the normal speed", the preset's Speed
    {
        Settings fd;
        fd.fromText("[Preset Loud]\r\nLevellerFastDown=5\r\n[Preset Old own]\r\nLevellerSpeed=0.5\r\nLevellerFastDown=0\r\n");
        CHECK_NEAR(fd.valuesOf("Loud")[pLevAttack], 2.0, 1e-9);
        CHECK_NEAR(fd.valuesOf("Old own")[pLevAttack], 20.0, 1e-9);
        Settings kept; // an Attack of its own wins over an old Fast down beside it
        kept.fromText("[Preset Loud]\r\nLevellerFastDown=5\r\nLevellerAttack=0.7\r\n");
        CHECK_NEAR(kept.valuesOf("Loud")[pLevAttack], 0.7, 1e-9);
    }

    // [Audio] DriftCorrection: 0 / 1 of the versions before three modes keep their meaning, 2 = always, anything else
    // (a hand edit) the default
    for (const auto& [value, mode] : std::vector<std::pair<std::string, int>>{
             {"0", Settings::kDriftOff}, {"1", Settings::kDriftAuto}, {"2", Settings::kDriftAlways},
             {"7", Settings::kDriftAuto}, {"-3", Settings::kDriftAuto}, {"x", Settings::kDriftAuto},
             // the values just outside (seventh outside review: bounds moved by one were not caught)
             {"3", Settings::kDriftAuto}, {"-1", Settings::kDriftAuto}}) {
        Settings dc;
        dc.fromText("[Audio]\r\nDriftCorrection=" + value + "\r\n");
        CHECK(dc.driftCorrection == mode);
    }
    for (const int mode : {Settings::kDriftOff, Settings::kDriftAuto, Settings::kDriftAlways}) { // each survives saving
        Settings dc, back;
        dc.driftCorrection = mode;
        back.fromText(dc.toText());
        CHECK(back.driftCorrection == mode);
    }

    // [Audio] InputPeriodMs: 0, 8, 6, 5, 4; anything else 10 ms (0) - 3 and 2 ms clicked through a virtual cable even
    // at its fastest timer; the former ShortInputPeriod=1 (the device's shortest period, distorted) is not taken over
    for (const auto& [line, ms] : std::vector<std::pair<std::string, int>>{
             {"InputPeriodMs=8", 8}, {"InputPeriodMs=6", 6}, {"InputPeriodMs=5", 5}, {"InputPeriodMs=4", 4},
             {"InputPeriodMs=3", 0}, {"InputPeriodMs=2", 0}, {"InputPeriodMs=7", 0}, {"InputPeriodMs=x", 0},
             {"ShortInputPeriod=1", 0}}) {
        Settings ip;
        ip.fromText("[Audio]\r\n" + line + "\r\n");
        CHECK(ip.inputPeriodMs == ms);
    }

    // linked presets: deck B follows deck A, also when a hand edit set them apart; survives a round trip
    Settings l;
    l.fromText("[Decks]\r\nPresetA=Loud\r\nPresetB=Gentle\r\nLinkPresets=1\r\n");
    CHECK(l.linkPresets && l.deck[0].preset == "Loud" && l.deck[1].preset == "Loud");
    Settings l2;
    l2.fromText(l.toText());
    checkSame(l, l2);

    // the main window's position of versions up to 0.1.132 (X / Y) becomes its place, size unknown; a bad line is
    // ignored (the window then opens centred)
    Settings w;
    w.fromText("[Window]\r\nX=200\r\nY=-30\r\nSpectrum=10,20,abc\r\nAbout=5,6,0,0,0\r\n");
    CHECK(w.windows.count("Main") && w.windows["Main"].x == 200 && w.windows["Main"].y == -30 && w.windows["Main"].w == 0);
    CHECK(!w.windows.count("Spectrum") && w.windows.count("About"));
    Settings w2;
    w2.fromText("[Window]\r\nX=-1\r\nY=-1\r\n"); // never moved
    CHECK(w2.windows.empty());

    // hand-written numbers: a decimal comma is read, "nan" is refused (the look-ahead keeps its default)
    Settings n;
    n.fromText("[Audio]\r\nLookAheadMs=2,5\r\n");
    CHECK_NEAR(n.lookAheadMs, 2.5, 1e-12);
    n.fromText("[Audio]\r\nLookAheadMs=nan\r\n");
    CHECK_NEAR(n.lookAheadMs, 1.5, 1e-12);
    // oversampling: 2, 4 or 8 (default 4); anything else the nearest of them
    CHECK(n.oversampling == 4);
    n.fromText("[Audio]\r\nOversampling=8\r\n");
    CHECK(n.oversampling == 8);
    n.fromText("[Audio]\r\nOversampling=1\r\n");
    CHECK(n.oversampling == 2);
    n.fromText("[Audio]\r\nOversampling=16\r\n");
    CHECK(n.oversampling == 8);
}

// A settings file an editor saved as UTF-16 is read (it read as empty and the next save dropped the own presets), and
// a file that is there but cannot be read is never overwritten.
void unreadable(const fs::path& dir) {
    std::printf("-- UTF-16 and unreadable settings files\n");
    std::string err;
    Settings s;
    CHECK(s.saveAs("Mine", defaultValues(), err));
    s.deck[0].preset = "Mine";
    const std::string text = s.toText();
    std::string utf16 = "\xFF\xFE"; // little endian with its BOM (the text is ASCII here)
    for (char c : text) {
        utf16 += c;
        utf16 += '\0';
    }
    const fs::path file = dir / "utf16.ini";
    {
        std::ofstream out(file, std::ios::binary);
        out.write(utf16.data(), static_cast<std::streamsize>(utf16.size()));
    }
    Settings r;
    CHECK(r.load(file));
    CHECK(r.find("Mine") != nullptr && r.deck[0].preset == "Mine");

    // a file that cannot be read but could be replaced (an ACL refusing "read data" to everyone): saveMerged refuses
    // and leaves it. (A folder of that name, the first version of this test, could not fail: a rename cannot
    // replace a folder either - outside review.) Merged against the shipped lists alone, the file lost "Mine".
#ifdef _WIN32 // Windows' ACLs (icacls); the core's other tests also build elsewhere
    const fs::path locked = dir / "locked.ini";
    std::error_code ec;
    {
        Settings mine;
        CHECK(mine.saveAs("Mine", defaultValues(), err));
        CHECK(mine.save(locked));
    }
    const std::string path = locked.string();
    CHECK(std::system(("icacls \"" + path + "\" /deny *S-1-1-0:(RD) >nul").c_str()) == 0);
    struct Restore { // the deny goes again however this block ends: no file left that nobody can read
        std::string path;
        void now() {
            if (!path.empty()) std::system(("icacls \"" + path + "\" /remove:d *S-1-1-0 >nul").c_str());
            path.clear();
        }
        ~Restore() { now(); }
    } restore{path};
    Settings probe;
    const bool unreadable = !probe.load(locked);
    Settings other; // a writer that never saw "Mine" (its base: the shipped lists)
    Settings base = other;
    other.driver = "changed";
    const bool saved = other.saveMerged(locked, base, Settings::SaveRole::Owner);
    restore.now();
    Settings after;
    CHECK(after.load(locked));
    std::printf("   unreadable file: load %s, saveMerged %s, \"Mine\" %s\n", unreadable ? "refused" : "WORKED",
                saved ? "WROTE" : "refused", after.find("Mine") ? "kept" : "LOST");
    CHECK(unreadable);
    CHECK(!saved);
    CHECK(after.find("Mine") != nullptr && after.driver.empty());
    fs::remove(locked, ec);
#endif
}

// The positions of every parameter (the GUI's sliders, the plug-in's host steps): the ends are the range (or "auto" /
// "off"), values rise with the position, and every shipped value has a position that gives it back within a step.
void positions() {
    for (int p = 0; p < kParamCount; ++p) {
        const ParamInfo& in = paramInfo(p);
        const int n = paramPositions(p);
        CHECK(n >= 1);
        if (p == pCompMakeup || p == pLevSilence) CHECK(paramValueAt(p, 0) < 0);
        else if (p == pCompSideHpf) CHECK(paramValueAt(p, 0) == 0);
        else CHECK(paramValueAt(p, 0) == in.min);
        CHECK(paramValueAt(p, n) == in.max);
        for (int k = 1; k <= n; ++k) CHECK(paramValueAt(p, k) >= paramValueAt(p, k - 1));
        for (int s = 0; s < kShippedPresetCount + kShippedColourCount; ++s) {
            const PresetValues v = s < kShippedPresetCount ? shippedPresetValues(s)
                                                           : shippedColourValues(s - kShippedPresetCount);
            const double x = v[static_cast<size_t>(p)], back = paramValueAt(p, paramPositionOf(p, x));
            // a step, or 2 % of the value on the log scales (300 positions over a factor of 8 or more)
            CHECK_NEAR(back, x, std::max(in.step * 0.5 + 1e-9, std::fabs(x) * 0.02));
        }
    }
}

// The program and the plug-in in one settings file: neither loses the other's presets or colours, the plug-in never
// writes the program's other settings, and a preset a deck uses stays.
void merged(const fs::path& dir) {
    std::printf("-- program and plug-in saving the same file\n");
    const fs::path file = dir / "shared.ini";
    std::string err;
    Settings program;
    program.driver = "Test ASIO";
    CHECK(program.saveAs("Club", defaultValues(), err));
    CHECK(program.save(file));
    Settings programBase = program;

    Settings plugin;
    CHECK(plugin.load(file));
    Settings pluginBase = plugin;
    plugin.driver = "something else"; // the plug-in's copy of a setting it does not own
    PresetValues v = defaultValues();
    v[pTarget] = -11.0;
    CHECK(plugin.saveAs("Plug", v, err));
    CHECK(plugin.saveAs("Bright plug", shippedColourValues(shippedColourIndex("Bright")), err, PresetKind::Colour));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));

    // the program, unaware, changes its own preset and deletes nothing of the plug-in's
    program.find("Club")->values[pTarget] = -12.5;
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(program.find("Plug") != nullptr);                          // the plug-in's preset came in
    CHECK(program.find("Bright plug", PresetKind::Colour) != nullptr);
    Settings check;
    CHECK(check.load(file));
    CHECK(check.driver == "Test ASIO");                              // the plug-in did not write it
    CHECK(check.find("Club") && check.find("Club")->values[pTarget] == -12.5);
    CHECK(check.find("Plug") && check.find("Plug")->values[pTarget] == -11.0);

    // the windows' places are the program's: a guest keeps them
    program.windows["Spectrum"] = WindowPlace{40, 50, 900, 600, false};
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveAs("Plug b", v, err));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(check.load(file));
    CHECK(check.windows.count("Spectrum") && check.windows["Spectrum"].w == 900);
    // the plug-in's own places ("Plugin..." names) are written by it, and the program's next save keeps them
    plugin.windows["PluginSpectrum"] = WindowPlace{10, 20, 700, 500, false};
    plugin.windows["Spectrum"] = WindowPlace{1, 1, 1, 1, false}; // stale copies of the program's are never written:
    pluginBase.windows["Spectrum"] = plugin.windows["Spectrum"]; // (unchanged since its base)
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    program.windows["Main"] = WindowPlace{5, 6, 800, 600, false};
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(check.load(file));
    CHECK(check.windows.count("PluginSpectrum") && check.windows["PluginSpectrum"].w == 700);
    CHECK(check.windows["Spectrum"].w == 900 && check.windows["Main"].w == 800);
    CHECK(program.windows.count("PluginSpectrum") == 1); // the program's copy took it over

    // the plug-in renames its preset and deletes its colour; the program meanwhile deletes Club
    CHECK(plugin.rename("Plug", "Plug 2", err));
    CHECK(plugin.remove("Bright plug", err, PresetKind::Colour));
    CHECK(program.remove("Club", err));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(check.load(file));
    CHECK(!check.find("Club"));
    CHECK(!check.find("Plug") && check.find("Plug 2"));
    CHECK(!check.find("Bright plug", PresetKind::Colour));
    CHECK(check.driver == "Test ASIO");
    CHECK(plugin.find("Club") == nullptr); // the plug-in sees the program's deletion after its save

    // a preset a deck uses stays, even when the other deleted it
    CHECK(program.saveAs("Live", defaultValues(), err));
    program.deck[0].preset = "Live";
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest)); // takes Live in
    CHECK(plugin.remove("Live", err));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest)); // deletes it from the file
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner)); // deck A uses it: back
    CHECK(check.load(file));
    CHECK(check.find("Live") != nullptr);

    // a guest with nothing new (the plug-in opening its preset window) only reads: the file stays as it was
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(fs::last_write_time(file, ec) == stamp);
    CHECK(plugin.find("Live") != nullptr);
    const fs::path none = dir / "none.ini"; // and no file is made for it
    Settings fresh, freshBase;
    CHECK(fresh.saveMerged(none, freshBase, Settings::SaveRole::Guest));
    CHECK(!fs::exists(none, ec));

    // the two switches both may change (the plug-in's Setup window): the one who changed it wins, the other keeps it
    plugin.showTooltips = false;
    plugin.driver = "not mine"; // still the program's
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(check.load(file));
    CHECK(!check.showTooltips && check.driver == "Test ASIO");
    CHECK(program.showTooltips); // the program's copy, unchanged by itself: its next save keeps the plug-in's choice
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(check.load(file));
    CHECK(!check.showTooltips);
    CHECK(!program.showTooltips); // and takes it over
    program.checkUpdates = false; // the program's own change wins at its save; the plug-in takes it at its next one
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(check.load(file));
    CHECK(!check.checkUpdates && !plugin.checkUpdates);
    // the scopes: the plug-in's choice survives the program's next save, and the program's the plug-in's
    plugin.scope[2] = {true, 5};
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    program.scope[0] = {false, 200};
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(check.load(file));
    CHECK(check.scope[2] == (ScopeChoice{true, 5}) && check.scope[0] == (ScopeChoice{false, 200}));
    CHECK(program.scope[2] == (ScopeChoice{true, 5}) && plugin.scope[0] == (ScopeChoice{false, 200}));
    // the spectra's averaging likewise (client, 2026-11-05)
    plugin.spectrumAverageMs[1] = 100;
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    program.spectrumAverageMs[0] = 1000;
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(check.load(file));
    CHECK(check.spectrumAverageMs[0] == 1000 && check.spectrumAverageMs[1] == 100);
    CHECK(program.spectrumAverageMs[1] == 100 && plugin.spectrumAverageMs[0] == 1000);
    {
        Settings odd; // a value not in the list: the default
        odd.fromText("[Window]\nSpectrumAverage=250\nPluginSpectrumAverage=2000\n");
        CHECK(odd.spectrumAverageMs[0] == 300 && odd.spectrumAverageMs[1] == 2000);
    }

    // the plug-in's choices per instance (client, 2026-10-30): read back, merged per instance, kept by the program
    PluginSlot one;
    one.preset = "Live";
    one.colour = "Warm";
    one.lookAheadMs = 3.0;
    one.oversampling = 8;
    one.mono = true;
    plugin.pluginSlots[1] = one;
    plugin.pluginLink = true; // the plug-ins' Setup window: linked
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    Settings other, otherBase; // a second plug-in instance's copy, read before the first one saved
    other = otherBase = programBase;
    PluginSlot two;
    two.colour = "Bright";
    other.pluginSlots[2] = two;
    CHECK(other.saveMerged(file, otherBase, Settings::SaveRole::Guest));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(check.load(file));
    CHECK(check.pluginSlots.size() == 2 && check.pluginSlots[1] == one && check.pluginSlots[2] == two);
    CHECK(check.pluginLink && program.pluginLink); // kept by the other plug-in's and the program's saves
    CHECK(program.pluginSlots.size() == 2);
    std::printf("   both kept each other's presets and colours; the program's settings untouched by the plug-in, "
                "except the two switches it changed; the plug-in's choices per instance merged and kept\n");
}

// A guest is one deck: its stale second deck entry (what it read from the file) never brings back a preset or colour
// the program deleted; its own deck and both of the owner's decks still keep what they use.
void guestDecks(const fs::path& dir) {
    std::printf("-- the decks a writer protects\n");
    const fs::path file = dir / "decks.ini";
    std::string err;
    Settings program;
    CHECK(program.saveAs("Club", defaultValues(), err));
    CHECK(program.saveAs("Hue", defaultValues(), err, PresetKind::Colour));
    CHECK(program.saveAs("Mine", defaultValues(), err));
    program.deck[1].preset = "Club";
    program.deck[1].colour = "Hue";
    CHECK(program.save(file));
    Settings programBase = program;

    Settings plugin;
    CHECK(plugin.load(file)); // deck[1] still names the program's Club / Hue
    CHECK(plugin.deck[1].preset == "Club" && plugin.deck[1].colour == "Hue");
    Settings pluginBase = plugin;
    plugin.deck[0].preset = "Mine"; // its own deck uses Mine

    // the program renames its deck B's preset away and deletes the colour, and deletes Mine
    CHECK(program.rename("Club", "Club2", err));
    CHECK(program.remove("Hue", err, PresetKind::Colour));
    CHECK(program.remove("Mine", err));
    program.deck[1].preset = "Club2";
    program.deck[1].colour = "Neutral";
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));

    plugin.windows["PluginX"] = WindowPlace{1, 2, 300, 200, false}; // something to write
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    Settings check;
    CHECK(check.load(file));
    CHECK(!check.find("Club") && !check.find("Hue", PresetKind::Colour)); // not brought back by deck[1]
    CHECK(check.find("Club2") != nullptr);
    CHECK(check.find("Mine") != nullptr); // deck[0] of the guest uses it: stays
    // and the program's next save does not meet them either
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(check.load(file));
    CHECK(!check.find("Club") && !check.find("Hue", PresetKind::Colour));

    // an owner still protects both of its decks
    CHECK(program.saveAs("Own B", defaultValues(), err));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest)); // takes Own B in
    program.deck[1].preset = "Own B";
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(plugin.remove("Own B", err));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest)); // deletes it from the file
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner)); // deck B uses it: back
    CHECK(check.load(file));
    CHECK(check.find("Own B") != nullptr);
}

// A save that fails (the folder is not there) leaves every change pending: the next save to a good place writes it.
void failedSave(const fs::path& dir) {
    std::printf("-- a failed save keeps the changes pending\n");
    const fs::path file = dir / "pending.ini";
    const fs::path bad = dir / "no such folder" / "pending.ini";
    Settings program;
    CHECK(program.save(file));
    Settings plugin;
    CHECK(plugin.load(file));
    Settings pluginBase = plugin;
    plugin.showTooltips = !plugin.showTooltips;
    plugin.scope[2] = {true, 5};
    plugin.spectrumAverageMs[1] = 100;
    plugin.windows["PluginSpectrum"] = WindowPlace{10, 20, 700, 500, false};
    PluginSlot slot;
    slot.preset = "Loud";
    slot.lookAheadMs = 3.0;
    plugin.pluginSlots[1] = slot;
    const bool tooltips = plugin.showTooltips;
    CHECK(!plugin.saveMerged(bad, pluginBase, Settings::SaveRole::Guest));
    std::error_code ec;
    CHECK(!fs::exists(bad.parent_path(), ec));
    CHECK(plugin.showTooltips == tooltips && pluginBase.showTooltips != tooltips); // still pending, not in base
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    Settings check;
    CHECK(check.load(file));
    CHECK(check.showTooltips == tooltips);
    CHECK(check.scope[2] == (ScopeChoice{true, 5}) && check.spectrumAverageMs[1] == 100);
    CHECK(check.windows.count("PluginSpectrum") && check.windows["PluginSpectrum"].w == 700);
    CHECK(check.pluginSlots.count(1) && check.pluginSlots[1] == slot);

    // the same for the owner, after the file is there: the change reaches the file at the next good save
    Settings programBase = program;
    program.checkUpdates = !program.checkUpdates;
    program.windows["Main"] = WindowPlace{5, 6, 800, 600, false};
    const bool updates = program.checkUpdates;
    CHECK(!program.saveMerged(bad, programBase, Settings::SaveRole::Owner));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(check.load(file));
    CHECK(check.checkUpdates == updates && check.windows.count("Main") && check.windows["Main"].w == 800);
}

} // namespace

// An own preset's reset values are the values it was saved with (client, 2026-11-12: "when an own loudness or EQ
// setting is saved, the current settings are the default for it"; they were the generic defaults): nothing differs
// from its reset value right after "Save as", a later change does and goes back to what was saved, the file keeps
// both, a preset of an older version takes its values, and the program and the plug-in merge them.
void savedValues(const fs::path& dir) {
    std::printf("-- an own preset's reset values: what it was saved with\n");
    std::string err;
    const PresetKind C = PresetKind::Colour;
    Settings s;
    PresetValues mine = shippedPresetValues(shippedPresetIndex("Medium"));
    mine[pLevRange] = 15;
    mine[pLevAttack] = 0.5;
    mine[pLevSilence] = 6;
    CHECK(s.saveAs("Medium (mine)", mine, err));
    const Preset* p = s.find("Medium (mine)");
    CHECK(p && sameValues(p->saved, p->values)); // just saved: every value is its reset value
    for (int i = 0; i < kFirstColourParam; ++i)
        CHECK(s.resetValueOf("Medium (mine)", i) == p->values[static_cast<size_t>(i)]);
    CHECK_NEAR(s.resetValueOf("Medium (mine)", pLevRange), 15, 1e-12);
    std::string text = s.toText();
    CHECK(text.find(".Saved=") == std::string::npos); // nothing to tell apart: the file is as it was
    // a later change differs from it, is written with it, and comes back by the reset
    s.find("Medium (mine)")->values[pLevRange] = 9;
    s.find("Medium (mine)")->values[pCompRatio] = 4;
    CHECK_NEAR(s.resetValueOf("Medium (mine)", pLevRange), 15, 1e-12);
    text = s.toText();
    CHECK(text.find("LevellerRange=9\nLevellerRange.Saved=15\n") != std::string::npos);
    CHECK(text.find("CompressorRatio=4\nCompressorRatio.Saved=2\n") != std::string::npos);
    CHECK(text.find("LevellerAttack.Saved") == std::string::npos); // unchanged since it was saved
    Settings back;
    back.fromText(text);
    checkSame(s, back);
    CHECK_NEAR(back.find("Medium (mine)")->values[pLevRange], 9, 1e-12);
    CHECK_NEAR(back.resetValueOf("Medium (mine)", pLevRange), 15, 1e-12);
    back.resetValue("Medium (mine)", pLevRange);
    CHECK_NEAR(back.find("Medium (mine)")->values[pLevRange], 15, 1e-12);
    CHECK_NEAR(back.find("Medium (mine)")->values[pCompRatio], 4, 1e-12); // the other change stays
    back.resetPreset("Medium (mine)");
    CHECK(sameValues(back.find("Medium (mine)")->values, back.find("Medium (mine)")->saved));
    CHECK_NEAR(back.find("Medium (mine)")->values[pCompRatio], 2, 1e-12);
    CHECK(back.toText().find(".Saved=") == std::string::npos);
    // saved over (Save as with its own name): the values of now are its reset values
    CHECK(s.saveAs("Medium (mine)", s.find("Medium (mine)")->values, err));
    CHECK_NEAR(s.resetValueOf("Medium (mine)", pLevRange), 9, 1e-12);
    CHECK(s.toText().find(".Saved=") == std::string::npos);
    // renamed: it keeps them
    s.find("Medium (mine)")->values[pLevRange] = 11;
    CHECK(s.rename("Medium (mine)", "Mine", err));
    CHECK_NEAR(s.resetValueOf("Mine", pLevRange), 9, 1e-12);

    // the band count comes back with the splits it was saved with: 5 bands with a split of its own, changed to 3
    PresetValues five = defaultValues();
    five[pDensBands] = 5;
    loadDefaultSplits(five, 5);
    five[pDensSplit3] = 3000; // (the default is 2500)
    CHECK(s.saveAs("Five", five, err));
    Preset* f = s.find("Five");
    f->values[pDensBands] = 3;
    loadDefaultSplits(f->values, 3);
    Settings three;
    three.fromText(s.toText());
    const Preset* g = three.find("Five");
    CHECK(g != nullptr);
    if (g) {
        CHECK_NEAR(g->values[pDensBands], 3, 1e-12);
        CHECK_NEAR(g->saved[pDensBands], 5, 1e-12);
        for (int k = pDensSplit1; k <= pDensSplit4; ++k) // the four splits five bands use
            CHECK_NEAR(g->saved[static_cast<size_t>(k)], five[static_cast<size_t>(k)], 1e-9);
        three.resetValue("Five", pDensBands);
        CHECK_NEAR(g->values[pDensBands], 5, 1e-12);
        CHECK_NEAR(g->values[pDensSplit3], 3000, 1e-9);
        for (int k = pDensSplit1; k <= pDensSplit4; ++k)
            CHECK_NEAR(g->values[static_cast<size_t>(k)], five[static_cast<size_t>(k)], 1e-9);
    }

    // an EQ of one's own: the same
    PresetValues eq = shippedColourValues(shippedColourIndex("Warm"));
    CHECK(s.saveAs("My EQ", eq, err, C));
    s.find("My EQ", C)->values[pToneBass] = -3;
    Settings eqBack;
    eqBack.fromText(s.toText());
    CHECK_NEAR(eqBack.find("My EQ", C)->values[pToneBass], -3, 1e-12);
    CHECK_NEAR(eqBack.resetValueOf("My EQ", pToneBass), eq[pToneBass], 1e-12);
    eqBack.resetPreset("My EQ", C);
    CHECK(sameValues(eqBack.find("My EQ", C)->values, eqBack.find("My EQ", C)->saved));
    CHECK_NEAR(eqBack.find("My EQ", C)->values[pToneBass], eq[pToneBass], 1e-12);

    // a preset of a version before this (no .Saved lines; the generic defaults were its reset values): what it holds
    Settings old;
    old.fromText("[Preset Old one]\nTarget=-11\nLevellerRange=15\nCompressorRatio=3\n[Colour Old EQ]\nBass=4\n");
    CHECK(old.find("Old one") && old.find("Old EQ", C));
    CHECK_NEAR(old.resetValueOf("Old one", pLevRange), 15, 1e-12);
    CHECK_NEAR(old.resetValueOf("Old one", pTarget), -11, 1e-12);
    CHECK_NEAR(old.resetValueOf("Old EQ", pToneBass), 4, 1e-12);
    CHECK(sameValues(old.find("Old one")->saved, old.find("Old one")->values));
    // a shipped preset keeps its shipped values as reset values, whatever the file says
    Settings ship;
    ship.fromText("[Preset Loud]\nTarget=-9\nTarget.Saved=-3\n");
    CHECK_NEAR(ship.find("Loud")->values[pTarget], -9, 1e-12);
    CHECK_NEAR(ship.resetValueOf("Loud", pTarget), shippedPresetValues(shippedPresetIndex("Loud"))[pTarget], 1e-12);

    // the program and the plug-in: a preset saved over in the program (its values the same, its reset values new)
    // reaches the plug-in, and the plug-in's own save does not bring the old reset values back
    const fs::path file = dir / "saved.ini";
    Settings program;
    PresetValues club = defaultValues();
    club[pTarget] = -12;
    CHECK(program.saveAs("Club", club, err));
    program.find("Club")->values[pLevRange] = 8; // changed after it was saved: differs from its reset value
    CHECK(program.save(file));
    Settings programBase = program;
    Settings plugin;
    CHECK(plugin.load(file));
    Settings pluginBase = plugin;
    CHECK_NEAR(plugin.resetValueOf("Club", pLevRange), paramInfo(pLevRange).def, 1e-12);
    CHECK(program.saveAs("Club", program.find("Club")->values, err)); // saved over: 8 dB is its reset value now
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    plugin.windows["PluginSpectrum"] = WindowPlace{1, 2, 300, 200, false}; // something of its own to save
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK_NEAR(plugin.resetValueOf("Club", pLevRange), 8, 1e-12); // taken over from the file
    Settings after;
    CHECK(after.load(file));
    CHECK_NEAR(after.find("Club")->values[pLevRange], 8, 1e-12);
    CHECK_NEAR(after.resetValueOf("Club", pLevRange), 8, 1e-12);
    // ... and a change in the plug-in (its window's slider) keeps the reset value the program gave it
    plugin.find("Club")->values[pLevRange] = 5;
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK_NEAR(program.find("Club")->values[pLevRange], 5, 1e-12);
    CHECK_NEAR(program.resetValueOf("Club", pLevRange), 8, 1e-12);
}

// The sounds (client, 2026-11-13): a loudness preset and an EQ under one name - twelve shipped, own ones made, changed,
// renamed, deleted and reset; what a deck plays follows its sound; a settings file of a version before the sounds;
// the program and the plug-in saving them in the same file.
void sounds(const fs::path& dir) {
    std::printf("-- sounds: a loudness preset and an EQ under one name\n");
    Settings s;
    CHECK(static_cast<int>(s.sounds.size()) == Settings::shippedSoundCount() && s.sounds.size() == 12);
    for (const Sound& x : s.sounds) {
        CHECK(x.shipped && s.find(x.loudness) && s.find(x.eq, PresetKind::Colour));
        CHECK(Settings::shippedSoundIndex(x.name) >= 0);
    }
    CHECK(s.deck[0].sound == "Lelystad" && s.deck[0].preset == "Medium" && s.deck[0].colour == "Neutral");
    CHECK(s.findSound("Zwolle")->loudness == "Medium" && s.findSound("Zwolle")->eq == "Auto"); // (client, 2026-11-13)
    CHECK(s.findSound("Utrecht")->loudness == "Medium" && s.findSound("Utrecht")->eq == "Auto");
    CHECK(s.findSound("Groningen")->loudness == "Very Loud" && s.findSound("Groningen")->eq == "Bass+");
    {   // two sounds with the same two parts: a deck keeps the one it chose
        Settings same, back2;
        CHECK(same.chooseSound(0, "Zwolle"));
        back2.fromText(same.toText());
        CHECK(back2.deck[0].sound == "Zwolle" && back2.deck[0].preset == "Medium" && back2.deck[0].colour == "Auto");
    }
    // as shipped, no sound is written
    CHECK(s.toText().find("\n[Sound ") == std::string::npos);

    // a deck takes a sound; a sound's part changes and its decks follow; reset
    CHECK(s.chooseSound(0, "Zwolle") && s.deck[0].preset == "Medium" && s.deck[0].colour == "Auto");
    CHECK(!s.chooseSound(1, "Amsterdam") && s.deck[1].sound == "Lelystad");
    CHECK(s.chooseSound(1, "Zwolle"));
    s.setSoundPart("Zwolle", PresetKind::Colour, "Warm");
    CHECK(s.deck[0].colour == "Warm" && s.deck[1].colour == "Warm" && s.findSound("Zwolle")->eq == "Warm");
    s.setSoundPart("Zwolle", PresetKind::Colour, "No such EQ"); // refused
    CHECK(s.findSound("Zwolle")->eq == "Warm");
    s.setDeckPart(0, PresetKind::Dynamics, "Very Loud"); // the deck's sound changes, and the other deck with it
    CHECK(s.findSound("Zwolle")->loudness == "Very Loud" && s.deck[1].preset == "Very Loud");
    std::string text = s.toText();
    CHECK(text.find("[Sound Zwolle]") != std::string::npos && text.find("SoundA=Zwolle") != std::string::npos);
    Settings back;
    back.fromText(text);
    CHECK(back.findSound("Zwolle")->eq == "Warm" && back.findSound("Zwolle")->loudness == "Very Loud");
    CHECK(back.deck[0].sound == "Zwolle" && back.deck[0].colour == "Warm" && back.deck[1].sound == "Zwolle");
    CHECK(back.toText() == text);
    s.resetSound("Zwolle");
    CHECK(s.findSound("Zwolle")->eq == "Auto" && s.findSound("Zwolle")->loudness == "Medium" && s.deck[0].colour == "Auto");
    CHECK(s.toText().find("[Sound Zwolle]") == std::string::npos);

    // own sounds: save as, saved over, rename, delete; a shipped name is refused
    std::string err;
    CHECK(!s.saveSoundAs("Utrecht", "Loud", "Warm", err) && !err.empty());
    CHECK(!s.saveSoundAs("bad=name", "Loud", "Warm", err));
    CHECK(s.saveSoundAs("Feest", "Loud", "Warm", err) && !s.findSound("Feest")->shipped);
    CHECK(s.chooseSound(0, "Feest") && s.deck[0].preset == "Loud" && s.deck[0].colour == "Warm");
    s.setSoundPart("Feest", PresetKind::Dynamics, "Light");
    std::string l, e;
    s.soundResetParts("Feest", l, e);
    CHECK(l == "Loud" && e == "Warm"); // an own sound resets to what it was saved with
    text = s.toText();
    CHECK(text.find("Loudness.Saved=Loud") != std::string::npos);
    back.fromText(text);
    back.soundResetParts("Feest", l, e);
    CHECK(back.findSound("Feest")->loudness == "Light" && l == "Loud" && e == "Warm");
    CHECK(s.saveSoundAs("Feest", "Gentle", "Quiet", err) && s.deck[0].preset == "Gentle"); // saved over: its decks follow
    CHECK(!s.renameSound("Zwolle", "Kampen", err)); // shipped
    CHECK(!s.renameSound("Feest", "Utrecht", err)); // taken
    CHECK(s.renameSound("Feest", "Party", err) && s.deck[0].sound == "Party");
    s.pluginSlots[1].sound = "Party";
    CHECK(s.renameSound("Party", "Fest", err) && s.pluginSlots[1].sound == "Fest");
    CHECK(!s.removeSound("Groningen", err));
    CHECK(s.removeSound("Fest", err) && !s.findSound("Fest"));
    CHECK(s.deck[0].sound == "Lelystad" && s.deck[0].preset == "Medium" && s.deck[0].colour == "Neutral");

    // a sound follows its presets: one renamed, one deleted
    CHECK(s.saveAs("Club", defaultValues(), err));
    CHECK(s.saveSoundAs("Nacht", "Club", "Bright", err) && s.chooseSound(1, "Nacht"));
    CHECK(s.rename("Club", "Club 2", err) && s.findSound("Nacht")->loudness == "Club 2" && s.deck[1].preset == "Club 2");
    s.findSound("Nacht")->savedLoudness = "Club 2";
    CHECK(s.remove("Club 2", err) && s.findSound("Nacht")->loudness == "Medium" &&
          s.findSound("Nacht")->savedLoudness == "Medium");

    // a deck's parts set apart: no sound, and the file says so; a file of a version before the sounds
    Settings apart;
    apart.deck[0].preset = "Gentle";
    apart.deck[0].colour = "Bright";
    text = apart.toText();
    CHECK(text.find("SoundA=\n") != std::string::npos);
    back.fromText(text);
    CHECK(back.deck[0].sound.empty() && back.deck[0].preset == "Gentle" && back.deck[0].colour == "Bright");
    back.setDeckPart(0, PresetKind::Colour, "Warm"); // now Den Haag's two parts
    CHECK(back.deck[0].sound == "Den Haag");
    back.fromText("[Decks]\nPresetA=Loud\nColourA=Warm\nPresetB=Gentle\nColourB=Bright\n"
                  "[Plugin]\nLoudness1=Medium\nColour1=Auto\nLoudness2=Light\nColour2=Bright\n");
    CHECK(back.deck[0].sound == "'s-Hertogenbosch" && back.deck[1].sound.empty() && back.deck[1].preset == "Gentle");
    CHECK(back.pluginSlots[1].sound == "Utrecht" && back.pluginSlots[2].sound.empty());
    back.fromText("[Decks]\nSoundA=Zwolle\nPresetA=Gentle\nColourA=Warm\n"); // the file's sound does not hold them
    CHECK(back.deck[0].sound == "Den Haag" && back.deck[0].preset == "Gentle");

    // the program and the plug-in: each keeps the other's sounds and changes
    const fs::path file = dir / "sounds.ini";
    std::error_code ec;
    fs::remove(file, ec);
    Settings program, programBase, plugin, pluginBase;
    CHECK(program.saveSoundAs("Mijn", "Loud", "Auto", err) && program.chooseSound(0, "Mijn"));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(plugin.load(file) && pluginBase.load(file) && plugin.findSound("Mijn"));
    plugin.setSoundPart("Utrecht", PresetKind::Colour, "Vocal");
    CHECK(plugin.saveSoundAs("Plug", "Light", "Warm", err));
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    program.setSoundPart("Mijn", PresetKind::Dynamics, "Gentle");
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    Settings check;
    CHECK(check.load(file));
    CHECK(check.findSound("Plug") && check.findSound("Utrecht")->eq == "Vocal" && check.findSound("Mijn")->loudness == "Gentle");
    CHECK(check.deck[0].sound == "Mijn" && check.deck[0].preset == "Gentle");
    // the plug-in deletes the program's sound meanwhile: the program's deck A uses it, so it stays
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest)); // takes the program's change in
    CHECK(plugin.removeSound("Mijn", err) && plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(check.load(file) && check.findSound("Mijn"));
    // a sound the plug-in changed: the program's deck playing it follows at its next save
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    plugin.setSoundPart("Mijn", PresetKind::Colour, "Bright");
    CHECK(plugin.saveMerged(file, pluginBase, Settings::SaveRole::Guest));
    CHECK(program.saveMerged(file, programBase, Settings::SaveRole::Owner));
    CHECK(program.deck[0].sound == "Mijn" && program.deck[0].colour == "Bright");
    std::printf("   twelve shipped; chosen, changed, reset, own ones made, renamed and deleted; old files; merged\n");
}

int main() {
    const fs::path dir = tempDir();
    defaults();
    sounds(dir);
    savedValues(dir);
    positions();
    roundTrip(dir);
    presetRules();
    handEdited();
    merged(dir);
    guestDecks(dir);
    failedSave(dir);
    unreadable(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return test::finish("settings_test");
}
