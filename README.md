# DJAP (DJ Audio Processor)

A standalone Windows loudness and dynamics processor. For DJs: two independent stereo channels - deck A and deck
B - come in on an ASIO interface, each runs its own processing chain with its own preset, and goes out again on
its own output pair. For other Windows users: one channel, one stereo processor (Audio window, "Processing"). The
same code renders WAV files offline. Licence: GPL-3.0.

## What it does

Per deck, in this order (`docs/design.md` sections 2-4):

- **Leveller** - slow automatic gain that brings each song to the preset's target loudness (EBU R 128 / BS.1770),
  holds during fades, breakdowns and pauses, catches up quickly after a stopped deck.
- **Compressor** - wide-band, soft knee, threshold relative to the target.
- **Density** - multiband (1-7 bands, flat sum): each band is compared with its own recent level.
- **Bass enhancer, exciter, de-esser, stereo width** - optional colour stages.
- **Soft clipper** and **look-ahead true-peak limiter** - the output never passes the ceiling (default -1 dBTP).

The limiter look-ahead (default 1.5 ms, 0-5 ms) is the only delay the processing adds; bypass is the input delayed by
the same amount, bit-exact, so switching never jumps. Five shipped presets go from easy listening to very loud
(Gentle -18, Light -16, Medium -14, Loud -12, Very Loud -10 LUFS), and three follow a medium: FM Station (the dense FM sound at about -13 LUFS), DAB+ (EBU R 128: -23 LUFS, wide dynamics) and Web Stream (-16 LUFS, -2 dBTP for MP3 / AAC); every value of every preset can be changed, and
own presets can be saved, renamed and deleted.

Besides its preset each deck has a **colour**: the tone of the music, kept apart from its loudness - bass, low-mid,
presence and treble controls, an automatic tone correction that pulls each song's balance towards that of modern
productions (a thin old record gets more bass, a dull one more highs), a sub-bass generator (an octave under the
bass) and a loudness contour for quiet listening. The leveller keeps the loudness on target, so a colour changes
only the balance. Nine shipped colours (Neutral, Warm, Bass+, Less mid, Bright, Vocal, Oldies, Auto, Quiet); own
colours are managed like presets.

## Programs

- `DJAP.exe` - the GUI: two deck strips (preset and colour - one choice when both decks share them -, processing /
  bypass, sliders for bass level, bass enhancer, bass width and bass frequency, meters for input and output
  loudness, true peak, the gain of every stage and the spectrum of input and output), an analysis window
  (compressor curve, loudness and gain over the last minute, goniometer, correlation and stereo width), a preset
  window with the stage switches and every parameter, a colour window,
  tooltips on every control, and the
  Audio window (audio API - ASIO, WASAPI shared or exclusive, DirectSound, MME -; for ASIO the driver and channel
  pairs, for the others each deck's input and output device; sample rate, buffer size, the option to give both
  decks the same preset, start / stop, latency, real-time health). With ASIO a deck's input may also come from a
  WASAPI, DirectSound or MME device (for example a player sending into a virtual cable): the two clocks' drift is
  corrected by a slightly adjusted read speed (at most 0.2 %), at the cost of a buffer of about 10-20 ms, shown
  with the measured drift. The outputs can follow the Windows volume and mute (the keyboard's volume keys; on by
  default with one channel), applied after the processing.
  The program lives in the notification area (system tray): closing or minimising the window hides it there while
  the audio goes on; a click on the icon brings it back, its menu has Exit. At the first start the Audio window
  opens, to choose the audio device. A second start shows the running program.
  Settings are kept in `DJAP.ini` next to the program (or in `%APPDATA%\DJAP\`); at the first start the settings of an earlier version (up to 0.1.57) are copied there, the old file stays.
  Test options: `--ini <file>`, `--simulate <file.wav>` (runs a file through both decks without sound),
  `--diag <file.txt>` (writes the call stack of a crash).
- `DJAPCmd.exe` - the command line, for renders and checks; it never plays audio and never changes the
  settings:

  ```
  DJAPCmd --render in.wav out.wav [--a <preset|off>] [--b <preset|off>] [--ca <colour>] [--cb <colour>] [--look <ms>] [--keep-delay] [--pcm24]
  DJAPCmd --presets
  DJAPCmd --measure <file.wav> [...]
  DJAPCmd --tone <file> [...]
  DJAPCmd --devices
  ```

  A 2-channel file is deck A, a 4-channel file is deck A (1-2) and deck B (3-4). Input may be WAV or anything
  Windows decodes (MP3, M4A, WMA, FLAC; mono plays on both channels); the output is WAV.

## Building

Windows, Visual Studio 2026 (MSVC, with its CMake and Ninja). The ASIO SDK is not in the repository: put it in
`third_party\asiosdk` first (`THIRD_PARTY.md`, `tools\get_asiosdk.ps1`). Without it the programs build without live
audio.

```
build.cmd                  # Release build of everything -> build\
build.cmd build-core core  # DSP core and unit tests only -> build-core\
```

## Installing

`tools\make_installer.ps1` builds the setup program `release\DJAP-Setup-<version>.exe` (Windows 10 / 11,
x64, English; Inno Setup 6 is needed on the building PC only). It installs both programs into `Program Files\DJAP`
with a Start menu entry and an uninstaller, and checks the PC first:

- the Windows components the programs use (no Visual C++ redistributable is needed: the runtime is linked in);
- Media Foundation, which the N editions of Windows lack: only `DJAPCmd` needs it, and setup offers to
  add Microsoft's Media Feature Pack from Windows Update;
- VAC (Virtual Audio Cable; the free Lite version is enough), needed only to run Windows' own sound through the
  processor with one channel - not for DJ use through an ASIO interface. Setup does not install it; when it is
  missing it says what it is for and offers its download page;
- the ASIO drivers it finds (listed on the last page before installing).

After the installation DJAP starts and, at the first start, opens the Audio window. Setup updates an existing
installation - also one of a version up to 0.1.57, whose folder and shortcuts it removes (its settings are
taken over by the program) - and asks to end a running DJAP first (notification area: Exit). Uninstalling keeps
the settings in `%APPDATA%\DJAP` unless you choose to delete them. The setup is not signed: SmartScreen warns
before the first run.

## Tests

```
powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1 -Renders
```

runs the unit tests (`tests\*_test.cpp`: loudness meter against the EBU Tech 3341 / 3342 cases, limiter and true
peak, every dynamics stage, the whole deck - bit-exact bypass, no clicks on any change, no allocation on the audio
path, CPU -, settings and presets, the audio engine with every device sample format, the drift correction with
simulated clocks) and `tools\render_check.ps1`,
which renders synthetic songs through every preset and checks the results with an independent meter.

## Layout

- `src/core/` - the DSP core (no Windows code; the base for a later VST3 plug-in)
- `src/app/` - GUI, command line, ASIO host
- `tests/` - unit tests
- `tools/` - test and setup scripts (`run_tests.ps1`, `render_check.ps1`, `get_asiosdk.ps1`, `music_survey.ps1`,
  `music_listen.ps1`, `make_installer.ps1`, `make_icon.py`)
- `installer/` - the setup program's script (`DJAP.iss`) and header images
- `assets/` - the program icon (`DJAP.ico`, `icon-256.png`) and the wide logo of the About window (`logo.png`),
  drawn by `tools/make_icon.py`
- `docs/design.md` - the design: every parameter with its unit, range, default and reason

ASIO is a trademark and software of Steinberg Media Technologies GmbH.
