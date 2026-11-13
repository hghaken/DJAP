// SPDX-License-Identifier: GPL-3.0-or-later
// Program settings and presets, stored in a readable UTF-8 ini file (one comment line per value).
#pragma once

#include "params.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ap {

struct Preset {
    std::string name;
    PresetValues values{};
    // An own preset's reset values: its values when it was last saved with "Save as" (client, 2026-11-12: "when an
    // own loudness or EQ setting is saved, the current settings are the default for it" - they were the generic
    // defaults, so a preset just saved showed every value it had changed in bold, with a reset button that took it
    // away from what was saved). A shipped preset resets to its shipped values (Settings::resetValues); this is then
    // a copy of those.
    PresetValues saved{};
    bool shipped = false;
};

// A sound: a loudness preset and an EQ together under one name - what a deck (and a plug-in) chooses (client,
// 2026-11-13: "combine the Loudness and the EQ into one setting named after a capital in the Netherlands ... stored
// on disk, changed by the user and saved again ... chosen in the main window with a drop-down; an Edit button where
// the Loudness and the EQ are set apart"; then "make 12 sets, the 12 provincial capitals"). The twelve shipped ones
// carry those names; the user's own follow. Its two parts are chosen in the Sound window; a change is saved at once,
// as a preset's values, and every deck using the sound follows.
struct Sound {
    std::string name;
    std::string loudness = "Medium", eq = "Neutral";
    // what "reset" goes back to: a shipped sound's shipped parts, an own sound's parts as it was saved (Save as)
    std::string savedLoudness = "Medium", savedEq = "Neutral";
    bool shipped = false;
    bool operator==(const Sound& o) const {
        return name == o.name && loudness == o.loudness && eq == o.eq && savedLoudness == o.savedLoudness &&
               savedEq == o.savedEq && shipped == o.shipped;
    }
};

struct DeckSettings {
    // Its sound: Medium with Neutral, the two below. What the deck plays is `preset` and `colour`; the sound is the
    // one that holds them (Settings::chooseSound, setDeckPart) - empty when they were chosen apart and no sound has
    // them (a settings file of a version before the sounds).
    std::string sound = "Lelystad";
    std::string preset = "Medium";
    std::string colour = "Neutral"; // colour preset (tone, auto tone, sub bass, loudness)
    bool processing = true;
};

// The two kinds of preset: dynamics (loudness, compressor, ...; the colour values unused) and colour (only the
// colour values used, docs/design.md 3.3). Both hold a whole PresetValues array.
enum class PresetKind { Dynamics, Colour };
inline PresetKind kindOf(int param) { return isColourParam(param) ? PresetKind::Colour : PresetKind::Dynamics; }

// Where a window was last (placement.h): its normal - not maximised - rectangle in screen pixels (width and height 0:
// not known, the window keeps its own size), and whether it was maximised.
struct WindowPlace {
    int x = 0, y = 0, w = 0, h = 0;
    bool maximised = false;
    bool operator==(const WindowPlace& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h && maximised == o.maximised;
    }
};

// What one plug-in instance last chose (client, 2026-10-30: "the loudness and colour preset are not kept after a
// restart of the DJ program"): that program keeps no plug-in state - at every start it sets each parameter back to
// its default - so the plug-in keeps its own choices here, per instance (Settings::pluginSlots).
struct PluginSlot {
    std::string sound = "Lelystad"; // its sound; empty: the two below match none (values set by the host)
    std::string preset = "Medium", colour = "Neutral";
    double lookAheadMs = 1.5;
    int oversampling = 4;
    bool mono = false;
    bool operator==(const PluginSlot& o) const {
        return sound == o.sound && preset == o.preset && colour == o.colour && lookAheadMs == o.lookAheadMs &&
               oversampling == o.oversampling && mono == o.mono;
    }
};

// The analysis window's scope (client, 2026-11-05: "save this value"): the input or the output, and the time across it.
struct ScopeChoice {
    bool input = false;
    int ms = 50; // 1, 5, 10, 20, 50, 200 or 2000
    bool operator==(const ScopeChoice& o) const { return input == o.input && ms == o.ms; }
};

class Settings {
public:
    Settings(); // shipped presets, default audio settings

    // Audio
    int decks = 2;               // 2 = two decks (DJ: deck A and deck B); 1 = one channel (deck A only)
    std::string api = "ASIO";    // ASIO, WASAPI, DirectSound or MME
    std::string driver;          // ASIO driver name; empty = none chosen
    std::string inDevice[2], outDevice[2]; // WASAPI / DirectSound / MME: each deck's devices (empty = deck unused)
    bool exclusive = false;      // WASAPI exclusive mode
    // ASIO with WASAPI deck inputs: the inputs' period in ms - 0 = Windows' 10 ms default, or 8, 6, 5 or 4 ms through
    // Windows' low-latency shared mode when the device has it (seventh outside review: the 10 ms are the largest part
    // of the latency). A choice, not "the shortest": a virtual cable allowed 1 ms and the sound came out distorted.
    // Measured 2026-10-28 (a 1 kHz tone through the cable, 15-30 s per period): with the cable's own timer at 3 ms
    // clean from 8 ms, at 1 ms clean from 4 ms; 2-3 ms clicked at either - the period must be 3-4 timer ticks.
    int inputPeriodMs = 0;
    // WASAPI / DirectSound / MME: each deck's input read with drift correction (a capture and a playback stream with
    // the deck's AsyncInput between them: the input and output device may run on different clocks; adds a buffer of
    // about 8-23 ms). kDriftAuto: so unless WASAPI finds input and output on one device (one clock: one duplex
    // stream); kDriftAlways: also then (a device that holds two clocks after all); kDriftOff: always one duplex
    // stream, no added buffer, but devices on different clocks drift apart (a glitch now and then).
    static constexpr int kDriftOff = 0, kDriftAuto = 1, kDriftAlways = 2;
    int driftCorrection = kDriftAuto;
    int sampleRate = 48000;      // requested rate
    int inputA = 1, inputB = 3;  // first channel (1-based) of each deck's input pair
    int outputA = 1, outputB = 3;
    std::string asioInputs = "ASIO"; // ASIO: the decks' inputs from the pairs (ASIO) or from WASAPI / DirectSound /
                                     // MME devices (drift-corrected, docs/design.md section 6.1)
    std::string asioInputDevice[2];  // ... each deck's device then; empty = its ASIO input pair
    double lookAheadMs = 1.5;    // limiter look-ahead, both decks
    int oversampling = 4;        // 2, 4 or 8: the soft clipper and the exciter (deck.h, kOversamplingDefault)
    bool mono = false;           // both decks' outputs mono: (L + R) / 2 (Setup window; DeckControl::mono)
    int bufferSize = 0;          // driver buffer in frames; 0 = the driver's preferred size
    bool startAudio = false;     // start the audio at program start (the last Start, not stopped since)
    // WASAPI / DirectSound / MME: a device that goes away is tried again for retryMinutes (1, 2, 5, 10 or 30), then the
    // output plays on fallbackOutput (empty: none, go on trying) until the chosen one is back (recovery.h)
    std::string fallbackOutput;
    int retryMinutes = 5;
    // the outputs follow the volume (and mute) of Windows' default playback device - the volume keys; one choice
    // per mode, [0] one channel (on by default), [1] two decks (off: a DJ's outputs do not move with a key)
    bool followVolume[2] = {true, false};
    bool followsVolume() const { return followVolume[decks == 1 ? 0 : 1]; }
    DeckSettings deck[2];
    bool linkPresets = false;    // deck B always uses deck A's preset (choosing one on either deck sets both)
    // every window's last place, by its name (Main, Setup, Loudness, Colour, Analysis, Spectrum, About); a window
    // without one opens centred on the screen (client, 2026-10-26)
    std::map<std::string, WindowPlace> windows;
    bool snapToBands = false; // the spectrum window: a dragged colour point snaps to the density bands' splits
    // the analysis window's scope: deck A, deck B (the program), the plug-in's (every instance); merged three-way
    ScopeChoice scope[3];
    // the spectra's averaging in ms (the spectrum window's list; client, 2026-11-05): [0] the program's decks, [1] the
    // plug-in (every instance); one of kSpectrumAverages, merged three-way like the scopes
    static constexpr int kSpectrumAverages[] = {0, 100, 200, 300, 500, 1000, 2000};
    int spectrumAverageMs[2] = {300, 300};
    bool latencyProbe = true; // the Health window's Windows latency probe (latencyprobe.h) runs
    bool showTooltips = true; // the explanations when the mouse rests on a control (all windows, the plug-in too)
    bool trayHintShown = false;  // the "still running in the notification area" balloon was shown once
    bool checkUpdates = true;    // ask GitHub once a day for a newer version (Setup window)
    std::string updateUrl;       // where to ask instead of the public repository (tests); empty = GitHub
    // the plug-in's choices by instance number: 1 = the first instance open in a host (deck 1 in a DJ program), 2 =
    // the second, ...; written by the plug-in only, kept by the program
    std::map<int, PluginSlot> pluginSlots;
    // the plug-ins in one DJ program keep the same settings (client, 2026-10-30: "a check box in the Setup window that
    // keeps all audio settings of deck A and deck B the same - deck A leading - as in the program"): instance 1 leads
    bool pluginLink = false;
    // An audio device was chosen in the Audio window (an ASIO driver, or a device of another API for a deck);
    // false at the first start: the program then opens the Audio window.
    bool audioConfigured() const;

    std::vector<Preset> presets; // from the quietest output to the loudest (sortPresets), own ones among them
    std::vector<Preset> colours; // colour presets: shipped first, then the user's in the order made
    void sortPresets();          // after loading and when a preset is added
    // sounds: the twelve shipped ones (from the quietest loudness to the loudest), then the user's in the order made
    std::vector<Sound> sounds;
    static constexpr const char* kDefaultSound = "Lelystad"; // Medium with Neutral: the decks' defaults
    Sound* findSound(const std::string& name);
    const Sound* findSound(const std::string& name) const;
    // The first sound with these two parts (shipped ones first), or null.
    const Sound* soundWith(const std::string& loudness, const std::string& eq) const;
    // Deck d takes the sound (its loudness preset and EQ); false when there is no such sound.
    bool chooseSound(int d, const std::string& name);
    // A sound's loudness preset (Dynamics) or EQ (Colour) is now `preset`: every deck using the sound follows.
    void setSoundPart(const std::string& sound, PresetKind k, const std::string& preset);
    // Deck d's loudness preset or EQ is now `preset` (a list in the spectrum window, a preset saved under a new
    // name): its sound changes with it - or, for a deck without a sound, the deck alone.
    void setDeckPart(int d, PresetKind k, const std::string& preset);
    // Sound management, as the presets'. Save as: a new own sound with these parts (or an own one saved over).
    bool saveSoundAs(const std::string& name, const std::string& loudness, const std::string& eq, std::string& error);
    bool renameSound(const std::string& from, const std::string& to, std::string& error); // decks and slots follow
    bool removeSound(const std::string& name, std::string& error); // its decks keep their parts, on the default sound
    void resetSound(const std::string& name);                        // back to its reset parts; its decks follow
    // The parts it resets to (shipped: shipped; own: as saved).
    void soundResetParts(const std::string& name, std::string& loudness, std::string& eq) const;
    // A shipped sound's name by its index (0 .. count - 1), and its shipped parts; -1 / false for another name.
    static int shippedSoundCount();
    static int shippedSoundIndex(const std::string& name);
    static bool shippedSoundParts(const std::string& name, std::string& loudness, std::string& eq);

    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;

    // The program and the plug-in share one settings file (client, 2026-10-26): each saves without losing the
    // other's presets and colours. The lists in the file as it is now are taken, and the changes this object made
    // since `base` (its lists as last read or written) are applied to them: presets it added or changed are written,
    // presets it deleted (or renamed away) are removed, everything else in the file stays. The program (Owner) writes
    // its other settings; the plug-in (Guest) keeps the file's - except the two switches both can change (Show
    // tooltips, the daily update check: the plug-in's Setup window), merged three-way like the lists, the windows'
    // places, merged per window (the plug-in keeps its own under "Plugin..." names), and the plug-in's choices, merged
    // per instance. Afterwards this object holds the
    // merged lists (the other's new presets included), those switches and places, and `base` is set to them. False
    // when the file could not be written.
    enum class SaveRole { Owner, Guest };
    bool saveMerged(const std::filesystem::path& file, Settings& base, SaveRole role);
    std::string toText() const;
    void fromText(const std::string& text);

    std::vector<Preset>& list(PresetKind k) { return k == PresetKind::Colour ? colours : presets; }
    const std::vector<Preset>& list(PresetKind k) const { return k == PresetKind::Colour ? colours : presets; }
    Preset* find(const std::string& name, PresetKind k = PresetKind::Dynamics);
    const Preset* find(const std::string& name, PresetKind k = PresetKind::Dynamics) const;
    // Values of a preset (Medium's when the name is unknown) / of a colour preset (Neutral's).
    PresetValues valuesOf(const std::string& name) const;
    PresetValues colourValuesOf(const std::string& name) const;
    // What deck d runs: its preset with its colour.
    PresetValues deckValues(int d) const { return withColour(valuesOf(deck[d].preset), colourValuesOf(deck[d].colour)); }

    // Preset management (either kind). Each returns false with a reason in `error` when it cannot be done.
    bool saveAs(const std::string& name, const PresetValues& values, std::string& error,
                PresetKind k = PresetKind::Dynamics);
    bool rename(const std::string& from, const std::string& to, std::string& error,
                PresetKind k = PresetKind::Dynamics);
    bool remove(const std::string& name, std::string& error, PresetKind k = PresetKind::Dynamics);
    // shipped: shipped values; own: generic defaults
    void resetPreset(const std::string& name, PresetKind k = PresetKind::Dynamics);
    void resetValue(const std::string& name, int param); // the same, one value (the kind follows the parameter)
    // What "reset" goes back to: a shipped preset's shipped values, an own preset's values as it was saved
    // (Preset::saved); the generic defaults for a name that is not in the list.
    PresetValues resetValues(const std::string& name, PresetKind k = PresetKind::Dynamics) const;
    double resetValueOf(const std::string& presetName, int param) const;
    static PresetValues shippedValues(const std::string& name, PresetKind k); // or the generic defaults
};

} // namespace ap
