// SPDX-License-Identifier: GPL-3.0-or-later
// DJAPCmd: the command-line program (docs/design.md section 8). Renders WAV files through the real deck
// processors, lists the presets, measures files (EBU R 128 / BS.1770) and lists the ASIO drivers. Never plays audio.
#include "appstate.h"
#include "colour.h"
#include "mediafile.h"
#include "render.h"
#include "settings.h"
#include "sysaudio.h"
#include "truepeak.h"
#include "version.h"
#include "wavfile.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <vector>

#if AP_HAVE_ASIO
#include "asiohost.h"
#endif

using namespace ap;
namespace fs = std::filesystem;

namespace {

const char* const kUsage =
    "DJAPCmd " AP_VERSION_STRING " - DJ Audio Processor, two-deck loudness and dynamics, command line\n"
    "\n"
    "  --render <in.wav> <out.wav> [options]\n"
    "        Processes an audio file offline through the same code as the live program (WAV; MP3, M4A, WMA, FLAC\n"
    "        through Windows Media Foundation; mono is played on both channels). The output is always WAV.\n"
    "        2 channels = deck A; 4 channels = deck A (1-2) and deck B (3-4).\n"
    "        --a <preset|off>   loudness preset of deck A, or off (unprocessed); default: the settings' choice\n"
    "        --b <preset|off>   the same for deck B (4-channel files)\n"
    "        --ca <colour>      colour of deck A (tone, auto tone, sub bass, contour); default: the settings' choice\n"
    "        --cb <colour>      the same for deck B\n"
    "        --look <ms>        limiter look-ahead 0..5 ms (above 0 at least 66 samples, the true-peak\n"
    "                           minimum); default: the settings (1.5 ms)\n"
    "        --oversampling <n> the soft clipper's and the exciter's: 2, 4 or 8; default: the settings (4)\n"
    "        --mono             both channels (L + R) / 2 through the chain; default: the settings (stereo)\n"
    "        --keep-delay       keep the look-ahead delay in the output, like a live device (default: removed)\n"
    "        --pcm24            write 24-bit PCM (default: 32-bit float, lossless)\n"
    "        --trace <file.csv> deck A every 100 ms: loudness in / out (momentary, short-term), the leveller's gain\n"
    "                           and state, compressor, density bands, de-esser, clipper and limiter (dB)\n"
    "  --presets            lists the loudness presets and colours (shipped and own) with their main values\n"
    "  --tone <file> [more files]\n"
    "        Long-term tone balance (60 Hz, 250 Hz, 3 kHz, 10 kHz against 1 kHz) and what the auto tone would\n"
    "        correct at 100 % (6 dB at most, treble 3 dB).\n"
    "  --measure <file.wav> [more files]\n"
    "        Integrated loudness, loudness range, max momentary / short-term, sample peak and true peak (16x),\n"
    "        per deck for 4-channel files.\n"
    "  --devices            lists the ASIO drivers (from the registry) and the WASAPI / DirectSound / MME devices,\n"
    "                       with the shared periods each WASAPI input allows\n"
    "                       (lists only: no driver or device is opened)\n"
    "  --ini <file>         use this settings file instead of DJAP.ini (read only)\n"
    "  --version, --help\n"
    "\n"
    "Exit code: 0 done, 1 wrong command line, 2 a file could not be read or written.\n";

int usageError(const std::string& msg) {
    std::fprintf(stderr, "DJAPCmd: %s\nRun DJAPCmd --help for the commands.\n", msg.c_str());
    return 1;
}

std::string pathText(const fs::path& p) { return toUtf8(p.wstring()); }

std::string duration(double seconds) {
    char buf[32];
    const int m = static_cast<int>(seconds / 60.0);
    std::snprintf(buf, sizeof buf, "%d:%04.1f", m, seconds - 60.0 * m);
    return buf;
}

std::string lufs(double v) {
    if (v <= kFloorDb + 1.0) return "    -inf";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%8.2f", v);
    return buf;
}

// ---- --measure ----

struct Measurement {
    double integrated, range, maxM, maxS, samplePeakDb, truePeakDb;
};

Measurement measureChannels(const WavData& w, int first, int count) {
    const size_t n = w.frames();
    std::vector<float> l(n), r;
    if (count == 2) r.resize(n);
    float sp = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        l[i] = w.samples[i * static_cast<size_t>(w.channels) + static_cast<size_t>(first)];
        sp = std::max(sp, std::fabs(l[i]));
        if (count == 2) {
            r[i] = w.samples[i * static_cast<size_t>(w.channels) + static_cast<size_t>(first) + 1];
            sp = std::max(sp, std::fabs(r[i]));
        }
    }
    LoudnessMeter m;
    measureStereo(m, w.sampleRate, l.data(), count == 2 ? r.data() : nullptr, n);
    TruePeakDetector tp;
    tp.configure(16, 128, 10.0); // far finer than the live meter: a reference for files
    float peak = 0.0f;
    for (const std::vector<float>* ch : {&l, &r}) {
        if (ch->empty()) continue;
        tp.reset();
        for (float x : *ch) peak = std::max(peak, tp.push(x));
        for (int i = 0; i <= tp.delay(); ++i) peak = std::max(peak, tp.push(0.0f));
    }
    return {m.integrated(), m.loudnessRange(), m.maxMomentary(), m.maxShortTerm(), linToDb(sp), linToDb(peak)};
}

// The filters are designed for the file's own rate: 8 to 384 kHz (else a message, not nonsense numbers).
bool usableRate(const fs::path& f, const WavData& w) {
    if (w.sampleRate >= 8000 && w.sampleRate <= 384000) return true;
    std::fprintf(stderr, "%s: a sample rate of %d Hz cannot be measured (8000 .. 384000 Hz)\n", pathText(f).c_str(),
                 w.sampleRate);
    return false;
}

int measure(const std::vector<fs::path>& files) {
    int result = 0;
    for (const fs::path& f : files) {
        WavData w;
        std::string err;
        if (!readAudioFile(f, w, err)) {
            std::fprintf(stderr, "%s: %s\n", pathText(f).c_str(), err.c_str());
            result = 2;
            continue;
        }
        if (!usableRate(f, w)) {
            result = 2;
            continue;
        }
        std::printf("%s: %d Hz, %d channel%s, %s\n", pathText(f).c_str(), w.sampleRate, w.channels,
                    w.channels == 1 ? "" : "s", duration(static_cast<double>(w.frames()) / w.sampleRate).c_str());
        std::printf("              I (LUFS)  LRA (LU)  max M     max S     peak dBFS  true peak dBTP\n");
        struct Part {
            const char* label;
            int first, count;
        };
        std::vector<Part> parts;
        if (w.channels == 1) parts = {{"mono", 0, 1}};
        else if (w.channels == 4) parts = {{"deck A", 0, 2}, {"deck B", 2, 2}};
        else parts = {{"stereo", 0, 2}};
        if (w.channels != 1 && w.channels != 2 && w.channels != 4)
            std::printf("  (%d channels: only channels 1-2 measured)\n", w.channels);
        for (const Part& p : parts) {
            const Measurement m = measureChannels(w, p.first, p.count);
            std::printf("  %-10s %s  %8.2f  %s  %s  %9.2f  %9.2f\n", p.label, lufs(m.integrated).c_str(), m.range,
                        lufs(m.maxM).c_str(), lufs(m.maxS).c_str(), m.samplePeakDb, m.truePeakDb);
        }
    }
    return result;
}

// ---- --tone ----

std::string padded(const std::string& s, size_t width); // below

int tone(const std::vector<fs::path>& files) {
    int result = 0;
    std::printf("%-50s %8s %8s %8s %8s   auto tone at 100 %%: bass low-mid presence treble\n", "file (balance in dB "
                "against 1 kHz)", "60 Hz", "250 Hz", "3 kHz", "10 kHz");
    for (const fs::path& f : files) {
        WavData w;
        std::string err;
        if (!readAudioFile(f, w, err)) {
            std::fprintf(stderr, "%s: %s\n", pathText(f).c_str(), err.c_str());
            result = 2;
            continue;
        }
        if (!usableRate(f, w)) {
            result = 2;
            continue;
        }
        const size_t n = w.frames(), ch = static_cast<size_t>(w.channels);
        std::vector<float> mono(n);
        for (size_t i = 0; i < n; ++i)
            mono[i] = ch == 1 ? w.samples[i] : 0.5f * (w.samples[i * ch] + w.samples[i * ch + 1]);
        double bal[kToneBands];
        toneBalance(mono.data(), n, w.sampleRate, bal);
        std::string name = pathText(f.filename());
        if (name.size() > 50) name = name.substr(0, 47) + "...";
        std::printf("%s %8.1f %8.1f %8.1f %8.1f  ", padded(name, 50).c_str(), bal[0], bal[1], bal[3], bal[4]);
        for (int b : {0, 1, 3, 4}) std::printf(" %+6.1f", clampd(kToneReference[b] - bal[b], b == 4 ? -3.0 : -6.0, b == 4 ? 3.0 : 6.0));
        std::printf("\n");
    }
    return result;
}

// ---- --presets ----

// Pads a UTF-8 string to `width` characters (printf's %-24s counts bytes: "één" would shift the columns).
std::string padded(const std::string& s, size_t width) {
    size_t chars = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++chars;
    return chars < width ? s + std::string(width - chars, ' ') : s;
}

int presets(const Settings& s, const fs::path& ini, bool iniFound) {
    std::printf("Settings: %s%s\n\n", pathText(ini).c_str(), iniFound ? "" : " (not there yet: shipped presets only)");
    std::printf("  %-24s %-8s %7s %9s %11s %6s %6s %8s  %s\n", "loudness", "kind", "target", "lev range", "comp thr/r",
                "bands", "clip", "ceiling", "used by");
    for (const Preset& p : s.presets) {
        const PresetValues& v = p.values;
        std::string kind = "own";
        if (p.shipped) {
            const PresetValues shipped = shippedPresetValues(shippedPresetIndex(p.name));
            kind = "shipped";
            for (int i = 0; i < kParamCount; ++i)
                if (std::fabs(v[static_cast<size_t>(i)] - shipped[static_cast<size_t>(i)]) > 1e-9) kind = "changed";
        }
        std::string used;
        if (s.deck[0].preset == p.name) used = "deck A";
        if (s.deck[1].preset == p.name) used += used.empty() ? "deck B" : ", deck B";
        char lev[16] = "off", comp[32] = "off", bands[8] = "off", clip[16] = "off";
        if (v[pLevOn] > 0.5) std::snprintf(lev, sizeof lev, "%.1f dB", v[pLevRange]);
        if (v[pCompOn] > 0.5) std::snprintf(comp, sizeof comp, "%+.1f / %.1f", v[pCompThreshold], v[pCompRatio]);
        if (v[pDensOn] > 0.5) std::snprintf(bands, sizeof bands, "%d", static_cast<int>(v[pDensBands]));
        if (v[pClipOn] > 0.5) std::snprintf(clip, sizeof clip, "%.1f", v[pClipDrive]);
        std::printf("  %s %-8s %7.1f %9s %11s %6s %6s %8.1f  %s\n", padded(p.name, 24).c_str(), kind.c_str(), v[pTarget], lev,
                    comp, bands, clip, v[pLimCeiling], used.c_str());
    }
    std::printf("\n  target LUFS, compressor threshold LU re target / ratio, clipper drive dB, limiter ceiling dBTP\n\n");
    std::printf("  %-24s %-8s %6s %8s %9s %7s %6s %5s %9s  %s\n", "colour", "kind", "bass", "low-mid", "presence",
                "treble", "auto", "sub", "contour", "used by");
    for (const Preset& p : s.colours) {
        const PresetValues& v = p.values;
        std::string kind = "own";
        if (p.shipped) {
            const PresetValues shipped = shippedColourValues(shippedColourIndex(p.name));
            kind = "shipped";
            for (int i = kFirstColourParam; i < kParamCount; ++i)
                if (std::fabs(v[static_cast<size_t>(i)] - shipped[static_cast<size_t>(i)]) > 1e-9) kind = "changed";
        }
        std::string used;
        if (s.deck[0].colour == p.name) used = "deck A";
        if (s.deck[1].colour == p.name) used += used.empty() ? "deck B" : ", deck B";
        std::printf("  %s %-8s %+6.1f %+8.1f %+9.1f %+7.1f %5.0f%% %4.0f%% %8.0f%%  %s\n", padded(p.name, 24).c_str(),
                    kind.c_str(), v[pToneBass], v[pToneLowMid], v[pTonePresence], v[pToneTreble], v[pAutoAmount],
                    v[pSubAmount], v[pLoudness], used.c_str());
    }
    std::printf("\n  tone in dB; auto tone, sub bass and loudness in %%\n");
    return 0;
}

// ---- --devices ----

int devices() {
#if AP_HAVE_ASIO
    const std::vector<AsioDriverEntry> list = AsioHost::drivers(); // registry only; opens nothing
    if (list.empty()) {
        std::printf("ASIO: no drivers installed.\n");
    } else {
        std::printf("ASIO drivers (%zu):\n", list.size());
        for (const AsioDriverEntry& d : list) std::printf("  %s %s\n", padded(d.name, 40).c_str(), toUtf8(d.path).c_str());
        std::printf("  (channels and sample rates: in DJAP's Setup window, which opens the driver)\n");
    }
#else
    std::printf("Built without the ASIO SDK: no ASIO driver list.\n");
#endif
    // the other APIs: device lists only (no stream is opened)
    for (AudioApi api : {AudioApi::Wasapi, AudioApi::DirectSound, AudioApi::Mme}) {
        std::vector<SysDevice> ins, outs;
        std::string err;
        std::printf("\n%s:\n", apiName(api));
        if (!SysAudio::devices(api, ins, outs, err)) {
            std::printf("  not available: %s\n", err.c_str());
            continue;
        }
        for (const SysDevice& d : ins) {
            std::printf("  in   %s%s\n", d.name.c_str(), d.isDefault ? "  (default)" : "");
            if (api != AudioApi::Wasapi) continue;
            // the periods of Windows' shared low-latency mode (asked only; no stream is opened)
            const SysAudio::EnginePeriods p = SysAudio::inputPeriods(d.name);
            if (p.known)
                std::printf("         shared periods at %u Hz: default %u, step %u, shortest %u, longest %u frames "
                            "(%.2f .. %.2f ms)\n",
                            p.rate, p.defaultFrames, p.stepFrames, p.minFrames, p.maxFrames,
                            1000.0 * p.minFrames / p.rate, 1000.0 * p.maxFrames / p.rate);
            else
                std::printf("         shared periods: not given (no low-latency mode)\n");
        }
        for (const SysDevice& d : outs) std::printf("  out  %s%s\n", d.name.c_str(), d.isDefault ? "  (default)" : "");
    }
    return 0;
}

// ---- --render ----

// Resolves a deck choice ("off", a preset name, or empty = the settings' choice for that deck).
bool chooseDeck(const Settings& s, int d, const std::string& choice, const std::string& colourChoice, double lookMs,
                RenderDeck& out, std::string& label, std::string& err) {
    out = RenderDeck();
    out.control.lookMs = lookMs;
    out.control.oversampling = s.oversampling;
    out.control.mono = s.mono;
    std::string name = choice;
    if (name.empty()) {
        if (!s.deck[d].processing) {
            label = "off (as in the settings)";
            return true;
        }
        name = s.deck[d].preset;
    }
    if (name == "off") {
        label = "off";
        return true;
    }
    const Preset* p = s.find(name);
    if (!p) {
        err = "there is no loudness preset \"" + name + "\" (see --presets)";
        return false;
    }
    const std::string colourName = colourChoice.empty() ? s.deck[d].colour : colourChoice;
    const Preset* c = s.find(colourName, PresetKind::Colour);
    if (!c) {
        err = "there is no colour \"" + colourName + "\" (see --presets)";
        return false;
    }
    out.used = true;
    out.values = withColour(p->values, c->values);
    label = "\"" + p->name + "\", colour \"" + c->name + "\"";
    return true;
}

// --trace: deck A's meter records summed to rows of 100 ms. Loudness per BS.1770 from the records' K-weighted energy
// (momentary = last 400 ms, short-term = last 3 s, ungated); gains are the row's last value (leveller) or its largest
// reduction (the others); state: quiet (the leveller holds), frozen, warm (catching up after a pause).
bool writeTrace(const fs::path& file, const std::vector<MeterRecord>& recs, double fs, std::string& err) {
    std::FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) {
        err = "cannot write the trace";
        return false;
    }
    std::fprintf(f, "time_s,in_M,in_S,out_M,out_S,leveller_dB,state,compressor_dB,compressor_in,");
    for (int b = 0; b < kMaxBands; ++b) std::fprintf(f, "density%d_dB,", b + 1);
    std::fprintf(f, "deesser_dB,clipper_dB,limiter_dB\n");
    struct Slice { double frames, in, out; };
    std::vector<Slice> hist; // one per record, for the sliding windows
    auto lufsOver = [&](double seconds, bool out) {
        double e = 0, n = 0;
        for (size_t i = hist.size(); i-- > 0 && n < seconds * fs;) {
            e += out ? hist[i].out : hist[i].in;
            n += hist[i].frames;
        }
        return n > 0 && e > 0 ? -0.691 + 10.0 * std::log10(e / n) : -99.0;
    };
    const double rowFrames = 0.1 * fs;
    double t = 0, inRow = 0;
    float comp = 0, deess = 0, clip = 0, lim = 0, dens[kMaxBands] = {};
    for (const MeterRecord& r : recs) {
        hist.push_back({static_cast<double>(r.frames), r.energyIn, r.energyOut});
        comp = std::max(comp, r.compressorDb);
        deess = std::max(deess, r.deEsserDb);
        clip = std::max(clip, r.clipperDb);
        lim = std::max(lim, r.limiterDb);
        for (int b = 0; b < kMaxBands; ++b)
            if (std::fabs(r.densityDb[b]) > std::fabs(dens[b])) dens[b] = r.densityDb[b];
        inRow += r.frames;
        t += r.frames / fs;
        if (inRow < rowFrames) continue;
        inRow = 0;
        const std::string state = std::string(r.flags & mfQuiet ? "quiet " : "") + (r.flags & mfFrozen ? "frozen " : "") +
                                  (r.flags & mfWarm ? "warm" : "");
        std::fprintf(f, "%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%s,%.2f,%.2f,", t, lufsOver(0.4, false), lufsOver(3.0, false),
                     lufsOver(0.4, true), lufsOver(3.0, true), r.levellerDb, state.c_str(), comp, r.compressorIn);
        for (int b = 0; b < kMaxBands; ++b) std::fprintf(f, "%.2f,", b < r.bands ? dens[b] : 0.0f);
        std::fprintf(f, "%.2f,%.2f,%.2f\n", deess, clip, lim);
        comp = deess = clip = lim = 0;
        for (float& d : dens) d = 0;
        if (hist.size() > 4096) hist.erase(hist.begin(), hist.begin() + 2048); // > 3 s kept
    }
    std::fclose(f);
    return true;
}

void report(const char* deck, const std::string& label, const RenderStats& st, bool used) {
    std::printf("  %s: %s\n", deck, label.c_str());
    std::printf("      in %s LUFS  ->  out %s LUFS, LRA %.1f -> %.1f LU, true peak %.2f dBTP\n",
                lufs(st.in.integrated()).c_str(), lufs(st.out.integrated()).c_str(), st.in.loudnessRange(),
                st.out.loudnessRange(), st.maxTruePeakDb);
    if (used)
        std::printf("      leveller %+.1f dB on average, compressor up to %.1f dB, limiter up to %.1f dB (%.2f dB on "
                    "average)\n",
                    st.meanLevellerDb, st.maxCompressorDb, st.maxLimiterDb, st.meanLimiterDb);
}

int render(const Settings& s, const fs::path& in, const fs::path& out, const std::string& a, const std::string& b,
           const std::string& ca, const std::string& cb, double lookMs, bool keepDelay, bool pcm24,
           const fs::path& trace) {
    std::error_code ec;
    if (fs::exists(out, ec) && fs::equivalent(in, out, ec)) return usageError("the output file is the input file");
    if (!trace.empty()) { // checked before the render (it once was after it, and could overwrite the input or output)
        const auto same = [&](const fs::path& x) {
            // Windows' file names ignore case: OUT.WAV is out.wav (the output does not exist yet: equivalent() fails)
            const std::wstring a = fs::absolute(trace, ec).lexically_normal().wstring(),
                               b = fs::absolute(x, ec).lexically_normal().wstring();
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                        TRUE) == CSTR_EQUAL ||
                   (fs::exists(trace, ec) && fs::exists(x, ec) && fs::equivalent(trace, x, ec));
        };
        if (same(in) || same(out)) return usageError("--trace must name a file of its own, not the input or the output");
        const fs::path traceDir = trace.has_parent_path() ? trace.parent_path() : fs::path(L".");
        if (!fs::is_directory(traceDir, ec)) {
            std::fprintf(stderr, "%s: the folder does not exist\n", pathText(traceDir).c_str());
            return 2;
        }
    }
    WavData w;
    std::string err;
    if (!readAudioFile(in, w, err)) {
        std::fprintf(stderr, "%s: %s\n", pathText(in).c_str(), err.c_str());
        return 2;
    }
    if (w.channels != 2 && w.channels != 4)
        return usageError(pathText(in) + " has " + std::to_string(w.channels) + (w.channels == 1 ? " channel" : " channels") +
                          "; --render needs 2 (deck A) or 4 (deck A + deck B)");
    const fs::path outDir = out.has_parent_path() ? out.parent_path() : fs::path(L".");
    if (!fs::is_directory(outDir, ec)) {
        std::fprintf(stderr, "%s: the folder does not exist\n", pathText(outDir).c_str());
        return 2;
    }
    if (w.channels == 2 && !b.empty()) return usageError("--b needs a 4-channel file (deck B = channels 3-4)");
    RenderDeck decks[2];
    std::string labels[2];
    for (int d = 0; d < w.channels / 2; ++d)
        if (!chooseDeck(s, d, d == 0 ? a : b, d == 0 ? ca : cb, lookMs, decks[d], labels[d], err)) return usageError(err);

    std::printf("%s: %d Hz, %d channels (%s), %s\n", pathText(in).c_str(), w.sampleRate, w.channels,
                w.channels == 4 ? "deck A + deck B" : "deck A", duration(static_cast<double>(w.frames()) / w.sampleRate).c_str());
    WavData o;
    RenderResult res;
    std::vector<MeterRecord> recs;
    RecordSink sink;
    if (!trace.empty()) sink = [&recs](int d, const MeterRecord& r) { if (d == 0) recs.push_back(r); };
    if (!renderOffline(w, decks, !keepDelay, 256, o, res, err, sink)) {
        std::fprintf(stderr, "render: %s\n", err.c_str());
        return 2;
    }
    for (int d = 0; d < w.channels / 2; ++d) report(d == 0 ? "deck A" : "deck B", labels[d], res.deck[d], decks[d].used);
    std::printf("  latency %d frames (%.2f ms): %s\n", res.latency, 1000.0 * res.latency / w.sampleRate,
                keepDelay ? "kept in the output" : "removed from the output");
    std::printf("  processing took %.2f s for %.1f s of audio (%.2f %% of one core)\n", res.processSeconds,
                res.audioSeconds, res.audioSeconds > 0 ? 100.0 * res.processSeconds / res.audioSeconds : 0.0);
    if (!writeWav(out, o, pcm24, err)) {
        std::fprintf(stderr, "%s: %s\n", pathText(out).c_str(), err.c_str());
        return 2;
    }
    std::printf("Wrote %s (%s)\n", pathText(out).c_str(), pcm24 ? "24-bit PCM" : "32-bit float");
    if (!trace.empty()) {
        if (!writeTrace(trace, recs, w.sampleRate, err)) {
            std::fprintf(stderr, "%s: %s\n", pathText(trace).c_str(), err.c_str());
            return 2;
        }
        std::printf("Wrote %s (deck A every 100 ms)\n", pathText(trace).c_str());
    }
    return 0;
}

int run(int argc, wchar_t** argv); // below

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // a file too long for the memory, or anything else unexpected: a message and exit code 2, never a crash
    try {
        return run(argc, argv);
    } catch (const std::bad_alloc&) {
        std::fputs("not enough memory (the file is too long to process at once)\n", stderr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
    }
    return 2;
}

namespace {

int run(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::fputs(kUsage, stdout);
        return 1;
    }

    // options shared by the commands
    std::string command;
    std::vector<fs::path> files;
    std::string deckA, deckB, colourA, colourB;
    fs::path ini, trace;
    double lookMs = -1.0;
    int oversampling = 0; // 0: the settings
    bool mono = false;    // --mono: this run mono (else the settings)
    bool keepDelay = false, pcm24 = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string a = toUtf8(args[i]);
        auto value = [&](std::string& outValue) {
            if (i + 1 >= args.size()) return false;
            outValue = toUtf8(args[++i]);
            return true;
        };
        if (a == "--help" || a == "-h" || a == "/?") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (a == "--version") {
            std::printf("DJAPCmd %s\n", AP_VERSION_STRING);
            return 0;
        } else if (a == "--ca" || a == "--cb") {
            std::string& target = a == "--ca" ? colourA : colourB;
            if (!value(target) || target.empty()) return usageError(a + " needs a colour name");
        } else if (a == "--render" || a == "--presets" || a == "--measure" || a == "--devices" || a == "--tone") {
            if (!command.empty()) return usageError("one command at a time (" + command + " and " + a + ")");
            command = a;
        } else if (a == "--a" || a == "--b") {
            std::string& target = a == "--a" ? deckA : deckB;
            if (!value(target) || target.empty()) return usageError(a + " needs a loudness preset name or off");
        } else if (a == "--look") {
            std::string v;
            if (!value(v)) return usageError("--look needs a value in ms");
            size_t used = 0;
            try {
                lookMs = std::stod(v, &used);
            } catch (...) {
                used = 0;
            }
            if (used == 0 || used != v.size()) return usageError("--look needs a number of ms, not \"" + v + "\"");
            if (!(lookMs >= 0.0 && lookMs <= kMaxLookMs)) return usageError("--look must lie in 0..5 ms");
        } else if (a == "--oversampling") {
            std::string v;
            if (!value(v) || (v != "2" && v != "4" && v != "8")) return usageError("--oversampling needs 2, 4 or 8");
            oversampling = v[0] - '0';
        } else if (a == "--mono") {
            mono = true;
        } else if (a == "--ini") {
            std::string v;
            if (!value(v)) return usageError("--ini needs a file");
            ini = fs::path(args[i]);
        } else if (a == "--trace") {
            std::string v;
            if (!value(v)) return usageError("--trace needs a file");
            trace = fs::path(args[i]);
        } else if (a == "--keep-delay") {
            keepDelay = true;
        } else if (a == "--pcm24") {
            pcm24 = true;
        } else if (a.size() > 1 && a[0] == '-') {
            return usageError("unknown option " + a);
        } else {
            files.push_back(fs::path(args[i]));
        }
    }
    if (command.empty()) return usageError("no command given");

    // settings: read only; the command line never writes them
    const bool ownIni = !ini.empty();
    if (!ownIni) ini = settingsFile();
    Settings s;
    std::error_code ec;
    const bool iniFound = fs::exists(ini, ec);
    if (iniFound && !s.load(ini)) {
        std::fprintf(stderr, "%s: cannot read the settings\n", pathText(ini).c_str());
        return 2;
    }
    if (ownIni && !iniFound) return usageError("settings file " + pathText(ini) + " not found");
    const bool lookGiven = lookMs >= 0.0;
    if (!lookGiven) lookMs = s.lookAheadMs;
    if (oversampling) s.oversampling = oversampling; // this run only: the command line never writes the settings
    if (mono) s.mono = true;

    if (command == "--render") {
        if (files.size() != 2) return usageError("--render needs an input and an output file");
        return render(s, files[0], files[1], deckA, deckB, colourA, colourB, lookMs, keepDelay, pcm24, trace);
    }
    if (!deckA.empty() || !deckB.empty() || !colourA.empty() || !colourB.empty() || lookGiven || keepDelay || pcm24 ||
        !trace.empty() || oversampling || mono)
        return usageError("--a, --b, --ca, --cb, --look, --oversampling, --mono, --keep-delay, --pcm24 and --trace "
                          "belong to "
                          "--render");
    if (command == "--measure") {
        if (files.empty()) return usageError("--measure needs at least one file");
        return measure(files);
    }
    if (command == "--tone") {
        if (files.empty()) return usageError("--tone needs at least one file");
        return tone(files);
    }
    if (!files.empty()) return usageError(command + " takes no files");
    if (command == "--presets") return presets(s, ini, iniFound);
    return devices();
}

} // namespace
