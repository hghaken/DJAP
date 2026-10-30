# DJAP (DJ Audio Processor)

A standalone Windows loudness and dynamics processor. For DJs: two independent stereo channels - deck A and deck
B - come in on an ASIO® interface, each runs its own processing chain with its own preset, and goes out again on
its own output pair. For other Windows users: one channel, one stereo processor (Setup window, "Processing"). A
plug-in brings the same processing into DJ software, one deck per plug-in. The same code renders WAV files offline.
Copyright (C) 2026 hghaken. Licence: GPL-3.0-or-later (`LICENSE`); free software, with ABSOLUTELY NO WARRANTY.
The code was written with the AI assistant Claude (Anthropic), implemented from public standards and papers (the
sources are listed in the technical manual), and improved with outside reviews by Grok (xAI).

## Video

[![DJAP in three minutes - watch on YouTube](https://img.youtube.com/vi/1PePjk0rk-Y/maxresdefault.jpg)](https://www.youtube.com/watch?v=1PePjk0rk-Y)

[DJAP in three minutes on YouTube](https://www.youtube.com/watch?v=1PePjk0rk-Y): before and after, the loudness presets,
every value, the EQs, the analysis, Setup, Windows' own sound and the plug-in. (Intro, b-roll and voice-over are
AI-generated.)

## Download

The setup program is on the [releases page](https://github.com/hghaken/DJAP/releases/latest)
(`DJAP-Setup-<version>.exe`, Windows 10 / 11, 64-bit). It installs the DJAP program and, if chosen, the VST®3
plug-in for DJ software (into the computer's plug-in folder; an installation for all users).

<img src="docs/images/vst-compatible-logo.png" width="110" alt="VST compatible">
<img src="docs/images/asio-compatible-logo.png" width="90" alt="ASIO compatible">

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media
Technologies GmbH.

## Manuals

- [User manual](docs/manual.md) ([PDF](docs/DJAP-Manual.pdf)) - installing, connecting, every window, control and
  setting, loudness presets and EQs, the command line, troubleshooting.
- [Technical manual](docs/technical.md) ([PDF](docs/DJAP-Technical-Manual.pdf)) - how each part of the audio
  processing works, with formulas, measurements, tests and the sources (standards and papers) with links.

The setup installs both PDFs with Start menu entries. `python -I tools\make_manuals.py` rebuilds the PDFs from the
Markdown (markdown-it-py and Chrome or Edge).

## What it does

Per deck, in this order (`docs/design.md` sections 2-4):

- **Leveller** - slow automatic gain that brings each song to the loudness preset's target loudness (EBU R 128 / BS.1770),
  holds during fades, breakdowns and pauses, catches up quickly after a stopped deck.
- **Compressor** - wide-band, soft knee, threshold relative to the target.
- **MB Compressor** - multiband (1-7 bands, flat sum): each band is compared with its own recent level.
- **Bass enhancer, exciter, de-esser, stereo width** - optional stages that shape the sound.
- **Soft clipper** and **look-ahead true-peak limiter** - the output never passes the ceiling (default -1 dBTP).

The limiter look-ahead (default 1.5 ms, 0-5 ms) is the only latency the processing adds (the multiband filters let the
lowest frequencies follow a few milliseconds later, as in any multiband processor); bypass is the input delayed by
the same amount, bit-exact, so switching never jumps. Five shipped loudness presets go from easy listening to very loud
(Gentle -18, Light -16, Medium -14, Loud -12, Very Loud -10 LUFS), and three follow a medium: FM Station (the dense FM sound at about -13 LUFS), DAB+ (EBU R 128: -23 LUFS, wide dynamics) and Web Stream (-16 LUFS, -2 dBTP for MP3 / AAC); every value of every loudness preset can be changed, and
own loudness presets can be saved, renamed and deleted.

Besides its loudness preset each deck has an **EQ**: the tone of the music, kept apart from its loudness - bass, low-mid,
presence and treble controls, an automatic tone correction that pulls each song's balance towards that of modern
productions (a thin old record gets more bass, a dull one more highs), a sub-bass generator (an octave under the
bass) and a contour for quiet listening. The leveller keeps the loudness on target, so an EQ changes
only the balance. Nine shipped EQs (Neutral, Warm, Bass+, Less mid, Bright, Vocal, Oldies, Auto, Quiet); own
EQs are managed like loudness presets.

## Programs

- `DJAP.exe` - the GUI: two deck strips (loudness preset and EQ - one choice when both decks share them -, processing /
  bypass, meters for input and output loudness, true peak, the gain of every stage and the spectrum of input and
  output), a spectrum window (a click on a deck's spectrum: the EQ as a curve with points to drag, the
  MB Compressor bands with their gains, the loudness preset and EQ lists and the four bass sliders - bass level,
  enhancer, width and frequency), an analysis window (compressor curve, loudness and gain over the last minute,
  goniometer, correlation and stereo width), a loudness window with the stage switches and every parameter, a
  EQ window, tooltips on every control (they can be switched off), windows that open where they were left, and
  the Setup window (audio API - ASIO, WASAPI shared or exclusive, DirectSound, MME -; for ASIO the driver and channel
  pairs, for the others each deck's input and output device; sample rate, buffer size, the option to give both
  decks the same loudness preset, start / stop, latency, real-time health). With ASIO a deck's input may also come from a
  WASAPI, DirectSound or MME device (for example a player sending into a virtual cable): the two clocks' drift is
  corrected by a slightly adjusted read speed (at most 0.2 %), at the cost of a buffer of about 8-23 ms, shown
  with the measured drift. The outputs can follow the Windows volume and mute (the keyboard's volume keys; on by
  default with one channel), applied after the processing.
  The program lives in the notification area (system tray): closing or minimising the window hides it there while
  the audio goes on; a click on the icon brings it back, its menu has Shutdown. At the first start the Setup window
  opens, to choose the audio device. A second start shows the running program.
  Settings are kept in `DJAP.ini` next to the program (or in `%APPDATA%\DJAP\`); at the first start the settings of an earlier version (up to 0.1.57) are copied there, the old file stays.
  Test options: `--ini <file>`, `--simulate <file.wav>` (runs a file through both decks without sound),
  `--diag <file.txt>` (writes the call stack of a crash).
- `DJAPCmd.exe` - the command line, for renders and checks; it never plays audio and never changes the
  settings:

  ```
  DJAPCmd --render in.wav out.wav [--a <preset|off>] [--b <preset|off>] [--ca <EQ>] [--cb <EQ>] [--look <ms>] [--oversampling <2|4|8>] [--keep-delay] [--pcm24] [--trace <file.csv>]
  DJAPCmd --presets
  DJAPCmd --measure <file.wav> [...]
  DJAPCmd --tone <file> [...]
  DJAPCmd --devices
  ```

  A 2-channel file is deck A, a 4-channel file is deck A (1-2) and deck B (3-4). Input may be WAV or anything
  Windows decodes (MP3, M4A, WMA, FLAC; mono plays on both channels); the output is WAV.
- `DJAP.vst3` - the plug-in for DJ software: one deck (stereo in, stereo out) with the same chain, the same loudness presets
  and EQs (read from the program's settings file) and a window like a deck strip of the program: loudness preset,
  EQ, look-ahead, Bypass, the four bass sliders and the meter panel. Every value is a parameter the host can
  show and automate; a project stores the values themselves. The host is told the latency (the look-ahead).

## Building

Windows, Visual Studio 2026 (MSVC, with its CMake and Ninja). The ASIO SDK is not in the repository: put it in
`third_party\asiosdk` first (`THIRD_PARTY.md`, `tools\get_asiosdk.ps1`). Without it the programs build without live
audio. The plug-in needs Steinberg's plug-in SDK in `third_party\vst3sdk` (`tools\get_vst3sdk.ps1 -Yes`); without
it everything else builds.

```
build.cmd                  # Release build of everything -> build\
build.cmd build-core core  # DSP core and unit tests only -> build-core\
```

## Installing

`tools\make_installer.ps1` builds the setup program `release\DJAP-Setup-<version>.exe` (Windows 10 / 11,
x64, English; Inno Setup 6 is needed on the building PC only). It installs both programs into `Program Files\DJAP`
with a Start menu entry and an uninstaller - and, on its "Select Components" page, the plug-in for DJ software into
`Common Files\VST3\DJAP` (an installation for all users only; both are chosen by default) - and checks the PC first:

- the Windows components the programs use (no Visual C++ redistributable is needed: the runtime is linked in);
- Media Foundation, which the N editions of Windows lack: only `DJAPCmd` needs it, and setup offers to
  add Microsoft's Media Feature Pack from Windows Update;
- VAC (Virtual Audio Cable®, <https://vac.muzychenko.net/en/>; the free Lite version is enough), needed only to run Windows' own sound through the
  processor with one channel - not for DJ use through an ASIO interface. Setup does not install it; when it is
  missing it says what it is for and offers its download page;
- the ASIO drivers it finds (listed on the last page before installing).

<img src="docs/images/vac-logo.png" width="48" alt="Virtual Audio Cable">

After the installation DJAP starts and, at the first start, opens the Setup window. A plug-in that DJ software has
loaded is never a reason to close that software: the setup replaces it when Windows starts again. Setup updates an existing
installation - also one of a version up to 0.1.57, whose folder and shortcuts it removes (its settings are
taken over by the program) - and asks to end a running DJAP first (notification area: Shutdown). Uninstalling keeps
the settings in `%APPDATA%\DJAP` unless you choose to delete them. The setup is not signed: SmartScreen warns
before the first run.

Updates: once a day DJAP asks GitHub whether a newer release of this repository exists and tells you (a message at
the notification-area icon, in the status bar and in the About window - right-click: About DJAP...; switch it off
in Setup -> Updates). Nothing is downloaded until you click Install update in the About window (while the audio runs
it asks first). When GitHub's API refuses (its limit of 60 requests an hour per network), the check reads the release
page instead. The setup is then downloaded, checked against the SHA-256 checksum GitHub publishes for it,
installed silently into the same folder, and DJAP starts again with your settings; audio that ran before the update
starts again by itself.

## Tests

```
powershell -ExecutionPolicy Bypass -File tools\run_tests.ps1 -Renders
```

runs the unit tests (`tests\*_test.cpp`: loudness meter against the EBU Tech 3341 / 3342 cases, limiter and true
peak, every dynamics stage, the whole deck - bit-exact bypass, no clicks on any change, no allocation on the audio
path, CPU -, settings and loudness presets, the audio engine with every device sample format, the drift correction with
simulated clocks; `plugin_test` loads the built plug-in like a host and checks it is bit-exact with the program's
deck) and `tools\render_check.ps1`,
which renders synthetic songs through every loudness preset and checks the results with an independent meter.

## Layout

- `src/core/` - the DSP core (no Windows code; shared by the program, the command line and the plug-in)
- `src/app/` - GUI, command line, ASIO host
- `src/plugin/` - the plug-in for DJ software
- `tests/` - unit tests
- `tools/` - test and setup scripts (`run_tests.ps1`, `render_check.ps1`, `get_asiosdk.ps1`, `get_vst3sdk.ps1`,
  `music_survey.ps1`, `music_listen.ps1`, `make_installer.ps1`, `make_icon.py`, `make_manuals.py`, `publish.ps1`)
- `installer/` - the setup program's script (`DJAP.iss`) and header images
- `assets/` - the program icon (`DJAP.ico`, `icon-256.png`) and the wide logo of the About window (`logo.png`),
  drawn by `tools/make_icon.py`
- `docs/design.md` - the design: every parameter with its unit, range, default and reason
- `docs/manual.md`, `docs/technical.md` - the user and technical manuals (and their PDFs); `docs/images/` - the
  manual's screenshots

## Disclaimer

DJAP is provided "as is", without warranty of any kind, and you use it at your own risk. It can make sound louder
than its source: start with the volume low, and never set levels that can damage hearing, speakers or other
equipment. The authors are not liable for any damage to hearing, equipment, recordings, data or settings, or for
loss of income (for example from a dropout during a performance). The meters follow the published loudness
standards but are not a certified measuring instrument. The code was written with an AI assistant and tested, but
may still contain errors. Third-party software such as a virtual audio cable is used under its own licence and at
your own responsibility. DJAP is not affiliated with, endorsed or sponsored by any company or product named here;
such names only identify what DJAP works with.

## Licence and trademarks

DJAP is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See `LICENSE`. The third-party parts and their
licences are in `THIRD_PARTY.md`; the Steinberg ASIO SDK files a release was built with are attached to that
release.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media
Technologies GmbH. Windows is a trademark of the Microsoft group of companies. Virtual Audio Cable is a registered
trademark of its developer, Eugene Muzychenko; its logo is shown only to identify it. Other names are trademarks of their
respective owners.
