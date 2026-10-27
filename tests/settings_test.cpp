// Settings and presets: ini round trip, shipped presets stored as differences, rename / delete / reset rules,
// hand-edited and damaged files (docs/design.md 3.1, 8).
#include "check.h"
#include "settings.h"

#include <algorithm>
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
    CHECK(a.bufferSize == b.bufferSize && a.startAudio == b.startAudio);
    CHECK(a.api == b.api && a.exclusive == b.exclusive);
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
    }
}

fs::path tempDir() {
    const fs::path d = fs::temp_directory_path() / "DJAP_settings_test";
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
    s.bufferSize = 192;
    s.startAudio = true;
    s.followVolume[0] = false; // both the other way round than the defaults
    s.followVolume[1] = true;
    s.api = "WASAPI";
    s.exclusive = true;
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
    s.resetPreset("Own");
    CHECK(sameValues(s.find("Own")->values, defaultValues()));
    CHECK_NEAR(Settings::resetValueOf("Gentle", pLevRange), 12, 1e-12);
    CHECK_NEAR(Settings::resetValueOf("Own", pLevRange), paramInfo(pLevRange).def, 1e-12);

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
    s.resetPreset("Club kleur", C);
    CHECK(sameValues(s.find("Club kleur", C)->values, defaultValues()));
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
    const fs::path locked = dir / "locked.ini";
    std::error_code ec;
    {
        Settings mine;
        CHECK(mine.saveAs("Mine", defaultValues(), err));
        CHECK(mine.save(locked));
    }
    const std::string path = locked.string();
    CHECK(std::system(("icacls \"" + path + "\" /deny *S-1-1-0:(RD) >nul").c_str()) == 0);
    Settings probe;
    const bool unreadable = !probe.load(locked);
    Settings other; // a writer that never saw "Mine" (its base: the shipped lists)
    Settings base = other;
    other.driver = "changed";
    const bool saved = other.saveMerged(locked, base, Settings::SaveRole::Owner);
    std::system(("icacls \"" + path + "\" /remove:d *S-1-1-0 >nul").c_str());
    Settings after;
    CHECK(after.load(locked));
    std::printf("   unreadable file: load %s, saveMerged %s, \"Mine\" %s\n", unreadable ? "refused" : "WORKED",
                saved ? "WROTE" : "refused", after.find("Mine") ? "kept" : "LOST");
    CHECK(unreadable);
    CHECK(!saved);
    CHECK(after.find("Mine") != nullptr && after.driver.empty());
    fs::remove(locked, ec);
}

// The positions of every parameter (the GUI's sliders, the plug-in's host steps): the ends are the range (or "auto" /
// "off"), values rise with the position, and every shipped value has a position that gives it back within a step.
void positions() {
    for (int p = 0; p < kParamCount; ++p) {
        const ParamInfo& in = paramInfo(p);
        const int n = paramPositions(p);
        CHECK(n >= 1);
        if (p == pCompMakeup) CHECK(paramValueAt(p, 0) < 0);
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
    std::printf("   both kept each other's presets and colours; the program's settings untouched by the plug-in\n");
}

} // namespace

int main() {
    const fs::path dir = tempDir();
    defaults();
    positions();
    roundTrip(dir);
    presetRules();
    handEdited();
    merged(dir);
    unreadable(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return test::finish("settings_test");
}
