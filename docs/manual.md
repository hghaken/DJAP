# DJAP user manual

DJAP (DJ Audio Processor) is a loudness and dynamics processor for Windows. It sits between a sound source and the
amplifier or recorder and makes every song come out at the same loudness, with a controlled sound and peaks that
never pass a set ceiling. This manual explains how to install it, connect it, and use every window, control and
setting. How the processing works inside is described in the separate technical manual (`DJAP-Technical-Manual.pdf`
in the installation folder; `docs/technical.md` on <https://github.com/hghaken/DJAP>).

This manual describes DJAP from version 0.1.96 on (the first version with updates from GitHub).

## Contents

1. [What DJAP does](#1-what-djap-does)
2. [Installing, updating and removing](#2-installing-updating-and-removing)
3. [Connecting DJAP](#3-connecting-djap)
4. [The main window](#4-the-main-window)
5. [The Setup window](#5-the-setup-window)
6. [Presets and the preset window](#6-presets-and-the-preset-window)
7. [Colours and the colour window](#7-colours-and-the-colour-window)
8. [The analysis window](#8-the-analysis-window)
9. [Latency and real-time health](#9-latency-and-real-time-health)
10. [Updates](#10-updates)
11. [The command-line program DJAPCmd](#11-the-command-line-program-djapcmd)
12. [The settings file](#12-the-settings-file)
13. [Troubleshooting](#13-troubleshooting)
14. [Glossary](#14-glossary)

---

## 1. What DJAP does

DJAP has two independent stereo channels, **deck A** and **deck B**. In a DJ set-up each deck of the DJ software or
each channel of the mixer is sent through its own DJAP deck, so both decks are processed separately and come out at
the same loudness. For other use - for example everything Windows plays - DJAP can also run as **one channel**: one
stereo processor.

Each deck runs the same chain of processing stages:

| Step | What it does for you |
|---|---|
| **Leveller** | A slow, automatic hand on the volume: quiet and loud songs come out at the same loudness. It does not follow the music within a song and holds still during fades, breakdowns and pauses. |
| **Colour** | The tone of the music: bass, low-mid, presence and treble, an automatic tone correction per song, a sub-bass generator and a loudness contour for quiet listening. |
| **Compressor** | Evens out the loud parts within a song. |
| **Density** | A multiband stage (1 to 7 frequency bands): a band that sticks out is turned down, one that falls away is lifted - the full, dense "radio" sound. |
| **Bass enhancer** | Overtones of the bass, so the bass is heard on small speakers. |
| **Exciter** | Overtones in the highs: shine and air. |
| **De-esser** | Takes the sharpness off "s" and "t" sounds. |
| **Stereo width** | A wider or narrower stereo image; the bass can be made mono. |
| **Soft clipper** | Rounds off the highest peaks, without delay. |
| **Limiter** | The safety net: the output never passes the ceiling (true peak, default -1 dBTP). |
| **Output gain** | A last trim, only downwards. |

What each step does and how much is set by the deck's **preset** (the loudness and dynamics) and its **colour**
(the tone). DJAP ships with 8 presets and 9 colours; every value can be changed and saved under your own name.

Things to know up front:

- **Low latency.** The only delay DJAP adds is the limiter's look-ahead: 1.5 ms by default (0 to 5 ms), the same for
  both decks so they stay in time with each other when mixed.
- **Bypass is exact.** With a deck on Bypass its input goes out unchanged, bit for bit, delayed by the same
  look-ahead - so switching between processed and unprocessed never shifts the sound in time.
- **No clicks.** Every change - a preset, a colour, a slider, a stage switched on or off - glides in over a few
  milliseconds. Starting and stopping the audio fade in and out.
- **Everything is saved at once.** There is no Save button: every change is stored in the settings file.

## 2. Installing, updating and removing

### 2.1 What you need

- Windows 10 or 11, 64-bit.
- For two decks: an audio interface with an **ASIO** driver and at least 4 inputs and 4 outputs (two stereo pairs
  each way).
- For one channel (Windows' own sound): any sound card. To run Windows' sound through DJAP you need a virtual audio
  cable, such as VAC (Virtual Audio Cable; the free Lite version is enough) - see 3.2.

No other software is needed: the programs carry their own runtime.

### 2.2 Installing

Run `DJAP-Setup-<version>.exe`. The setup is not signed, so Windows SmartScreen may warn before the first run
("More info", then "Run anyway").

The setup checks the computer first and shows what it found on the last page before installing:

- **Windows components** the programs use. If one is missing the setup stops and says which.
- **Media Foundation.** The "N" editions of Windows lack it. DJAP itself runs without it, but the command-line
  program DJAPCmd needs it to read MP3, M4A, WMA and FLAC files. The setup offers to add Microsoft's Media Feature
  Pack from Windows Update.
- **VAC.** Only needed for Windows' own sound with one channel. When it is missing the setup explains what it is for
  and can open its download page at the end. The setup never installs it.
- **ASIO drivers** found on the computer.

Options on the way:

- **Create a desktop shortcut** (off by default).
- **Start DJAP in the notification area when Windows starts, with the audio devices used last** (on by default).
  This can be changed later in the Setup window.

DJAP is installed in `C:\Program Files\DJAP`. Installing a newer version over an older one keeps all settings and
own presets. If DJAP is running, the setup asks to close it first.

### 2.3 The first start

At the very first start no audio device has been chosen yet, so the **Setup window** opens by itself. Choose the
processing (two decks or one channel), the audio API and the devices, then press **Start audio** - see chapter 3 and
chapter 5.

### 2.4 The notification area

DJAP lives in the notification area (the icons next to the clock). Closing or minimising the main window hides it
there; **the audio goes on**. The first time this happens a message says so.

- **Click** the icon to show the window again.
- **Right-click** the icon for its menu: *Open DJAP*, *Setup...*, *Exit*.
- The icon's tooltip shows whether the audio runs and each deck's preset and colour (or "bypass").

DJAP really ends only with **Exit** (from the icon's menu or the main window's right-click menu), when Windows shuts
down, or when a setup closes it. Before it stops, the audio fades out over 10 ms, so there is no click.

Only one DJAP runs at a time: starting it a second time brings the running one to the front.

### 2.5 Starting with Windows

With "Start DJAP when Windows starts" on (Setup window, or the setup's option), DJAP starts hidden in the
notification area when you log on, and starts the audio with the devices of the last session if the audio was
running when it was last closed. Devices that are not ready yet right after logging on are tried again every 5
seconds for 2 minutes. If you stopped the audio before closing DJAP, it stays stopped at the next start.

Windows' own list of startup apps (Settings > Apps > Startup) can switch it off too; DJAP shows that as off.

### 2.6 Updating

DJAP checks once a day whether a newer version has been published and tells you; installing it takes one click.
See chapter 10.

### 2.7 Removing

Use Windows' Settings > Apps > Installed apps > DJAP > Uninstall. The uninstaller removes the programs, the
shortcuts and the "start with Windows" entry. It then asks whether to delete your settings and own presets too
(`%APPDATA%\DJAP`); answer No to keep them for a later installation.

## 3. Connecting DJAP

### 3.1 DJ set-up with an ASIO interface (two decks)

DJAP needs the two decks as separate stereo signals and gives back two processed stereo signals:

```
deck A source -> interface inputs 1-2 -> DJAP deck A -> interface outputs 1-2 -> mixer / amplifier
deck B source -> interface inputs 3-4 -> DJAP deck B -> interface outputs 3-4 -> mixer / amplifier
```

The channel pairs can be chosen freely in the Setup window (any input pair and any output pair per deck, as long as
the two decks use different output pairs).

Where to take the deck signals from:

- **Before the channel fader and crossfader (pre-fader)** is best: the leveller then settles on a song while you
  pre-listen, and your fader moves are not "corrected".
- **After the fader** also works: the leveller's hold rule keeps fades and cuts from being pumped up, but it is
  never as clean as pre-fader.

EQ kills and filter sweeps look like level changes to the leveller. Deep cuts are caught by its hold rule; with
lighter EQ moves the leveller may make up part of the cut over a few seconds. A slower leveller (preset window,
Leveller > Speed) makes that less noticeable.

**An ASIO driver usually serves one program at a time.** If your DJ software uses the same interface through ASIO,
DJAP may not be able to open it, or opening it may take the interface away from the DJ software. Use a second
interface, or route the DJ software's deck outputs to the inputs DJAP reads through the interface's own mixer
software. DJAP always asks before it opens a driver.

### 3.2 Windows' own sound (one channel)

To process everything Windows plays (browser, music player, video):

1. Install a virtual audio cable (VAC). It adds a playback device ("Line 1") and a recording device of the same name
   connected to it.
2. Make the cable's playback device the **default playback device** in Windows' sound settings. All Windows sound now
   goes into the cable.
3. In DJAP's Setup window choose **Processing: One channel**, **Audio API: WASAPI**, as **Input** the cable's recording
   device and as **Output** your speakers or headphones.
4. Press **Start audio**.

With one channel, the outputs follow the Windows volume by default (the keyboard's volume keys and the taskbar
slider), after the processing - see 5.2. Without this, the volume keys would turn down the cable and the leveller
would turn it back up.

You can also keep an ASIO interface as the output and take the input from a WASAPI device: choose **Audio API: ASIO**
and **Input from: WASAPI devices** (see 5.1, "Deck inputs"). The two devices run on separate clocks; DJAP corrects
the drift between them at the cost of a buffer of about 10-20 ms.

### 3.3 Which audio API?

| API | Latency | Use it for |
|---|---|---|
| **ASIO** | lowest | Audio interfaces with an ASIO driver; the only API where both decks run in one synchronous stream. Best for DJs. |
| **WASAPI** shared | low to medium | Windows' own audio system, shared with other programs. Each deck has its own input and output device. |
| **WASAPI** exclusive | low | As shared, but DJAP takes the devices for itself while the audio runs (DJAP asks first). |
| **DirectSound**, **MME** | higher | Older interfaces; for devices without a good driver. |

With every API the processing itself adds only the limiter look-ahead; the rest of the latency is the devices'
buffers (chapter 9).

### 3.4 Test safely

Opening a driver or a device in exclusive mode takes it from other programs, and outputs you think are silent may
still be routed to speakers. For a first test, use outputs nobody listens to, start with a low volume on the
amplifier, and watch the meters before you turn up.

## 4. The main window

![The main window with two decks](images/main.jpg)

The main window shows one strip per deck (one strip, "Channel", over the whole width with one channel). The window has
no menu bar: **right-click anywhere in it** for *Setup...* and *Exit*. Every control has a tooltip that explains it
when the mouse rests on it.

### 4.1 The deck strip

- **Title row.** The deck's name and its route, for example `ASIO: inputs 1-2 -> ASIO: outputs 1-2`, or with
  other APIs the device names. With linked presets deck B shows "linked to deck A".
- **Graphs** opens the analysis window (chapter 8).
- **Bypass.** Lit (green): the deck's input goes out unchanged, delayed by the look-ahead. Not lit: the deck is
  processed. Saved per deck.
- **Preset** and its **Edit...** button: the deck's preset (the loudness and dynamics) and the preset window
  (chapter 6). The list runs from the quietest output to the loudest; your own presets are sorted in.
- **Colour** and its **Edit...** button: the deck's colour (the tone) and the colour window (chapter 7).

A new preset or colour is heard at once, without a click.

### 4.2 The bass sliders

Four sliders for the most-used bass settings. They change the preset or the colour itself, so every deck using that
preset or colour changes with it. A change is heard while dragging and saved when the slider is let go. Arrow keys and
Page Up / Page Down step the slider that has the focus.

| Slider | Range | What it does | Belongs to |
|---|---|---|---|
| **Bass level** | -12 .. +9 dB | More or less bass below the bass frequency (the colour's bass shelf). The leveller keeps the loudness, so this changes the balance, not the volume. 0 dB = off. | colour |
| **Bass enhancer** | 0 .. 100 % | Strength of the bass overtones, so the bass is heard on small speakers. 0 % switches the enhancer off; more switches it on. | preset |
| **Bass width** | 0 .. 100 % | Stereo width of the bass: 0 % = bass in mono (tighter in a club, safe on mono systems), 100 % = as recorded. Moving it switches the stereo-width stage on. | preset |
| **Bass frequency** | 40 .. 250 Hz | Where "bass" ends for the three sliders above: the colour's bass corner and, within their own ranges, the enhancer's frequency (60-160 Hz) and the width stage's bass split. Each can be set separately in the preset and colour windows. | both |

### 4.3 The meter panel

The dark panel under the sliders shows what the deck is doing.

**LED bars, left.** *In L / R* is the input level (sample peak in dBFS, including the preset's input gain); *Out L /
R* is the output level (true peak in dBTP). Lit segments show the level; the zone colours show amber and red near the
top (input: amber from -6, red from -1 dBFS; output: amber in the last 3 dB below the ceiling, red above it). A
single LED holds the peak for 1.5 s. Across each pair a **white** line shows the momentary loudness and a **yellow**
line the target loudness. The scale on the left is in dB, the one on the right in LUFS; they share one axis, so the
white line can be compared with the bars and with the target directly.

**Stage rows, right.** What each stage does now:

- **Leveller** - its gain, up (right of 0) or down (left), over its range.
- **Compressor**, **De-esser**, **Clipper**, **Limiter** - gain reduction, 0 to 12 dB.
- **Density** - one small bar per band (with the band's frequency range under it), up or down by up to 12 dB.

A stage that is switched off in the preset is shown greyed out.

**Spectrum.** The frequency content of the input (**original**, orange) and the output (**processed**, turquoise),
20 Hz to 20 kHz. The dotted lines are the density stage's band splits, with the band numbers at the top.

**Clicking in the panel** opens the setting behind it: a stage row opens the preset window on that stage's tab, the
In bars open the General tab (input gain, target), the Out bars the Limiter tab (ceiling).

### 4.4 The status bar

Left: the audio state - the driver or devices, sample rate, buffer size, the latency of the devices, the heaviest
period and the number of late periods (dropouts); with an input on another clock its extra latency and drift; error
messages; a new version when one is available. Right: the Windows volume when the outputs follow it, and the delay
the processing adds.

### 4.5 Linked presets

With "Deck A and deck B use the same preset" on (Setup window), the main window shows one set of controls - deck A's
preset, colour, sliders, Edit buttons and Bypass - and they count for both decks. Deck B's strip shows only its route
and its meters.

## 5. The Setup window

![The Setup window](images/setup.jpg)

Right-click the main window (or the notification-area icon) and choose **Setup...**. Every choice is saved at once.
While the audio runs, a new device, channel pair, sample rate, buffer size or input API is applied at once by starting
the audio again (a short fade-out and fade-in).

### 5.1 Audio

| Row | What it does |
|---|---|
| **Processing** | *Two decks* (deck A and deck B, each its own input, output and preset) or *One channel* (one stereo processor, for example for what Windows plays). |
| **Audio API** | ASIO, WASAPI, DirectSound or MME (see 3.3). Choosing another API stops the audio. **Exclusive** (WASAPI only): lower latency, but the devices are taken from other programs while the audio runs. |
| **Deck inputs** / **Input from** (ASIO only) | Where the decks' inputs come from: *ASIO input pairs*, or devices of WASAPI, DirectSound or MME (drift-corrected; adds a buffer of about 10-20 ms). The outputs stay ASIO. |
| **ASIO driver** | The installed ASIO drivers. **Open driver** opens it (DJAP asks first: it may take the interface from other programs); **Close driver** releases it; **Control panel** opens the driver's own settings (buffer size, clock source). Under it: the driver's state. |
| **Deck A / B input**, **output** | ASIO: a channel pair per deck, with the driver's channel names once it is open. Other APIs: an input and an output device per deck; *(none)* = the deck does not run. A saved device that is not connected shows "(not found)". |
| **Sample rate** | 44.1, 48, 88.2 or 96 kHz. ASIO uses this rate if the driver accepts it, else the interface's own rate. Other APIs convert when a device runs at another rate. |
| **Buffer size** | ASIO: the driver's preferred size, or a size it allows. Other APIs: the period asked of Windows. Smaller = less latency, more risk of dropouts (watch Health). |
| **Look-ahead** | The limiter's look-ahead, 0 to 5 ms (default 1.5 ms), the same for both decks. Longer handles peaks more gently. True-peak limiting needs at least 1.0 ms; at 0 ms the limiter works on the sample peak and adds no delay. Changed at once with a short fade. |

### 5.2 Options

- **Deck A and deck B use the same preset** - linked presets (4.5).
- **The outputs follow the Windows volume (volume keys, taskbar) and mute.** The volume of Windows' default playback
  device turns DJAP's outputs down after the processing (never up: the ceiling holds). The meters show the level
  before this volume. If the default playback device is the virtual cable DJAP reads, and Windows turns that cable
  down in software, DJAP undoes that at its input, so the processing still sees the music's own level. One choice per
  mode: on by default with one channel, off with two decks (a key on the keyboard should not move a DJ's outputs).
- **Start DJAP when Windows starts** (2.5). Stored in Windows' list of startup programs, not in the settings file.

### 5.3 Updates

- **Check GitHub for a new version once a day** - on by default.
- **Check now** asks at once.
- **Install update** downloads and installs a newer version (chapter 10). Greyed while the audio runs or when there is
  nothing newer.
- The text beside the buttons shows this version and what the last check found.

### 5.4 Start, stop and state

- **Start audio** starts the audio with the choices above (with WASAPI exclusive it asks first).
- **Stop audio** fades out and stops.
- Under the buttons: the state - the driver or devices, rate, buffer, the latency of input, output and processing,
  and per deck the extra latency and drift of an input on another clock.
- **About DJAP...** shows the version, the settings file in use and the licences.

### 5.5 Health

Counters that show whether the computer keeps up with the audio (chapter 9). **Reset counters** sets them to 0.

## 6. Presets and the preset window

### 6.1 The shipped presets

A preset holds the target loudness and every value of every stage. Five presets are named by strength, three by the
medium they imitate. In the preset lists they appear from the quietest output to the loudest:

| Preset | Output | Character |
|---|---|---|
| **DAB+** | -23 LUFS | Digital radio the EBU R 128 way: the music keeps its dynamics; songs are moved to the target slowly instead of squashed; only passages above -18 LUFS are compressed; no density, clipping or exciter. Quiet next to the others: turn the amplifier up. |
| **Gentle** | -18 LUFS | Easy listening: songs are brought to one loudness slowly and gently, most of the dynamics stay. Light compression, 3 density bands, no clipping. For background music and quiet rooms. |
| **Light** | -16 LUFS | A little more even and present than Gentle, still natural. 4 density bands, the de-esser and a touch of clipping. |
| **Web Stream** | -16 LUFS | Internet radio, podcasts, MP3 / AAC files: like Light but without clipping and with the ceiling at -2 dBTP, because lossy coding raises peaks and turns clipping into audible distortion. |
| **Medium** | -14 LUFS | The usual loudness of streaming services: even and full without sounding squashed. 5 density bands, a little exciter, the de-esser. A good start for most music. |
| **FM Station** | about -13 LUFS | The sound of an FM station: processed like a loud station (fast, dense 5 bands, full low end, slightly wider, hard clipping, highs held down) and then turned down 4 dB, as the transmitter's power limit does. It sounds like FM; it is not a signal ready for a transmitter. |
| **Loud** | -12 LUFS | Dense and punchy: earlier and harder compression, 6 density bands, bass enhancer and exciter, more clipping. For parties and noisy rooms. |
| **Very Loud** | -10 LUFS | Close to modern club masters: very dense and loud, little dynamics left. 7 density bands, the strongest compression and clipping. For loud venues; on long listening it can tire. |

All presets have a limiter ceiling of -1.0 dBTP except Web Stream (-2.0 dBTP).

The main values (`DJAPCmd --presets` lists them for the presets on your computer, including your own):

| Preset | Target LUFS | Leveller range / speed | Compressor threshold / ratio | Density bands | Clipper drive | Ceiling dBTP |
|---|---|---|---|---|---|---|
| DAB+ | -23 | 15 dB / 0.5 dB/s | +5 LU / 2.0 | off | off | -1.0 |
| Gentle | -18 | 12 dB / 0.5 dB/s | +6 LU / 1.5 | 3 | off | -1.0 |
| Light | -16 | 9 dB / 1.0 dB/s | +4 LU / 1.8 | 4 | 1 dB | -1.0 |
| Web Stream | -16 | 9 dB / 1.0 dB/s | +4 LU / 1.8 | 4 | off | -2.0 |
| Medium | -14 | 9 dB / 1.0 dB/s | +2 LU / 2.0 | 5 | 2 dB | -1.0 |
| FM Station | -9 (out -4 dB) | 12 dB / 2.0 dB/s | -3 LU / 3.5 | 5 | 6 dB | -1.0 |
| Loud | -12 | 12 dB / 1.5 dB/s | 0 LU / 2.5 | 6 | 3 dB | -1.0 |
| Very Loud | -10 | 12 dB / 1.5 dB/s | -2 LU / 3.0 | 7 | 4.5 dB | -1.0 |

These values are starting points; change them to taste.

### 6.2 The preset window

![The preset window](images/preset.jpg)

Click **Edit...** next to a deck's preset. At the top:

- **Preset** - the preset shown; choosing another one here also moves the deck it was opened from to it.
- **Save as...** saves the current values under a new name; the deck moves to the new preset.
- **Rename...** and **Delete** - for your own presets only. Deleting a preset moves the decks that used it to Medium.
- **Reset preset** - back to the shipped values (for an own preset: to the general defaults).
- Under it, what the preset does to the sound, and "Some values are changed" when they differ from the shipped ones.

Below that, a **tab per stage**. A stage that is switched off says "(off)" on its tab. On each tab:

- The stage's **On** check box first.
- A row per value: name, **slider**, the **value** (it can be typed - press Enter or leave the field; a comma counts
  as the decimal sign), the unit, a small **reset** button (back to the reset value; only enabled when the value
  differs, and its tooltip names the reset value) and a short explanation. A value in **bold** differs from its reset
  value. The row's tooltip has the full explanation and the range.
- Under the rows, what the stage does to the sound and what more or less of its values sounds like.

Every change is heard at once; a dragged slider is saved when it is let go. Values outside the range are limited to
it; text that is not a value gives a warning. Times and frequencies that span a wide range move on a logarithmic
scale.

### 6.3 All preset values

Defaults below are those of Medium (the general defaults of a new own preset).

**General**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| Target loudness | LUFS | -24 .. -8 | -14 | The loudness every song is brought to (at input gain 0). Higher sounds louder and denser; lower keeps more dynamics. |
| Input gain | dB | -15 .. +18 | 0 | Drives the compressor, clipper and limiter harder: more punch and density; the output gets louder by the same amount. High values squash the sound. |
| Output gain | dB | -15 .. 0 | 0 | A last trim after the limiter, only down, so the ceiling holds. To match another source without changing the sound. |

**Leveller** - the slow hand on the volume.

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Range | dB | 0 .. 15 | 9 | The largest boost or cut. Larger evens out bigger differences between songs. |
| Speed | dB/s | 0.1 .. 6 | 1.0 | How fast the gain may move. Higher reaches a new song's level sooner but can be heard as the volume drifting within a song. |
| Fast down | dB/s | 0 .. 20 | 10 | How fast the gain comes down when a song comes in clearly too loud (after a quiet intro or a break). 0 = the normal speed. |
| Hold below | LU | 5 .. 20 | 10 | Holds the gain while the music is this far under the target: fades, breakdowns and pauses are not pumped up. |
| Catch up after a pause | on / off | | on | After 2 s of silence (a stopped deck) the gain moves to the new song's level within about half a second. |

**Compressor** - evens out the loud parts within a song.

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Threshold | LU | -10 .. +15 | +2 | Relative to the target: material louder than this is compressed. Lower = more compression. |
| Ratio | :1 | 1 .. 8 | 2.0 | How strongly. Higher is more even and more "in your face"; too much sounds flat and tired. |
| Knee | dB | 0 .. 12 | 6 | Width of the soft knee: wider starts more gently. |
| Attack | ms | 1 .. 100 | 10 | Short catches drum hits (less punch); longer lets them through (more punch). |
| Release | ms | 20 .. 1000 | 200 | Short sounds lively but can pump; long is smooth. |
| Side-chain high-pass | Hz | off, 5 .. 200 | 80 | Bass below this does not make the compressor work, so a heavy kick does not pull the whole mix down. |
| Make-up | dB | auto, 0 .. 12 | auto | Gain after compression. Auto keeps the target level unchanged. |

**Density** - the multiband stage.

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Bands | | 1 .. 7 | 5 | Number of frequency bands (1 = wide-band). More bands act more selectively: the bass no longer pushes the voice around. |
| Amount | % | 0 .. 100 | 40 | Strength: from 1:1 at 0 % to 4:1 at 100 %. |
| Down above | dB | 0 .. 12 | 3 | A band this far above its own recent level is turned down. |
| Lift below | dB | 0 .. 20 | 6 | A band this far below its recent level is lifted. |
| Maximum lift | dB | 0 .. 12 | 6 | The largest lift of a band. |
| Attack | ms | 1 .. 200 | 20 | Time to turn a band down. |
| Release | ms | 20 .. 2000 | 300 | Time to turn a band back up. |
| Split 1 .. 6 | Hz | 20 .. 16000 | per band count | The frequencies between the bands. Only the splits of the band count in use are shown; they always rise, at least 1.25x apart (moving one past its neighbour moves the neighbour). A new band count loads its default splits. |
| Band 1 .. 7 gain | dB | -6 .. +6 | 0 | A fixed gain per band: a multiband tone control. |

Default splits per band count: 2 bands 200 Hz; 3: 200 / 2500; 4: 120 / 600 / 4000; 5: 60 / 200 / 2500 / 6000; 6: 60
/ 200 / 800 / 2500 / 6000; 7: 60 / 150 / 400 / 1000 / 2500 / 6000 Hz.

**Bass enhancer**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | off | |
| Frequency | Hz | 60 .. 160 | 100 | Bass below this gets overtones. Higher is more audible but boomier. |
| Amount | % | 0 .. 100 | 30 | Strength of the overtones; too much sounds muddy. |

**Exciter**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Frequency | Hz | 1500 .. 8000 | 3500 | Highs above this are excited. Lower also brightens voices and snares (can turn harsh); higher only adds air. |
| Amount | % | 0 .. 100 | 15 | Strength; too much sounds hissy, especially on old or low-bit-rate recordings. |

**De-esser**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Frequency | Hz | 3000 .. 10000 | 5500 | Where it listens and acts. |
| Threshold | LU | -30 .. 0 | -10 | Relative to the target: sibilance louder than this is reduced (4:1). Lower acts more often. |
| Maximum reduction | dB | 0 .. 12 | 6 | The largest reduction. Too much makes voices lisp and sound dull. |

**Stereo width**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | off | |
| Width | % | 0 .. 200 | 100 | 100 % = as recorded, 0 % = mono, above 100 % wider (can sound hollow and loses power in mono). |
| Bass width | % | 0 .. 100 | 100 | Width of the bass below the split; 0 % = bass in mono. |
| Bass split | Hz | 40 .. 400 | 150 | Where the bass width starts. |

**Soft clipper**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Drive | dB | 0 .. 9 | 2 | How hard peaks are driven into the rounded curve: more loudness and density, less punch; pushed hard it sounds rough. |

**Limiter**

| Value | Unit | Range | Default | What it does |
|---|---|---|---|---|
| On | | | on | |
| Ceiling | dBTP | -6 .. 0 | -1.0 | The highest true-peak level of the output. Lower leaves more room for converters and lossy encoders. |
| Release | ms | 10 .. 500 | 80 | Short recovers fast (louder, can distort the bass); long is cleaner but can duck the sound after a peak. |

Switching the limiter off removes the ceiling guarantee; the output is still clamped at full scale.

### 6.4 Tips

- Start with **Medium** and compare one step up or down; judge at the same listening volume.
- For mixed old and new music, a **colour** with auto tone (Oldies or Auto) usually does more for the balance than
  changing the preset.
- If the volume seems to drift within songs, lower the leveller **Speed**; if new songs take too long to settle,
  raise it (or rely on catch-up after pauses).
- If the sound "breathes" or pumps, lengthen the compressor and density **Release**, or lower the density **Amount**.
- Watch the stage rows: a limiter that reduces more than a few dB all the time means the stages before it push too
  hard (lower input gain, clipper drive or target).

## 7. Colours and the colour window

### 7.1 The shipped colours

A colour holds only the tone. The leveller keeps the loudness on target, so a colour changes the balance, not the
volume.

| Colour | Values | Use |
|---|---|---|
| **Neutral** | everything off | The tone as it is. |
| **Warm** | bass +3, low-mid -1, treble -1.5 dB | Fuller and softer; bright or thin mixes, long listening. |
| **Bass+** | bass +5 dB | More bass, the rest unchanged. |
| **Less mid** | low-mid -3, presence -1.5 dB | Cleaner, less boxy; takes boom and harshness out of live and older recordings. |
| **Bright** | presence +2, treble +3 dB | Clearer and more open for dull recordings (sharp on bright ones). |
| **Vocal** | bass -1, low-mid -2, presence +2.5 dB | Voices forward: speech, vocals, karaoke. |
| **Oldies** | auto tone 70 %, sub bass 20 %, bass +1, treble +1 dB | Thin, flat old recordings of the 60s and 70s. |
| **Auto** | auto tone 100 % | Each song pulled towards a well-balanced production, up to 6 dB (treble 3 dB). |
| **Quiet** | loudness contour 60 % | Listening at a low volume: deep bass and highs added so the music stays full. |

### 7.2 The colour window

![The colour window](images/colour.jpg)

Click **Edit...** next to a deck's colour. It works like the preset window (6.2): Save as, Rename, Delete (own
colours; deleting moves its decks to Neutral), Reset colour, and a tab per part. The colour window has no on / off
switches: a part with all its amounts at 0 is off.

### 7.3 All colour values

| Tab | Value | Unit | Range | Default | What it does |
|---|---|---|---|---|---|
| Tone | Bass | dB | -12 .. +9 | 0 | Low shelf: more or less bass below its corner (0 = off). |
| | Bass corner | Hz | 40 .. 250 | 100 | Where the bass shelf starts. |
| | Low-mid | dB | -9 .. +6 | 0 | Bell: less takes out boom and mud. |
| | Low-mid frequency | Hz | 150 .. 1000 | 350 | Centre of the low-mid bell. |
| | Presence | dB | -9 .. +6 | 0 | Bell: voices and leads forward or back. |
| | Presence frequency | Hz | 1000 .. 6000 | 3000 | Centre of the presence bell. |
| | Treble | dB | -9 .. +9 | 0 | High shelf: more or less highs above its corner. |
| | Treble corner | Hz | 3000 .. 16000 | 8000 | Where the treble shelf starts. |
| Auto tone | Amount | % | 0 .. 100 | 0 | How far each song's balance is pulled towards the reference (0 = off). |
| | Maximum | dB | 0 .. 9 | 6 | The largest correction of a band (treble: half of it - the top of an old recording is often its hiss). |
| | Time | s | 2 .. 60 | 10 | How long a band is measured before it is corrected; faster for the first seconds of a song. |
| Sub bass | Amount | % | 0 .. 100 | 0 | Adds a tone an octave below the bass. Use little: it is felt on large speakers and takes headroom. |
| | Frequency | Hz | 40 .. 120 | 80 | Bass below this is followed an octave lower. |
| Loudness | Amount | % | 0 .. 100 | 0 | Equal-loudness contour for quiet listening: up to +9 dB deep bass and +4 dB highs. Leave it off at normal volume. |

The auto tone holds still during breakdowns and fades, so a quiet part does not change the sound.

## 8. The analysis window

![The analysis window](images/analysis.png)

Click **Graphs** on a deck strip. The window has a row per deck with five panels:

1. **Signal chain** - a row per step in chain order, from *Input* to *Output*. Each step has a lamp (lit when the step
   changes the level by 0.1 dB or more; turquoise = up, orange = down) and a bar against a dB scale from -12 to +12
   (a square-root scale, so small changes are visible). The compressor, de-esser, clipper and limiter show their own
   gain reduction; the other steps show the level after the step against the level before it. *Input* and *Output*
   show the momentary loudness as a bar (-40 .. 0 LUFS) and the peak as a white mark; the output's target is the
   yellow mark. A step that is switched off says "off". **Click a row** to open that step's settings (the preset
   window on its tab, or the colour window).
2. **Compressor** - the compressor's static curve for the preset: output against input, in LU relative to the target
   (the dashed line is 1:1). The white dot is where the detector is now; amber dots show the last 3 seconds.
3. **Loudness** - the momentary loudness of the input (original, orange) and output (processed, turquoise) over the
   last 60 seconds, relative to the target.
4. **Gain** - the leveller, compressor and density gains over the last 60 seconds.
5. **Stereo** - a goniometer of the output (mono is the vertical line, left and right the diagonals), with bars for
   the output's correlation (+1 = mono, 0 = unrelated, negative = out of phase) and the stereo width of input and
   output (0 % = mono).

The history is cleared at every start of the audio.

## 9. Latency and real-time health

**Latency** is the time from the input socket to the output socket. It has three parts:

- the device's input buffer and output buffer (set by the buffer size; ASIO reports them exactly),
- the processing: only the limiter look-ahead (1.5 ms by default; 66 samples at 44.1 kHz),
- with an input on another clock (an ASIO output fed from a WASAPI / DirectSound / MME input): that input's period
  plus the drift buffer, about 10-20 ms.

The Setup window and the status bar show the total.

**Health** (Setup window) shows whether the computer keeps up:

| Counter | Meaning |
|---|---|
| Periods | Audio periods processed since the start (or the last reset). |
| Heaviest / last | The processing time of the heaviest and the last period, in % of the period's length. |
| Over half their time | Periods that needed more than half of their time - a warning sign. |
| Late (dropout) | Periods not ready in time: you hear a dropout. |
| Answered with silence | The engine was not ready (for example while starting). |
| Driver resyncs, overloads | Reported by the ASIO driver. |

If late periods appear, raise the buffer size, close other heavy programs, or use fewer density bands. The processing
of both decks together uses only a few percent of one CPU core.

## 10. Updates

With "Check GitHub for a new version once a day" on (default), DJAP asks the project's public GitHub page about 15
seconds after it starts and then once a day. It only asks; it sends nothing about you or your computer beyond what any
web page request contains.

When a newer version exists, DJAP tells you once with a message at its notification-area icon, in the status bar and
in the Setup window. **Nothing is downloaded or installed until you click Install update** in the Setup window
(clicking the message opens it).

Install update:

1. is only possible while the audio is stopped (an update closes DJAP for a few seconds);
2. downloads the new setup and checks it against the SHA-256 checksum GitHub publishes for it - a setup that does not
   match, or has no checksum, is never run;
3. closes DJAP (with a fade-out) and runs the setup silently for the same folder; Windows may ask for permission;
4. starts DJAP again with your settings. "Start with Windows" stays as you set it.

If something goes wrong the Setup window says what. The setup's log of the last update is in
`%TEMP%\DJAP-update\setup.log`.

## 11. The command-line program DJAPCmd

`DJAPCmd.exe` (in the installation folder) processes files offline with exactly the same code as the live program. It
never plays audio and never changes the settings. Open a command prompt in the installation folder, or give its full
path.

```
DJAPCmd --render <in> <out.wav> [--a <preset|off>] [--b <preset|off>] [--ca <colour>] [--cb <colour>] [--look <ms>] [--keep-delay] [--pcm24]
DJAPCmd --presets
DJAPCmd --measure <file.wav> [more files]
DJAPCmd --tone <file> [more files]
DJAPCmd --devices
DJAPCmd --ini <file> ...     (use another settings file, read only)
DJAPCmd --version | --help
```

- **--render** processes a file. Input: WAV, or MP3 / M4A / WMA / FLAC through Windows Media Foundation (mono plays
  on both channels). A 2-channel file is deck A; a 4-channel file is deck A (channels 1-2) and deck B (3-4). Each deck
  uses its preset and colour from the settings unless `--a` / `--b` (preset, or `off` for unprocessed) and `--ca` /
  `--cb` (colour) say otherwise. The output is 32-bit float WAV (or 24-bit PCM with `--pcm24`), with the look-ahead
  delay removed (kept with `--keep-delay`). After rendering it prints a report of input and output loudness.
- **--presets** lists all presets and colours with their main values and which deck uses them.
- **--measure** prints the integrated loudness, loudness range, maximum momentary and short-term loudness, sample
  peak and true peak of WAV files (per deck for 4-channel files).
- **--tone** prints each file's long-term tone balance (60 Hz, 250 Hz, 3 kHz and 10 kHz against 1 kHz) and what the
  auto tone would correct at 100 %.
- **--devices** lists the ASIO drivers and the WASAPI / DirectSound / MME devices without opening any of them.

Example - process a song with the Loud preset and the Oldies colour into a 24-bit file:

```
DJAPCmd --render song.mp3 song-loud.wav --a Loud --ca Oldies --pcm24
```

Exit code: 0 done, 1 wrong command line, 2 a file could not be read or written.

## 12. The settings file

All settings, the decks' choices and your own presets and colours are kept in one text file, `DJAP.ini`:

- next to the program when that folder is writable (a portable copy),
- otherwise (an installation in Program Files) in `%APPDATA%\DJAP\DJAP.ini`.

The About window shows which file is in use. DJAP saves it after every change. It can be read and edited by hand -
**only while DJAP is closed**. Sections:

| Section | Holds |
|---|---|
| `[Audio]` | Processing (`Decks`), API, driver, devices, channel pairs, rate, buffer, look-ahead, `StartAudio` (start the audio at program start), follow the Windows volume |
| `[Decks]` | Each deck's preset, colour and processing switch, linked presets |
| `[Window]` | The main window's position |
| `[Updates]` | `CheckUpdates` (1 = the daily check is on) |
| `[Preset <name>]` | The values of a preset that differ from its shipped values (own presets: all values) |
| `[Colour <name>]` | The same for a colour |

To start over, close DJAP and delete (or rename) the file; DJAP starts with the shipped presets and opens the Setup
window. Back the file up to keep your own presets.

Settings of versions up to 0.1.57 (which had another program name) are copied once at the first start of DJAP; the
old file stays.

## 13. Troubleshooting

| Problem | What to do |
|---|---|
| **No sound** | Is the audio started (status bar)? Are the right input and output pairs or devices chosen? Is a deck on *(none)*? Do the In bars move? If In moves and Out does not, check the output route and the amplifier. |
| **"Open driver" fails** | Another program (DJ software) may hold the ASIO driver. Close it or use another interface. Check that the interface is connected and its driver installed. |
| **A device shows "(not found)"** | The saved device is not connected, or it was chosen under another API (each API names devices differently). Choose it again. |
| **Dropouts, crackles** | Look at Health: late periods? Raise the buffer size; close heavy programs; set Windows' power plan to high performance; use fewer density bands. With an input on another clock, underruns in the status bar mean the input fell behind: its buffer grows by itself. |
| **The volume keys do nothing** | Switch on "The outputs follow the Windows volume" in the Setup window. |
| **The sound pumps or breathes** | Lower the density amount or the compressor ratio, lengthen their release, or choose a gentler preset. |
| **A loud song starts too loud after a quiet intro** | That is the leveller's 3 s memory; "Fast down" (Leveller tab) brings it down within about a second. Raise it if needed. |
| **Quiet passages come up too much** | Raise "Hold below" or lower the leveller range. |
| **Everything sounds too dull / too sharp** | Try another colour (Bright, Warm), or lower the exciter / de-esser. |
| **Limiter works hard all the time** | Lower the input gain, the clipper drive or the target, or use a quieter preset. |
| **DJAP does not start with Windows** | Check the option in the Setup window and Windows' startup apps list. |
| **The update failed** | The Setup window says why; `%TEMP%\DJAP-update\setup.log` has the setup's log. The setup can always be downloaded from the GitHub page and run by hand. |
| **DJAP closed unexpectedly** | Start `DJAP.exe --diag <file.txt>` from a command prompt; when it happens again, the file holds a report that helps to find the cause. |

## 14. Glossary

| Term | Meaning |
|---|---|
| **LUFS** | Loudness Units relative to Full Scale: loudness measured as the ear hears it (ITU-R BS.1770). -14 LUFS is louder than -18 LUFS by 4 LU. |
| **LU** | Loudness Unit: a difference in loudness; 1 LU = 1 dB. |
| **Momentary / short-term / integrated loudness** | Loudness over 0.4 s, over 3 s, and over a whole programme. |
| **dBFS** | Level relative to digital full scale (0 dBFS = the largest sample value). |
| **True peak, dBTP** | The peak of the waveform between the samples, as a converter or encoder rebuilds it; it can be higher than the highest sample. |
| **Ceiling** | The highest level the limiter lets through. |
| **Look-ahead** | The limiter delays the audio slightly so it sees a peak coming and can turn down smoothly before it arrives. |
| **Threshold, ratio, knee, attack, release** | Where a compressor starts, how strongly it works, how gradually it starts, how fast it reacts and how fast it lets go. |
| **Multiband** | Processing the audio in separate frequency bands. |
| **Bypass** | The deck's input goes out unprocessed. |
| **ASIO** | A low-latency audio driver standard for audio interfaces. |
| **WASAPI** | Windows' own audio system; *exclusive* mode takes a device for one program. |
| **Pre-fader** | A signal taken before the mixer's channel fader, so fader moves do not change it. |
| **Goniometer** | A stereo display: mono is a vertical line, wide stereo a cloud. |
| **Correlation** | How similar left and right are: +1 mono, 0 unrelated, -1 out of phase. |

---

DJAP is free software under the GNU General Public License v3. ASIO is a trademark and software of Steinberg Media
Technologies GmbH.
