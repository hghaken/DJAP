# Third-party software

## Steinberg ASIO SDK 2.3.4

- **What:** the ASIO host interface (`common/asio.cpp`, `common/asio.h`, `host/asiodrivers.cpp`,
  `host/pc/asiolist.cpp` and their headers), compiled into the library `asiosdk` and linked into
  `DJAP.exe` and `DJAPCmd.exe` (the command line only reads the driver list).
- **Licence:** the SDK is dual-licensed by Steinberg Media Technologies GmbH: the proprietary Steinberg ASIO
  License, or the GNU General Public License version 3. DJAP uses it under the **GPL v3** option, which
  matches DJAP's own licence (`LICENSE`). The SDK's licence text is in `third_party/asiosdk/LICENSE.txt`.
- **Not in the repository:** `third_party/asiosdk/` is git-ignored. Get it with
  `tools\get_asiosdk.ps1` - either copied from an SDK already on the computer (`-From <folder>`) or downloaded
  from Steinberg after a confirmation, checked against the SHA-256 of the 2.3.4 release
  (`D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA`).
- **Not changed:** the SDK is used as it is. One quirk is worked around in our code instead: its driver list never
  fills in the driver's DLL path, so `AsioHost::drivers()` reads it from the registry.
- **Trademark:** ASIO is a trademark and software of Steinberg Media Technologies GmbH. The ASIO logo is not used.

## Steinberg VST 3 SDK v3.8.1 (plug-in)

- **What:** the plug-in interfaces and base classes (`pluginterfaces`, `base`, `public.sdk`: the factory, the
  single-component effect, parameters, the plug-in view base, the Windows module entry), compiled into
  `DJAP.vst3`; its validator, editor test host and module-info tool are built for the tests only. VSTGUI is not
  used (the plug-in's window is DJAP's own Win32 code).
- **Licence:** MIT, Copyright (c) 2026 Steinberg Media Technologies GmbH:

  > Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
  > documentation files (the "Software"), to deal in the Software without restriction, including without limitation
  > the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
  > to permit persons to whom the Software is furnished to do so, subject to the following conditions:
  >
  > The above copyright notice and this permission notice shall be included in all copies or substantial portions
  > of the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
  > TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  > THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
  > CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  > DEALINGS IN THE SOFTWARE.

  MIT allows use in GPL v3 software.
- **Not in the repository:** `third_party/vst3sdk/` is git-ignored; `tools\get_vst3sdk.ps1 -Yes` clones the tag
  `v3.8.1_build_84` with its submodules from github.com/steinbergmedia/vst3sdk and checks its commit
  (`3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`). Used unchanged.
- **Trademark:** VST is a registered trademark of Steinberg Media Technologies GmbH. The name and the VST
  Compatible Logo (`docs/images/vst-compatible-logo.png`, from the SDK) are used as the SDK's usage guidelines
  ask: on the download section of `README.md`, with this attribution.

## miniaudio 0.11.22

- **What:** `third_party/miniaudio/miniaudio.h` (David Reid), the device layer for WASAPI, DirectSound and MME.
  Compiled once (`src/app/miniaudio_build.cpp`) with only those three backends and without decoding, encoding,
  generation, resource manager, node graph and engine.
- **Licence:** a choice of public domain (Unlicense) or MIT No Attribution (`third_party/miniaudio/LICENSE`); both
  allow use in GPL v3 software. It is in the repository unchanged (copied from the developer's other project).

## Build and run-time components

- Microsoft C/C++ runtime and Windows SDK libraries (static runtime, `/MT`), from Visual Studio 2026.
- Windows common controls version 6 (through the program's manifest).
- Windows Media Foundation (part of Windows) decodes MP3 / M4A / WMA / FLAC input for the command line; no decoder
  is shipped with DJAP.

## Setup program

- The setup `DJAP-Setup-<version>.exe` is built with **Inno Setup 6** (Jordan Russell and Martijn Laan,
  jrsoftware.org), installed on the building PC only. The setup contains Inno Setup's installer and uninstaller
  runtime, which its licence (the Inno Setup License, a free licence) allows for any software; `installer/DJAP.iss`
  is our own script.

## Development only (not part of the programs)

- Pillow (Python imaging library) runs `tools/make_icon.py`, which draws the icon and the logo; the logo's text is
  set in Windows' Segoe UI. Only the resulting pictures are part of the program.
- markdown-it-py (MIT licence) runs `tools/make_manuals.py`, which turns the Markdown manuals into HTML; headless
  Chrome or Edge on the building PC prints them as PDF. Only the resulting PDFs are part of the setup.
- `audiotool.py` from the developer's audio-processing skill (numpy), used by `tools\render_check.ps1` as an
  independent loudness and true-peak meter. It is not distributed with DJAP.

## Standards and literature

The loudness meter and the true-peak detector follow ITU-R BS.1770 and EBU Tech 3341 / 3342; the compressor curve
follows Giannoulis, Massberg and Reiss (2012). These are specifications, not code; all code is written for this
project.
