// Program settings and presets, stored in a readable UTF-8 ini file (one comment line per value).
#pragma once

#include "params.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ap {

struct Preset {
    std::string name;
    PresetValues values{};
    bool shipped = false;
};

struct DeckSettings {
    std::string preset = "Medium";
    std::string colour = "Neutral"; // colour preset (tone, auto tone, sub bass, loudness)
    bool processing = true;
};

// The two kinds of preset: dynamics (loudness, compressor, ...; the colour values unused) and colour (only the
// colour values used, docs/design.md 3.3). Both hold a whole PresetValues array.
enum class PresetKind { Dynamics, Colour };
inline PresetKind kindOf(int param) { return isColourParam(param) ? PresetKind::Colour : PresetKind::Dynamics; }

class Settings {
public:
    Settings(); // shipped presets, default audio settings

    // Audio
    int decks = 2;               // 2 = two decks (DJ: deck A and deck B); 1 = one channel (deck A only)
    std::string api = "ASIO";    // ASIO, WASAPI, DirectSound or MME
    std::string driver;          // ASIO driver name; empty = none chosen
    std::string inDevice[2], outDevice[2]; // WASAPI / DirectSound / MME: each deck's devices (empty = deck unused)
    bool exclusive = false;      // WASAPI exclusive mode
    int sampleRate = 48000;      // requested rate
    int inputA = 1, inputB = 3;  // first channel (1-based) of each deck's input pair
    int outputA = 1, outputB = 3;
    std::string asioInputs = "ASIO"; // ASIO: the decks' inputs from the pairs (ASIO) or from WASAPI / DirectSound /
                                     // MME devices (drift-corrected, docs/design.md section 6.1)
    std::string asioInputDevice[2];  // ... each deck's device then; empty = its ASIO input pair
    double lookAheadMs = 1.5;    // limiter look-ahead, both decks
    int bufferSize = 0;          // driver buffer in frames; 0 = the driver's preferred size
    bool startAudio = false;     // start the audio at program start (the last Start, not stopped since)
    // the outputs follow the volume (and mute) of Windows' default playback device - the volume keys; one choice
    // per mode, [0] one channel (on by default), [1] two decks (off: a DJ's outputs do not move with a key)
    bool followVolume[2] = {true, false};
    bool followsVolume() const { return followVolume[decks == 1 ? 0 : 1]; }
    DeckSettings deck[2];
    bool linkPresets = false;    // deck B always uses deck A's preset (choosing one on either deck sets both)
    int windowX = -1, windowY = -1;
    bool trayHintShown = false;  // the "still running in the notification area" balloon was shown once
    bool checkUpdates = true;    // ask GitHub once a day for a newer version (Setup window)
    std::string updateUrl;       // where to ask instead of the public repository (tests); empty = GitHub
    // An audio device was chosen in the Audio window (an ASIO driver, or a device of another API for a deck);
    // false at the first start: the program then opens the Audio window.
    bool audioConfigured() const;

    std::vector<Preset> presets; // from the quietest output to the loudest (sortPresets), own ones among them
    std::vector<Preset> colours; // colour presets: shipped first, then the user's in the order made
    void sortPresets();          // after loading and when a preset is added

    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;
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
    static double resetValueOf(const std::string& presetName, int param);
    static PresetValues shippedValues(const std::string& name, PresetKind k); // or the generic defaults
};

} // namespace ap
