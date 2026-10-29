# DJAP (DJ Audio Processor) - design v0.2

Status: **approved by the client** (2026-10-24), with the client's later decisions recorded where they apply (CPU budget,
Gentle's leveller range). v0.2: five shipped presets (Gentle .. Very Loud), density stage up to 7 bands with the
band count set per preset.

Names (client, 2026-10-26: "change the name Preset into Loudness", everywhere): the GUI, the plug-in and the manuals
call the dynamics preset the **loudness preset** (its list "Loudness", its window the loudness window) and the colour's
loudness contour **Contour**. This document keeps "preset" for the loudness preset. Ini sections and keys (`[Preset X]`,
`PresetA`, the colour key `Loudness`), the parameter keys (and so the plug-in's parameter ids) and DJAPCmd's options
(`--a <preset>`, `--presets`) stay as they were. The window called "Audio window" here is the **Setup window** in the
program (its right-click menu: Setup / Shutdown). Numbers such as "10.3" or "principle 9" in the Reason columns refer
to the sections of the `audio-processing` handbook (the design basis below, not part of this repository); the
technical manual cites the public sources.

DJAP is a standalone Windows audio processor for DJs: two independent stereo channels (deck A and deck B)
come in on an ASIO® interface (4 inputs), each runs its own dynamics chain, and goes out on 4 outputs. The same DSP
code renders WAV files offline (for tests and checks) and is kept free of Windows / GUI code; it also runs as the
plug-in for DJ software (section 10, since 2026-10-26). Licence GPL-3.0-or-later; the code was written for DJAP with
the AI assistant Claude (one adapted design routine: hiir's halfband designer, `THIRD_PARTY.md`). Design basis: the
`audio-processing` skill's handbook (sections 3-5, 9-12, 16, Appendix B) - the developer's own knowledge base, built
from public standards, papers and measurements, which rules out presets, values or layouts of commercial products -
and the standards / papers it cites (technical manual, chapter 21).

## 1. Requirements

| # | Requirement | Source |
|---|---|---|
| R1 | Deck A (in 1-2 -> out 1-2) and deck B (in 3-4 -> out 3-4), fully independent processing and profiles | client |
| R2 | Live via ASIO; offline WAV in -> WAV out with the same code | client |
| R3 | Low latency: only the limiter's look-ahead adds delay; no other look-ahead, no extra buffering (the minimum-phase filters' group delay at low frequencies is documented in the technical manual, 1.5) | client (hard) |
| R4 | Both decks always have the same latency (they are mixed after the processor) | handbook 10.11 |
| R5 | Win32 GUI + command line | client |
| R6 | DSP core host-independent (the plug-in format the client's DJ software loads, confirmed 2026-10-25; built 2026-10-26, section 10) | client |
| R7 | 44.1 / 48 / 88.2 / 96 kHz; coefficients computed for the actual rate | own |
| R8 | Real-time safe: no allocation, lock, I/O or exception on the audio thread; NaN contained; FTZ/DAZ restored | handbook 11 |
| R9 | Builds without warnings (MSVC /W4, C++17); unit tests with known answers | client |

## 2. Chain per deck

```
deck input (stereo, pre-fader preferred - see 7)
 -> leveller gain + input gain       slow loudness control; its detector sits after width (below)
 -> noise reduction (4 bands or 1)   downward expander, threshold relative to the target (client, 2026-10-29); off in
                                     every shipped preset; before the colour, so a treble boost does not lift the hiss
                                     into its detector
 -> colour (tone, auto tone,         the deck's colour preset (3.3): tone shelves / bells, the auto-tone correction,
    loudness, sub bass)              the loudness contour, the sub-bass generator; before the detector, so the
                                     leveller keeps the loudness on target and only the balance changes
 -> compressor                       wide-band, feed-forward, soft knee, threshold relative to the target
 -> density (1-7 bands up/down)      LR4 cascade with phase-aligning all-passes (flat sum); band count per preset
 -> bass enhancer -> exciter -> de-esser   optional, per preset (Gentle: none; Light: the de-esser; table 3.1)
 -> stereo width (M/S, bass in mono option)
 -> [leveller detector]              measures here: compressor and density work included (handbook 9.2, fault 7);
                                    the leveller's own gain is divided out per control block, so the 3 s average
                                    does not lag its own moves (closed through it, the 20 dB/s catch-up overshot to
                                    the +9 dB range end)
 -> soft clipper                     zero latency; 2x / 4x / 8x oversampling with IIR halfbands (user's choice)
 -> look-ahead true-peak limiter     the only delay (default 1.5 ms)
 -> output gain (only down)
 -> clip guard                       non-finite -> 0, clamp [-1, 1]
 -> [Windows volume]                 only when followed (8): 0..1, glides 20 ms; meters measure before it
deck output
```

Stereo-linked gains everywhere; every stage switchable with a 20 ms ramp; master bypass = the input delayed by the
same look-ahead, bit-exact.

**Changes while playing never click** (measured by `deck_test`, every parameter on a tone in the stage's own band):
gains, make-up (the automatic make-up moves with threshold, ratio and knee), width, bass width, bass amount and
clipper drive follow a 5 ms one-pole; a new density band count or new density splits, and a new width split, run in
a spare filter set that first warms up 20 ms unheard (a filter started cold on running audio starts with a
transient) and then crossfades in over 20 ms with a raised cosine (a linear fade's corner showed on a 120 Hz tone);
a new bass enhancer frequency fades the enhancer out (20 ms), takes the new filters and fades it back in.

## 3. Parameters (per deck, stored in a named preset)

Values are the handbook's tested defaults unless marked **DJ**, where the use differs from a player that has per-song
analysis.

| Stage | Parameter | Unit | Default | Range | Reason |
|---|---|---|---|---|---|
| Global | target loudness T | LUFS | **-14** (per preset, see 3.1) | -24..-8 | **DJ**: dance masters sit at -6..-9 LUFS; -14 leaves 13 dB to a -1 dBTP ceiling and needs little extra mixer gain |
| Gains | input / output | dB | 0 / 0 | -15..+18 / -15..0 | output only down: ceiling holds (9.2) |
| Leveller | range | dB | **12** | 0..15 | **DJ**: no per-song gain in front, the leveller does the equalising (songs differ ~-6..-14 LUFS; hot streamed masters in DJ software need more than 9 dB of cut - client, 2026-10-29: 12 dB in every preset, DAB+ 15) |
| | speed | dB/s | **1.0** | 0.1..6 | **DJ**: faster than 0.5 because nothing levels ahead; still slow enough not to follow phrases |
| | attack | s per 10 dB | **1.0** (10 dB/s, all shipped presets) | 0.2..20 (never slower than the speed) | was "fast down" in dB/s until 2026-10-27 (client: attack and release as sliders). Client, 2026-10-25, after the real-music survey: a song that comes in loud after a quiet intro or a break played up to 9 LU over the target for 30 s at 0.5 dB/s (Gentle, DAB+) and went into the limiter (12.7 dB). When the gain is more than 3 dB above what the 3 s level needs, it comes down at this speed, following the 0.4 s level (the 3 s average lags a loud entry), only down, until within 0.5 dB; then the 3 s level starts from the level reached. Judged on the 3 s level, so a chorus 4 LU over the verse is not touched (`dynamics_test`: identical). Measured (`dynamics_test`, 16 dB entry after a 6 s intro): 4-10 s after the entry -6.0 LUFS without, -17.9 with (target -18); 6 dB/s left +1.9 dB in the first 2 s, 10 dB/s +0.3 |
| | hold below | LU under T | 10 | 5..20 | never pump a fade or a pause (principle 9); a quiet passage that stays under it for the quiet wait without falling (and not more than 12 LU under it) is levelled like any music (client, 2026-10-26: spoken verses 10 LU under the chorus stayed 6-8 LU too soft for 10-19 s) |
| | quiet wait | s | **1** (Gentle, DAB+: 3) | 0.5..10 | how long a steady quiet section waits before it is levelled; was a fixed 3 s until 2026-10-27 (client: "a soft intro sometimes takes too long to get to level"; then "a reasonable average for all the soft-to-loud songs": measured on copies of 18 songs with soft intros (Loud): the time until on level summed 322 -> 249 s, the seconds more than 6 LU too soft 601 -> 497, the steepest rise 3 -> 6.7 dB/s, no song further over the target (+3.6 LU at most, as before); 1 s per 10 dB took 234 s at 10 dB/s) |
| | falling | dB | 2.5 / 2 s; 0.8 + 0.8 / 4 s; 3 LU | fixed | no upward move while the music falls, judged on a trend level (0.4 s, control blocks > 10 dB under the 1.5 s level left out: the gaps between phrases; compared as 1 s averages): dropped > 2.5 dB over 2 s, or sank > 0.8 dB in each of the last two 2 s (a slow fade), kept 1 s; and while the 0.4 s level is > 3 LU under the 3 s level (the first moment of a fade). After a fall the 3 s level restarts from the 1.5 s level. Measured: 40 dB / 10 s fade lifted 0.7 dB, 1 dB/s fade 1.5 dB (4.7 before the trend rule) |
| | warm start | - | on | - | after a pause (>= 1 s of silence) the detector follows a 0.4 s one for 1.5 s, gain held (10.2.3); not after a quiet passage without silence (it restarted after every gap between two phrases) |
| | release | s per 10 dB | **1.5** (6.7 dB/s; Gentle, DAB+: 5) | 0.2..20 (never slower than the speed) | up when the gain is > 4 dB under what the 3 s level needs and the music is not falling (a soft intro, a quiet verse after a loud chorus), until within 1 dB - the counterpart of attack; and the catch-up after a pause upwards. Until 2026-10-27 a fixed "fast up" (4 x speed up to 3 dB/s, or 2 x speed if more: Gentle and DAB+ 2, the others 3, FM Station 4 dB/s) and a catch-up switch at 20 dB/s; measured on copies of 18 songs with soft intros (Loud): the time until on level summed 322 -> 249 s, the seconds more than 6 LU too soft 601 -> 497, the steepest rise 3 -> 6.7 dB/s, no song further over the target (+3.6 LU at most, as before); 1 s per 10 dB took 234 s at 10 dB/s. Gentle and DAB+ keep 2 dB/s (3 brought quiet song endings up too eagerly there) |
| | catch-up | - | always | - | after >= 2 s of silence (deck stopped) the gain moves to the level of the 0.4 s detector - up at the release, down at the attack speed (was a switch at 20 dB/s both ways: down at Gentle's release played a loud song 2 LU too loud, render check); it goes on while that level rose > 1 dB in the last second (a track fading in), at most 15 s, then the 3 s detector starts from the level reached (measured: 12 s fade-in from -50 dB to -7 LUFS ended 5.1 LU too loud without, 0.04 LU with) |
| | freeze | switch | off | - | **DJ**: holds the gain while the DJ works with EQ kills / filters. In the DSP core (`DeckControl::freeze`, for a later plug-in); the program has no button for it since 2026-10-25 (client: the F button "is not needed") |
| Noise reduction | on | switch | **off** (every shipped preset) | - | client, 2026-10-29: "a noise canceller, adjustable". Off by default: music has no noise to take away, and a threshold set for one record dulls the quiet parts of another; existing renders and presets stay as they were |
| | bands | count | 4 | 1 or 4 | client, 2026-10-29: "the three frequencies adjustable too" - four bands, three splits. Rumble / hum, low, mid and hiss each close on their own, so music in one band does not hold the hiss open in another; 1 = one gate for the whole sound (pauses between songs) |
| | threshold | LU re T | -45 | -80..-20 | relative to the target like the compressor's and the de-esser's: after the leveller every song sits there. -45 is about -59 LUFS at Medium - under the quiet passages of music (the leveller's hold level is 10 LU under T) and above the hiss of most old recordings and vinyl transfers (-60..-70 dBFS) |
| | depth | dB | 12 | 0..40 | the largest reduction: 12 dB takes hiss out of hearing in a pause without the "hole" of a hard gate; up to 40 for a gate |
| | ratio | 1:x | 2 | 1.5..10 | downward expansion under the threshold, (x - 1) dB per dB, 6 dB soft knee; 2 is gentle (the band fades as the music fades), 10 nearly a gate |
| | attack / hold / release | ms | 2 / 50 / 250 | 0.5..50 / 0..500 / 20..2000 | a band opens within a few ms (no look-ahead: the first transient after a pause passes nearly whole); the hold keeps it open between notes (no chatter); the release lets a note's tail fade as it would |
| | splits | Hz | 80 / 1000 / 5000 | 20..16000, rising, >= 1.25x apart | rumble and hum under 80 Hz, hiss above 5 kHz (where tape and vinyl noise is heard); LR4 with phase-aligning all-passes (flat sum) like the density bank; a change runs in a spare bank and crossfades (20 ms). Cost with 4 bands about 0.16 % of one core per deck (`deck_test --cpu`). No noise-floor estimate and no signal-to-noise ratio: the user's threshold decides (technical manual 4a) |
| Compressor | threshold | LU re T | +2 | -10..+15 | 10.3 / 9.7 |
| | ratio / knee | - / dB | 2.0 / 6 | 1..8 / 0..12 | 10.3 |
| | attack / release | ms | 10 / 200 | 1..100 / 20..1000 | 10.3, measured step response |
| | side-chain HPF | Hz | 80 | off, 40..200 | bass does not trigger it |
| | make-up | dB | auto | auto, 0..12 | target passes unchanged |
| Density | bands | count | 5 | 1..7 | 1 = wide-band (no split); more bands = each band moves more on its own, less pumping, more CPU |
| | amount | % | 40 | 0..100 | ratio 1:1..4:1 (10.4) |
| | down above / lift below / max lift | dB | 3 / 6 / 6 | 0..12 / 0..20 / 0..12 | 10.4 |
| | attack / release | ms | 20 / 300 | 1..200 / 20..2000 | 10.4 |
| | band gain (one per band, up to 7) | dB | 0 | -12..+12 | multiband tone control at amount 0 (client, 2026-10-26: was -6..+6) |
| | splits (bands - 1 of them) | Hz | per band count, table 3.2 | 20..16000, rising, >= 1.25x apart | 10.4, extended to 7 bands |
| Bass enhancer | frequency / amount | Hz / % | 100 / 30 | 60..160 / 0..100 | 10.6 (Larsen & Aarts) |
| Exciter | frequency / amount | Hz / % | 3500 / 15 | 1500..8000 / 0..100 | 10.7 |
| De-esser | frequency / threshold / max | Hz / LU re T / dB | 5500 / -10 / 6 | 3000..10000 / -30..0 / 0..12 | 10.8 |
| Stereo width | width / bass width / bass split | % / % / Hz | 100 / 100 / 150 | 0..200 / 0..100 / 40..400 | 10.5 |
| | oversampling | x | 4 | 2, 4, 8 | the soft clipper and the exciter's saturation (client, 2026-10-27: "CPU is not a problem, make the oversampling a user's choice"); 2x left about -40 dBc on hard-driven 15 kHz tones, 4x / 8x over 100 dB down; Setup and the plug-in, `[Audio] Oversampling`, `DeckControl::oversampling`; a change fades the two stages out and in |
| Soft clipper | drive | dB | 2 | 0..9 | 10.9; runs at 2x the rate (polyphase IIR halfbands, about 2.6 frames of phase delay, no latency) - its aliases were -16..-44 dBc on 10-15 kHz tones (outside review, client 2026-10-26); the exciter's saturation too |
| Limiter | ceiling | dBTP | -1.0 | -6..0 | R 128 / AES |
| | release | ms | 80 | 10..500 | 10.10 |
| | look-ahead | ms | 1.5 | 0, 1.5, 2, 3, 5 | the only latency; one value for both decks (R4). Above 0 never under the true-peak detector's delay + 18 frames (`lookFrames`: 66 frames, a limiter window of 19 - with 16 full-band noise went 0.10 dB past the margin; the former 0.5 and 1.0 ms fell back to the sample peak without a word, true peaks up to 3 dB over - outside review, client 2026-10-26); 0 = sample peak only |

### 3.1 Presets

A preset holds every value of every stage, the stage switches and the density band count. Each deck chooses its own
preset. Five shipped presets, named by strength (not by genre or situation), from easy listening to very loud. Their
values are **starting points for a listening test**, chosen as a smooth progression: each step is about 2 LU louder,
compresses a little earlier and harder, and uses more bands and more density.

| Preset | Target | Leveller range / speed | Compressor thr / ratio / attack / release | Density bands / amount / max lift | Bass / exciter / de-esser | Clipper drive | Limiter ceiling / release |
|---|---|---|---|---|---|---|---|
| **Gentle** | -18 LUFS | 12 dB / 0.5 dB/s | +6 LU / 1.5 / 20 ms / 300 ms | 3 / 20 % / 3 dB | off / off / off | off | -1.0 dBTP / 120 ms |
| **Light** | -16 LUFS | 12 dB / 1.0 dB/s | +4 LU / 1.8 / 15 ms / 250 ms | 4 / 30 % / 4 dB | off / off / on | 1 dB | -1.0 dBTP / 100 ms |
| **Medium** | -14 LUFS | 12 dB / 1.0 dB/s | +2 LU / 2.0 / 10 ms / 200 ms | 5 / 40 % / 6 dB | off / 15 % / on | 2 dB | -1.0 dBTP / 80 ms |
| **Loud** | -12 LUFS | 12 dB / 1.5 dB/s | 0 LU / 2.5 / 10 ms / 200 ms | 6 / 55 % / 6 dB | 20 % / 20 % / on | 3 dB | -1.0 dBTP / 60 ms |
| **Very Loud** | -10 LUFS | 12 dB / 1.5 dB/s | -2 LU / 3.0 / 5 ms / 150 ms | 7 / 70 % / 8 dB | 30 % / 25 % / on | 4.5 dB | -1.0 dBTP / 50 ms |

Reasons: Gentle sits at the low end of the AES playback range (-16..-20 LUFS) and keeps the music's dynamics (a slow
leveller and a high, gentle compressor). Its leveller range is 12 dB (client, 2026-10-24; was 6): club tracks of
-6..-9 LUFS must come down to -18 LUFS, else a loud track came out louder than in Light (measured with 6 dB: -9 LUFS
in -> -15.3 LUFS out, Light -16.0). Light, Medium and Web Stream have 12 dB too (client, 2026-10-29; was 9): a hot
streamed master played through DJ software held Medium's leveller at -9 dB and still came out above the target. At 0.5 dB/s a large cut takes its time during a song; after a pause the
catch-up (20 dB/s) does it within about half a second. Very Loud approaches modern club masters (-8..-9 LUFS) but keeps 1 dB
of margin so the limiter does not have to carry everything; its release stays at 50 ms or more so a 50 Hz bass
(20 ms period) is not distorted by the limiter's gain following the waveform. Compressor thresholds sit at or near
the target in the louder presets, since the leveller holds the music at the target and higher thresholds are never
reached (handbook 9.7). All other values not in this table follow the defaults in section 3. Ceiling -1.0 dBTP in
all presets (R 128 / AES); a user can change it.

Three more shipped presets (client, 2026-10-25) follow the norms and the sound of a medium rather than a strength.
The preset lists show every preset from the quietest output to the loudest (target + output gain; client,
2026-10-25): DAB+, Gentle, Light, Web Stream, Medium, FM Station, Loud, Very Loud, own presets in their place
(sorted after loading and when one is added; a target changed later moves it at the next start).

| Preset | Target / out | Leveller | Compressor thr / ratio / attack / release | Density | Bass / exciter / de-esser | Clipper | Limiter ceiling / release |
|---|---|---|---|---|---|---|---|
| **FM Station** | -9 LUFS processed, output gain -4 dB: about -13 LUFS out | 12 dB / 2.0 dB/s | -3 LU / 3.5 / 5 ms / 120 ms | 5 / 80 % / lift 8, max 10 dB, 10 / 200 ms | 40 % / 10 % / -14 LU, max 9 dB | 6 dB | -1.0 dBTP (-5 after the gain) / 40 ms |
| **DAB+** | -23 LUFS | 15 dB / 0.5 dB/s | +5 LU / 2.0 / 20 ms / 300 ms | off | off / off / -12 LU | off | -1.0 dBTP / 150 ms |
| **Web Stream** | -16 LUFS | 12 dB / 1.0 dB/s | +4 LU / 1.8 / 15 ms / 250 ms | 4 / 30 % / 4 dB | off / off / on | off | -2.0 dBTP / 100 ms |

Reasons. FM: stations measure about -12 .. -15 LUFS - the multiplex power limit (ITU-R BS.412, average over
60 s) caps the level - while their processing (multiband AGC, limiters, clippers) makes the sound very dense; so the
preset processes like a loud station (-9) and turns the result down 4 dB, leaving peaks only about 8 dB over the
loudness (measured: -13.3 LUFS, -5.1 dBTP). The pre-emphasis (50 us) leaves little room for highs: the de-esser is
set hard as a stand-in for a transmitter's high-frequency limiter, the exciter low; the width stage widens a little
(115 %, bass 50 %). It is the FM sound, not a transmitter-ready signal (no 15 kHz low-pass, no stereo coder, no
composite clipper). DAB+: EBU R 128 (Tech 3344) - programme loudness -23 LUFS, true peak at most -1 dBTP, wide
dynamics; songs are shifted to the target slowly (15 dB range for loud masters) instead of squashed, the compressor
acts only above -18 LUFS (R 128 s1's short-term maximum), no density, no clipping and no exciter (HE-AAC at low bit
rates smears clipping and spends its bits on added highs). Web Stream: internet radio, podcasts and MP3 / AAC files
at -16 LUFS (AES TD1008 for portable listening; music services normalise to -14 .. -16 and turn louder streams
down anyway), like Light but without the clipper and with the ceiling at -2 dBTP, since lossy coding raises the
peaks a little and makes clipping audible. A preset per file format (WAV, FLAC, MP3) was considered and not made:
lossless files need nothing special (any preset at -1 dBTP), lossy ones what Web Stream does.

Users can change every value of any preset, save as a new named preset, rename and delete their own presets, reset
one value or a whole shipped preset to its shipped values (shipped presets cannot be deleted). Changes are saved and
heard at once.

### 3.2 Density band splits per band count

Each band count has its own default split frequencies, spaced roughly evenly on a log scale over the musical
regions (sub, bass, low-mid, mid, presence, air). Changing the band count loads that count's defaults; the user can
then move each split (always rising, at least 1.25x apart; moving one past its neighbour moves the neighbour).
Bands are named "Band 1..N" with their range shown.

| Bands | Splits (Hz) |
|---|---|
| 1 | - (wide-band) |
| 2 | 200 |
| 3 | 200 / 2500 |
| 4 | 120 / 600 / 4000 |
| 5 | 60 / 200 / 2500 / 6000 (handbook) |
| 6 | 60 / 200 / 800 / 2500 / 6000 |
| 7 | 60 / 150 / 400 / 1000 / 2500 / 6000 |

Cost: 6 splits = 12 LR4 filters + 15 phase-aligning all-passes = 39 biquads per channel (5 bands: 22), run for
both channels at once in SSE2 pairs (bit-identical to scalar). Measured (`deck_test --cpu`, one deck Very Loud,
i7-12700F P-core): density 7 bands about 0.3-0.4 % of a core, the whole chain about 1.4-1.5 %.

**Changing the band count while playing** rebuilds the filter bank, whose phase differs per count. To avoid a click
each deck holds two preallocated banks (7 bands each); the new count runs in the spare bank, warms up 20 ms unheard
and the output crossfades from the old to the new bank over 20 ms (raised cosine). A split change does the same and
keeps the band levels and gains. During those 40 ms both banks run (CPU peak).

### 3.3 Colour presets

Client, 2026-10-25: besides the loudness (the preset) a deck gets a **colour**: a second kind of named preset that
holds only the tone of the music - a fixed EQ, an automatic tone correction per song, a sub-bass generator and a
loudness contour. Each deck chooses a preset and a colour; linked decks share both. The colour parameters live in the
same value table as the preset's (index >= `kFirstColourParam`); the ini stores them in `[Colour X]` sections and the
preset sections no longer carry them. The bass level slider (spectrum window and plug-in; on the main window's strips
until 2026-10-26) is the colour's Bass; the bass frequency slider sets the colour's bass corner and the preset's
enhancer frequency and bass split. (The earlier preset value
`BassLevel` is dropped: its job is the colour's Bass now.)

| Stage | Parameter | Unit | Default | Range | Reason |
|---|---|---|---|---|---|
| Tone | bass / corner | dB / Hz | 0 / 100 | -12..+12 / 40..250 | low shelf, Q 0.71: more bass for thin recordings, less for boomy ones; the leveller keeps the loudness, so +9 does not eat headroom |
| | low-mid / frequency | dB / Hz | 0 / 350 | -12..+12 / 150..1000 | bell, Q 0.9: boom and mud of older and live recordings |
| | presence / frequency | dB / Hz | 0 / 3000 | -12..+12 / 1000..6000 | bell, Q 0.9: voices and leads forward or back |
| | treble / corner | dB / Hz | 0 / 8000 | -12..+12 / 3000..16000 | high shelf, Q 0.71: dull or harsh recordings |
| Auto tone | amount | % | 0 | 0..100 | pulls each song's balance towards a reference (below); 0 = off |
| | maximum | dB | 6 | 0..9 | largest correction per band; the treble gets half, since the top of an old recording is often its hiss |
| | time | s | 10 | 2..60 | averaging time of the measurement; 1.5 s during the first 6 s of music, so a new song is corrected soon |
| Sub bass | amount / frequency | % / Hz | 0 / 80 | 0..100 / 40..120 | adds an octave below the bass a recording does not have (60s / 70s records were cut without it) |
| Contour | amount | % | 0 | 0..100 | equal-loudness contour for quiet listening: low shelf 100 Hz up to +9 dB, high shelf 10 kHz up to +4 dB |

**Filters.** Every section is a trapezoidal state-variable filter (Simper) designed at the control rate (16 samples)
and ramped linearly per sample from the old to the new coefficients, so a moving gain or frequency does not zipper
(per-block steps gave a click ratio of 98.9 on a 60 Hz tone; with the ramp below 1). Targets glide with a 25 ms
one-pole; a section is redesigned only when it moved >= 0.005 dB or >= 0.05 Hz. A section that has arrived at 0 dB is
skipped and its state cleared: a neutral colour is bit-exact. Responses match the analog prototypes within 0.0001 dB
(`colour_test`).

**Auto tone.** Five band-passes (Q 1) at 60, 250, 1000, 3000 and 10000 Hz measure the deck's input (mono, after the
input gain) and keep a long-term power average per band. The balance of a band is its level against the 1 kHz band;
the correction of the bass, low-mid, presence and treble sections is clamp((reference - balance) x amount, +-maximum)
and is added to that section's own gain. The averages and the correction hold while the deck is quiet (leveller hold)
or falling (fade), so a breakdown or a fade does not turn the EQ. Reference: the median balance of the late-1980s
dance and pop productions among the client's test music (`DJAPCmd --tone`, 45 songs): +5.9 / +3.4 / 0 /
-1.5 / -5.2 dB at 60 / 250 / 1000 / 3000 / 10000 Hz; the whole set spreads -6.4..+8.2 dB at 60 Hz and -17.5..-3.2 dB
at 10 kHz. No recent club music was in the set; the reference can be recalibrated with `--tone`. Measured corrections
at 100 %: a 1956 rock'n'roll single +6 bass / +3 treble, a 1973 ballad +6 / +3 (limit), a late-80s dance track
-2.3 / +1.2.

**Sub bass.** The deck's bass below the sub frequency (low-pass, two SVF sections) drives a flip-flop that toggles on
every rising zero crossing (10 % envelope hysteresis against noise), which gives a square wave an octave lower; it is
multiplied by the bass's envelope and low-passed at 0.6 x the sub frequency (two sections) to keep only the
fundamental. Measured: 30 Hz out of a 60 Hz tone at -2.3 dB at 100 %, -8.3 dB at 50 %; nothing on silence. Amount and
frequency ramp like the other sections.

**Shipped colours** (starting points for a listening test): Neutral (all off), Warm (bass +3, low-mid -1, treble
-1.5), Bass+ (bass +5), Less mid (low-mid -3, presence -1.5), Bright (presence +2, treble +3), Vocal (bass -1,
low-mid -2, presence +2.5), Oldies (bass +1, treble +1, auto tone 70 %, sub bass 20 %), Auto (auto tone 100 %),
Quiet (loudness 60 %). Managed like presets: save as, rename and delete own colours, reset a value or a shipped
colour. Cost: every colour stage on adds about 0.7 % of a core per deck (`deck_test --cpu`: 3.9 % against 3.2 %).

## 4. True-peak detection in the limiter

The handbook records that a 4x / 16-tap detector let full-band noise through by ~1.4 dB (fault 25). Plan: 8x
oversampling (planned 4x at 88.2 / 96 kHz; built: 8x at every rate), windowed-sinc prototype of odd length centred on a tap, starting
with 32 taps per phase (16 frames of detector delay, 0.36 ms) and going to 48 if the full-band noise test fails.
Built: 96 taps per phase with a Kaiser window (beta 5), plus a 0.1 dB margin at 1.5 ms look-ahead - 32 taps let
full-band noise 1.07 dB over the ceiling; the detector's delay is 48 frames, and with the window of at least 19 frames
the deck never uses less than 66 above 0 (`kMinLookFrames`, deck.h).
The limiter uses the true peak when the look-ahead covers the detector delay, else the sample peak; window
`W = look - D + 1` for a detector of delay D (generalising 10.10). Stated guarantee: sample peak always <= ceiling;
true peak <= ceiling + 0.1 dB for any input when look-ahead >= detector delay.

## 5. Latency budget

| Part | 44.1 kHz | 48 kHz |
|---|---|---|
| Limiter look-ahead 1.5 ms (0..5) | 66 frames | 72 frames |
| Every other stage | 0 | 0 |
| ASIO input + output buffer | driver (e.g. 2 x 64 frames) | |
| Input of another API with ASIO output (6.1) | capture period + drift buffer, about 10 + 19 ms, plus Windows' own audio engine and the devices | |

Shown in the GUI ("added by processing: 1.5 ms"). Changing the look-ahead: fade out 5 ms, reconfigure both decks,
refill, fade in (9.4).

## 6. Threads and data flow

- **ASIO driver thread** (`bufferSwitch` / `bufferSwitchTimeInfo` -> `AudioEngine::process`): converts the 4 input
  buffers from the driver's sample format (all little-endian PCM and float types; big-endian and DSD refused at
  start), runs deck A then deck B in chunks of at most 256 frames through fixed buffers, converts into the 4 output
  buffers (clamped, rounded, no dither), calls `ASIOOutputReady` when the driver supports it. Sets FTZ/DAZ for the
  block and restores the driver's mode. Only the 8 channels in use get buffers; outputs start silent.
- **Start without a click** (client, 2026-10-25: "a hard audio tick after starting"): every live start
  (`AudioEngine::prepare` -> `DeckProcessor::startFadeIn`) fades each deck's output in over 5 ms (the look-ahead
  change's fade); a deck input on another clock fades in over 10 ms (raised cosine) each time its buffer starts
  reading (`AsyncInput::kFadeInMs`: at the start and after an underrun). The program sets the Windows volume on the
  decks before the start at program start (`MainWindow::startSavedAudio`), so they begin at that volume; before, the
  first 50 ms played at 100 % and then glided down. Offline renders and the DSP tests start bit-exact, without fade.
  Stopping the same way (client: "also a short fade-out when the audio stops - Stop, Exit, the setup closing
  DJAP"): before a running stream stops or starts again with other choices (`AudioIO::fadeOutDecks`: Stop, Close
  driver, another API or device, Exit, Windows or a setup ending the program), each deck fades to silence over
  `kFadeOutMs` = 10 ms (raised cosine, `DeckProcessor::startFadeOut`) and stays silent; the GUI waits until every
  running deck reports silence (a deck that processed nothing for 100 ms does not run; at most 300 ms), then 50 ms
  more so the silence passes the device's buffers, then stops. The next start (`prepare`) plays normally.
- **ASIO host** (`AsioHost`, GUI thread): load + init, channel / buffer / rate query, start = set the rate (if the
  driver accepts it, else its own), buffer size (asked, else preferred), create buffers, prepare the engine at that
  rate, publish the decks' configurations, start. Driver messages: reset request and buffer-size change are flags
  that the GUI timer reports once they have stopped for 500 ms (then close and open again); resync and overload
  are counted; latency changes re-read.
- **Settings**: the GUI computes a complete coefficient set per deck and publishes it with a sequence lock (11.3);
  the audio thread takes it at the start of a chunk and ramps.
- **Meters**: per deck an SPSC ring of 10 ms records (K-weighted energy in / out, true peak per channel, gain of each
  stage); the GUI drains at 20 Hz and computes M / S / I / LRA (EBU Tech 3341 / 3342).
- **Health**: heaviest block in % of its period, blocks over half / over their period, shown in the Audio window.
- **Graphs**: per deck an SPSC ring of every frame's input and output (`TapRing`, 16384 frames); the GUI drains it
  at 20 Hz into the spectrum and stereo analyzers (`src/core/analysis.*`). The audio thread only copies four
  floats per frame; every FFT runs on the GUI thread (CPU of the decks unchanged: 3.0 %).
- ASIO COM object created and called on the GUI thread; driver reset request debounced, outputs reopened.

### 6.1 Inputs on another clock (ASIO output)

Client, 2026-10-25: with ASIO, a deck's input may come from a WASAPI, DirectSound or MME device instead of an ASIO
input pair (e.g. a player's output through a virtual cable), while its output stays an ASIO pair. "Deck inputs" in
the Audio window: ASIO input pairs, or the devices of one other API; per deck a device or its ASIO pair. Saved as
`AsioInputs` and `AsioInputDeviceA / B`; WASAPI exclusive applies to these inputs too. Input period (2026-10-28,
seventh outside review: the WASAPI input's 10 ms default period is the largest part of the latency left - about
14 ms of a measured 31.9): `[Audio] InputPeriodMs` 0 (default: Windows' 10 ms) / 8 / 6 / 5 / 4. A shared request goes on the
device's step within its shortest and longest (`SysAudio::onStep`, from IAudioClient3's GetSharedModeEnginePeriod;
eighth review: a request off the step was rounded down under itself and miniaudio kept 10 ms). The mode needs a
device that has it, running at the ASIO rate in Windows; else 10 ms, and the Setup window says so from the measured
input blocks (`AsyncInputState::steadyMs`, also the input's latency shown - the device's reported period can be
shorter than its packets; see Health's notes below). Why a choice and not "the shortest" (2026-10-28, live): the virtual cable allows 1.02 ..
10 ms; "the shortest" took 1 ms and the sound came out distorted. Measured with a 1 kHz tone through the cable's
unused second line (15-30 s per period; residual against a fitted sine per 50 ms): with the cable's timer at 3 ms,
1-2 ms gave duplicated data (-7 .. -10 dB), 3 ms -29 dB, 5 ms 33 of 280 windows bad, 6-7 ms one click, 8-10 ms
clean (-139 dB); with its timer at 1 ms, 2-3 ms 10-20 bad windows, 4-8 ms clean (twice 30 s at 4 ms). So the period
must be 3-4 timer ticks; 2 and 3 ms are not offered. Off by default. WASAPI only (DirectSound / MME have no such mode); in the
Setup window in the drift choice's place, which ASIO does not use.

The two devices run on their own crystals, so their rates differ by tens of ppm (typically up to +-100 ppm): read at
the nominal rate, a buffer between them runs dry or overflows within minutes. `AsyncInput` (`src/core/asyncinput.*`):

- The capture stream (miniaudio, capture-only, at the ASIO rate; miniaudio converts a device that runs at another
  rate) writes stereo blocks with a steady-clock time stamp into a ring of 32768 frames (lock-free, one writer and
  one reader; a block that does not fit is dropped and counted as an overrun).
- The ASIO buffer switch reads it through a 32-tap windowed-sinc interpolator (Kaiser beta 8, cut-off at Nyquist, 256
  tabulated fractional positions, linear in between). Position 0 is a unit impulse, so at a read speed of exactly 1
  the output is the input, delayed, bit-exact. Measured: flat within 0.001 dB to 18 kHz (44.1 and 48 kHz), residual
  below -90 dB to 10 kHz.
- Read speed = 1 + a PI loop on the fill, at most +-0.2 % (3.5 cents). The fill is estimated at the moment of reading
  (last block's end + time since it came, at most one block), so the capture blocks' sawtooth does not modulate the
  speed; smoothed over 0.5 s; error corrected over 4 s, the drift estimate follows over 16 s (critically damped).
  Measured with simulated clocks at -500..+500 ppm and late callbacks (capture up to 2 ms, output up to 0.5 ms):
  drift found within 2 ppm, fill on target within 0.5 ms, read-speed wobble about 15 ppm (0.03 cent), no xruns.
- Target fill = largest capture burst + output burst + 17 frames (half the taps + 1) + margin 3 ms; a burst =
  callbacks within half the last one's length, the largest of the last 20-40 s, at most 50 ms or 1.5 device periods
  (2026-10-27, fourth to sixth outside reviews: underruns after the start; a stall's catch-up kept the buffer up); the
  output's callbacks merge from its first real gap and reading starts after its second (seventh review: the start's
  priming counted as a burst when it came after the first capture block); callbacks before the first gap are not
  remembered (eighth review: a DirectSound stream's first callback is its whole buffer), and an output with no gap
  starts reading after 1 s. The target falls at most 0.3 ms/s. The PI loop's
  integral learns only while the read speed is inside its limits. An underrun fades out what is left (up to 3 ms).
  (Each underrun adds
  2 ms up to 20; each minute without one takes 0.5 ms back - 2026-10-26, after an outside review: the margin only
  grew); about 19 ms for
  10 ms capture blocks and 256-frame periods at 48 kHz. The deck gets silence until the fill is reached (start,
  after an underrun); each underrun adds 2 ms of margin (up to 20 ms). A fill far above target (the reader stalled,
  blocks dropped) moves the read position to the target (a resync).
- Shown: per deck the added latency (capture period + drift buffer), the drift in ppm, underruns and overruns, in
  the Setup window. A capture stream that stops by itself stops the audio with a message.
- Health's notes on the input (`inputNotes` in asyncinput.cpp, a pure function with a table test; tenth outside
  review): the input's period is the *median* finished burst of the last 20-40 s (`steadyMs`; histograms of 0.25 ms
  bins with their sums, so a steady stream reads exactly - the ninth round's minimum let one 16-frame burst show
  0.33 ms for up to 40 s). In shared WASAPI with a period asked for it is compared with the request on the device's
  step and with the device's default period (not a fixed 9 ms: a step of 144 frames made 8 ms 9.00 ms, as the device
  allows, and was reported as Windows' 10 ms blocks): no step fits (another rate in Windows / no low-latency mode /
  the step), the device kept its default, another length than asked, or the device's own step. Exclusive mode gets
  none of this advice. Bursts over 1.5 x the period are named (they set the drift buffer). Blocks read twice: a block
  equal bit for bit to the one before (16+ frames, not flat) starts a run; a run of 1-3 repeats ended by a new block
  counts - a test tone with whole periods per block repeats endlessly and DC or a clipped plateau is flat, neither
  counts (the ninth round's rule counted both, for good). Shown from 3 in the last 20-40 s on, and gone 20-40 s after
  the last.
- The deck's own processing is unchanged; this is the only path where an input adds buffering beyond the devices'
  own, and only by the user's choice.

## 7. DJ-specific notes

- An ASIO driver usually serves one program. With DJ software on the same interface, the standalone app needs the
  signal on separate inputs (e.g. a second interface or external sources). The plug-in route avoids this.
- The deck signal should be **pre-fader** (before the channel fader / crossfader): the leveller then settles while the
  DJ pre-listens, and fader moves are not "corrected". Post-fader works, but the hold rule then carries the fades.
- EQ kills and filter sweeps look like level changes to the leveller; the hold rule catches deep cuts (a freeze switch
  exists in the DSP core; the program does not offer it).

## 8. Modules

| Module | Content |
|---|---|
| `src/core/` (static lib `apcore`, no Windows headers) | biquads, LR4, K-weighting, true-peak detector, loudness meter (M/S/I/LRA), each stage, `DeckProcessor`, params + seqlock, meter ring, profiles (key/value) |
| `src/app/` | Win32 GUI (main window with two deck strips: profile, switches, meters; parameter table per deck; Audio window), ASIO host, ini, WAV reader / writer, CLI |
| `tests/` | one executable per area, own tiny assert harness, `tools/run_tests.ps1` |
| `third_party/asiosdk/` | Steinberg ASIO SDK (GPL-3.0 option), not committed, fetched / copied by a script |

CLI: a separate console program `DJAPCmd.exe` (the GUI program is a Windows-subsystem exe):
`--render in.wav out.wav [--a <preset|off>] [--b <preset|off>] [--look <ms>] [--oversampling <2|4|8>] [--keep-delay] [--pcm24] [--trace <csv>]` (4-channel
WAV = A + B, 2-channel = A; input also MP3 / M4A / WMA / FLAC decoded by Windows Media Foundation, mono on both
channels; default per deck: its preset and processing switch in the settings; output 32-bit float
with the look-ahead delay removed unless `--keep-delay`; `--trace`: deck A every 100 ms from the deck's own meter
records - loudness in / out, momentary and short-term, the leveller gain and state (quiet, frozen, warm), the other
stages' gains), `--presets` (list), `--devices` (ASIO drivers from the
registry, none opened), `--measure <wav...>` (I, LRA, max M / S, sample peak, true peak 16x; per deck for 4
channels), `--tone <files...>` (each file's balance at 60 / 250 / 3000 / 10000 Hz against 1 kHz and the auto
tone's correction at 100 %; calibrates the reference, 3.3), `--ca` / `--cb <colour>` (colour of a deck for
`--render`), `--ini <file>` (other settings, read only). Exit code 0 / 1 wrong command line / 2 file error.

GUI main window (`mainwnd`, `meterview`): two deck strips side by side. Each strip (client, 2026-10-25): on the
title row the route on two lines (client, 2026-10-27: "in and out on 2 lines, on a small screen it does not fit"):
"In: WASAPI: Line 1 (virtual cable)" above "Out: ASIO: outputs 7-8", each side with its API, each line ending in "..."
when too long, right after the deck's name; the Graphs and Bypass buttons at the right (Bypass: green "Bypass Off" while processing, red "Bypass On" while bypassed - client, 2026-10-29; saved as `Processing`);
then the preset choice with its "Edit..." button and the colour choice (3.3) with its "Edit..." button; and a meter panel without buttons (its R - reset meters - and F - freeze leveller - buttons were removed,
client 2026-10-25: "not needed"). With linked presets there is one
set of controls (client, 2026-10-25): deck A's preset and colour, Edit and Bypass
count for both decks; deck B's strip then shows only its route ("linked to deck A") and its meters.
Smallest size (client, 2026-10-26: "the smallest window (one channel) where the spectrum is still visible, fixed;
larger is always allowed"): the height that shows the five stage rows of the meter panel in full and a spectrum whose
dB labels do not touch (`meterViewMinHeight`, from the panel's own layout and fonts at the window's DPI); the width
600 px (one channel) or 900 px (two decks) at 96 DPI, scaled. Switching between one channel and two decks enlarges the
window when it is smaller than the new minimum.
The bass sliders (client, 2026-10-25; on the strips until 2026-10-26, then only in the spectrum window and the
plug-in - client: "now that these are in the big spectrum window, they can go here") change the
colour or the preset, so every deck using it: Bass level (-12..+12 dB, the colour's bass shelf), Bass enhancer
(0..100 %; 0 switches the enhancer off, more switches it on), Bass width (0..100 %, 0 = mono bass; switches the
width stage on), Bass frequency (40..250 Hz: sets the colour's bass corner and, within their ranges, the preset's
enhancer frequency and width split together). Heard while dragging, saved when let go. The stage switches (ten since the noise reduction) are in
the preset window (client, 2026-10-25; they were on the strips). The meter panel (client, 2026-10-25: "only IN L/R and OUT L/R, a dB and a LUFS scale; the R button is not
needed"): LED bars In L / R (sample peak, dBFS, after the preset's input gain - client: "the input slider should move the In bar"; shown by adding the gain to the measurement, which stays the deck's raw input for the offline report) and Out L / R (true peak, dBTP), segments of 5 px lit up to the
value, unlit ones dark in their zone's colour (input: amber from -6, red from -1 dBFS; output: amber in the last
3 dB under the ceiling, red over it) and a peak-hold LED for 1.5 s; across each pair the momentary loudness (white)
and the target (yellow); one axis -40..+3 with a dB scale on the left and a LUFS scale on the right; the gain of each stage (a click on a row opens the preset window on that stage's tab, a click on the In bars the General tab, on the Out bars the Limiter tab - client 2026-10-25; leveller up / down over
its range, compressor / de-esser / clipper / limiter reduction 0..12 dB, the noise reduction's most reduced band
0..24 dB (a row after the leveller, 2026-10-29: the panel one row taller); the density row was removed 2026-10-26 - the density
bands are lines in the spectrum) and the
spectrum; the two lines of numbers (M, S, I, LRA, max true peak, state) were removed at the client's request
(2026-10-25) to give the spectrum room. No menu bar (2026-10-25): a right-click opens Setup / Shutdown.
Status bar (client, 2026-10-27): the Windows volume and the program's CPU use (process time against wall time and
logical processors, over a second - as Task Manager shows it); then, only when present, a device error, a start being
retried, a failed save or a new version. The audio's state (devices, rate, latency, drift, health) is in the Setup
window, which also shows the retried start and a failed save. Per-monitor DPI aware. Every change is heard and
saved at once.

Graphs (client, 2026-10-25). In each meter panel, under the stage rows: the spectrum of the deck's input
(original, orange) and output (processed, turquoise), 20 Hz - 20 kHz on a log scale, 0 .. -96 dB (quieter is not drawn: the line leaves the graph at the bottom - client,
2026-10-27, asked about the highs of a dull recording, kept it so rather than -120 dB or a bent scale), with the
density splits and band numbers and each band's gain now (client, 2026-10-26: the density row went into the
spectrum, which got its room; the band lines in the density colour, like its label, splits and numbers - client,
2026-10-27; first lifted blue, turned down red; a line per band at its gain - of the strip, shade and lines first offered, the client kept only the lines;
the same in the spectrum window; `spectrumdraw.*` draws both).
Spectrum window (client, 2026-10-26: "all together in a separate, big spectrum graph"; `spectrumwnd.*`, a click on
a meter panel's spectrum, WM_COMMAND 105 in GUI tests): one deck's spectrum (left scale), the density bands, and the
deck's colour as an EQ curve (right scale, +-12 dB - client, 2026-10-26: "the scale is not quite right": the two
scales now share their grid, 8 lines of 12 dB on the left, 8 steps of 3 dB on the right, a label on each; `colourCurveDb` / `colourSectionDb` in `colour.*`, the
processing's own section designs, checked against it to 0.01 dB in `colour_test`): thick = now, with the auto
tone's correction (sent in the meter records, `MeterRecord::autoToneDb`), dashed = without it, thin = each section;
the sub bass's range shaded. A point per tone section (bass, low-mid, presence, treble) is dragged in the
parameters' own steps (`paramPositionOf` / `paramValueAt`): x = frequency, y = gain; a double-click sets the gain
to 0 dB; it edits the deck's colour preset like the colour window (heard at once, saved on release). Deck A / B
buttons with two decks; Loudness and Colour lists (client, 2026-10-26: as the main window's lists, both decks when
linked); the four bass sliders in a second row (client, 2026-10-26; `bassctl.*` holds their rule for both
windows: positions, texts, what a position changes), and two of them in the graph (client, 2026-10-26: "something
to drag in the graph"): the colour's bass point drives the sliders' Bass level and Bass frequency (all three
frequencies, as the slider); a bass zone with two bars for the enhancer and the width was tried and removed again
(client, same day: "remove these two"); a read-out of the frequency, levels and colour under the mouse
(on the graph's title row). Client, 2026-10-26 ("make
the tone bands equal to the density bands"; chosen over replacing the colour's sections by per-band gains, which
would tie a colour to a preset's band layout): a handle per density band - its fixed gain (Band N gain, -12..+12 dB)
on the EQ scale over exactly the band's range - dragged up and down (the deck's preset; double-click 0 dB); and
"Snap to bands" (`[Window] SnapToBands`): a dragged colour point within 10 px of a split (and the split within the
section's frequency range) takes the split's frequency exactly. Reset preset / Reset colour buttons (client,
2026-10-26): `Settings::resetPreset` of the deck's preset or colour, after the same question as the preset window's. The plug-in's meter panel
shows the bands the same way (lines); a click on its spectrum opens the Density tab.
The meter panel's spectrum: 4096-point FFT, Hann window, a frame every 2048 samples, power averaged over
0.3 s, shown at 256 log-spaced points (the loudest bin in a point's band; interpolated where the bins are wider
than the points, below about 300 Hz). A sine of amplitude A reads 20 log10(A) dB (on a bin; up to 1.42 dB less
between bins). The Graphs button left of Bypass in each strip (client, 2026-10-25; it replaced the View menu) opens a window with a row per deck - first the signal chain (client, 2026-10-25:
"for every step a bar, to see whether a step does anything; begin and end with an input / output volume bar"): a
row per step in chain order - Input, Leveller (with the input gain), Noise, Colour, Compressor, Density, Bass enhancer,
Exciter, De-esser, Stereo width, Soft clipper, Limiter, Output gain, Output - each with a lamp (lit at 0.1 dB or
more; turquoise up, orange down) and a bar against a scale, no numbers (client: "scale on the bars", then "remove
the numbers"); "off" in the bar of a step that is off. The scale (-12 -6 -1 0 +1 +6 +12 dB; -40..0 for input and
output) goes under every bar when the rows have room, else once above and once below the panel; numbers that would
touch are left out. A click on a row (hand pointer) opens that step's settings (client, 2026-10-25): the deck's preset
window on the stage's tab (Input, Output gain, Output: General), the colour window for Colour. A Noise row after the
Leveller (2026-10-29) shows the noise reduction's most reduced band. Input and output show
the momentary loudness as a bar (-40..0 LUFS) and the peak as a white mark
on the same axis (input: sample peak dBFS, output: true peak dBTP, both held and falling 20 dB/s); the output's
target in yellow. Noise reduction, compressor, de-esser, clipper and limiter show
their own gain reduction (their level change would hide it: make-up, the clipper's drive); the other steps the
level after the step against the level before it (`MeterRecord::chain`, sums of L^2 + R^2 at 13 points of the chain
per 10 ms record, averaged over 0.4 s in the GUI), on a square-root scale (0.5 dB visible, 12 dB full). Cost: about
+0.06 % of one core for two decks (`deck_test --cpu`: 3.09 -> 3.15 %). Then the compressor's static curve for the
preset (output against input, LU to the target, 1:1 dashed) with the detector's level now as a white dot and its last 3 s as amber dots fading with age (client: "the dots in
the compressor output"; a point per GUI refresh, 20 Hz, kept while the window is closed); the last 60 s
of momentary loudness of input and output in LU to the target; the last 60 s of the leveller, compressor and
density (mean of the bands) gains; a scope of the last 2 s (client, 2026-10-27: "an oscilloscope of L and R to
check for clipping, of the input and the output, chosen with a right-click"; then "the sines are too close
together": 1 / 5 / 10 / 20 / 50 / 200 ms of the samples themselves - 50 by default; under 1 sample per pixel a line
through the samples - ending at the newest rising zero crossing of the left channel within the last 50 ms (at least
the span), or 2 s as buckets; `WaveScope` in monitor.h keeps the last 32768 frames and 400 buckets of 2 s with
each channel's lowest / highest sample and the samples at |x| >= 0.9999; linear -1 .. +1, red where they clipped, the
limiter's ceiling for the output; the clip count always over 2 s); a goniometer of the output's last 43 ms (mono vertical, L / R on the
diagonals) with, in the same panel right under it (client: "the three values in the same stereo graph, not
taller"), bars for the output's correlation (0.3 s) and the input's and output's width (side level against mid
level, 0 % = mono). History points every 0.1 s; every start of the audio clears it.

Placement (`src/app/placement.*`). Remembered windows (client, 2026-10-26, replacing "dialogs in the middle of the
application" of 2026-10-25: "when no position is known: centre in the middle of the screen; after that remember every
change of every window - position and size - and write it into the ini file"): the main, Setup, loudness, colour,
analysis, spectrum and About windows each have a line in `[Window]` (`Main=x,y,width,height,maximised`, screen
pixels, the normal rectangle). A window opens at its line's place - with its size when it can be resized - if its
title bar is still on a monitor; else, and without a line, centred on the work area of its owner's monitor (the main
monitor for the main window). Every move, resize, maximise or restore is written at once (a drag once, when it ends;
a subclass of the window watches `WM_EXITSIZEMOVE` and `WM_WINDOWPOSCHANGED`); hiding or minimising is not a change. A
window last maximised opens maximised. The main window's `X` / `Y` of earlier versions become its place. The
plug-in's windows are only centred (it never writes the program's settings). The name prompt and every message box
stay centred on the window that asks (a CBT hook moves the box), kept inside that monitor's work area; while the
main window is minimised or in the notification area its normal position counts.

Look (client, 2026-10-25: "a brushed aluminium background, a random brushed pattern, and 3D buttons"): the main,
preset and Audio windows stand on brushed aluminium (`src/app/theme.*`) - a slightly warm grey (darker at the
client's request: about 152 / 150 / 145 on average, the client's sample), a lighter-to-darker vertical gradient with
a soft sheen in the middle, under a random pattern of fine horizontal strokes (a 1024 x 512 tile from a fixed seed:
per-row grain, strokes of random place, length 16-600 px and strength, soft ends, smeared along the rows; it wraps
without a seam). Each window renders its client area once per size; labels, check boxes and sliders get the same
bitmap as an aligned pattern brush, so the strokes run on behind them. Push buttons are owner-drawn 3D buttons (dark
metal face with a highlight on the upper edge and a shadow below; lighter while the mouse is over them; pushed in
while pressed; Bypass green "Bypass Off" or red "Bypass On" (`theme::setBypass`), About green with an update; grey when disabled; a dotted ring for the keyboard focus). The meter
panels, the analysis graphs and the small name prompt keep their own look. Fields are a light grey in the metal's
tint instead of white (client, 2026-10-25): edit fields and lists through their colour messages, the drop-down
lists and the preset window's tabs drawn by the program (rounded tabs, the chosen one joined to a line under the
strip).

Icon (client, 2026-10-25: a nice icon in the style of the client's own player): a dark brushed-metal tile with rounded corners, a
round dark dial with a light ring and on it a compressor curve - the input in amber going straight up, the output in
teal bending over at the knee towards the ceiling - and three level bars rising at the top right (teal, teal,
amber). The small sizes (16-32 px: the notification area, title bars) are drawn bolder without the bars.
`tools/make_icon.py` writes `assets/DJAP.ico` (16-256 px; bitmaps below 256, PNG at 256) and the setup's
header images; the program, its windows and the setup use it. It also draws the wide logo `assets/logo.png` (4:1:
the icon and the name - "Audio" dark, "Processor" in teal, engraved - on a light brushed plate, over "Loudness and
dynamics for two decks"). About ("About DJAP..." in the right-click menus of the main window and the notification-area icon - a button in the Setup window until 2026-10-27, client; `aboutwnd.*`, client 2026-10-25: the large logo there, as in the client's
player) shows it at the top on the metal, then the version, what the program does, the settings file (one line,
shortened in the middle), the licence and the ASIO notice, and an OK button; modal.

Notification area (client, 2026-10-25: "the application sits in the system tray"): the program shows its icon
there while it runs; the tooltip says whether the audio runs and each deck's preset (and colour, or "bypass").
Closing or minimising the main window hides it with its own windows (Audio, preset, analysis) and the audio goes
on; the first time a notification says so (`TrayHintShown`). A click on the icon shows the window again; its menu:
Open DJAP, Setup..., Shutdown (client, 2026-10-26: "change Exit into Shutdown"). The right-click menu's Shutdown, the icon's Shutdown, the end of the Windows session and setup
(Restart Manager) really close it, saving as before. Only one program runs on the user's own settings: a second
start shows the first one's window and ends (named mutex `DJAP.Running`, which setup also checks; test runs
with `--ini` or `--simulate` are exempt). At the first start - no audio device chosen yet
(`Settings::audioConfigured`) - the Audio window opens by itself, so after the installation the user sets up the
audio first.

Tooltips (client, 2026-10-25): every control in the main window, the Audio window and the preset window says what
it does when the mouse rests on it (`src/app/tooltips.*`, shown for 30 s, wrapped at 440 px). The meter panel has
one tip per part (each bar, each stage row, the numbers); each row of the preset window shows its full explanation
and range, its reset button the reset value. One switch for all of them (client, 2026-10-26: "an option to turn all
tooltips on or off, in Setup"): the Setup window's "Show tooltips", `[Window] Tooltips` (default on);
`Tooltips::setEnabled` sends TTM_ACTIVATE to every tooltip control of the module and to those made later. The
plug-in reads it from the settings file when it is loaded (it never writes it).

Preset window (`editwnd`, "Edit..." beside a deck's preset choice): the preset choice, Save as / Rename / Delete (own
presets only) / Reset preset (shipped values, or the generic defaults for an own preset), and a tab per stage
(client, 2026-10-25: the one long table was hard to read; a stage that is switched off says "(off)" on its tab).
The stage's on / off switch is the first row of its tab, a check box (client, 2026-10-25: it was a bar of nine
buttons above the tabs). Each tab has a row per parameter: name, a
slider, the value (bold when it differs from its reset value; a value can be typed and taken with Enter or by
leaving the field - a comma counts as the decimal sign, "auto" for make-up, "off" for the side-chain high-pass),
the unit, a small reset button (back to the reset value; enabled only when the value differs, its tooltip names
the reset value) and what it does (the full text and the range in the row's tooltip). Switches (the stage's own,
catch-up) are check boxes with a reset button too. Under a tab's rows a short text says what the stage does to
the sound and what more or less of its values sounds like (client, 2026-10-25; `kStageSound` in `editwnd.cpp`).
Every text wraps (client, 2026-10-26: "make all text lines wrappable" - the note was cut off with "..."): the
description and the note under the choice, each row's name and what it does (a row grows with its longest text),
the stage's effect on the sound. Smallest size (client, 2026-10-26, as for the main window): 800 px wide at 96 DPI,
and as high as the tallest tab with all its texts needs at the window's width (`EditWindow::place` measures without
moving; `WM_WINDOWPOSCHANGING` raises a smaller height, so a narrower window, whose texts take more lines, gets
taller), never more than the monitor's work area; a window too small after a change (more density bands) grows.
Under the preset (or colour) choice a line says what the chosen one does to the sound (client, 2026-10-25): a
description per shipped preset and colour (`kShippedSound`), plus "Some values are changed" when they differ from
the shipped ones; an own preset or colour gets a summary of its values (target and stages on; tone gains, auto
tone, sub bass, contour). Sliders move in the parameter's steps; times and frequencies that span a
factor of 8 or more (density splits, releases, attacks) move on a log scale (300 positions, rounded to the step);
make-up has "auto" and the side-chain high-pass "off" at the left end. A change is heard at once; a dragged slider
is saved when it is let go. Values outside the range are clamped, text that is no value gives a warning. Only the
splits and band gains of the
band count in use are listed; a new band count brings its default splits; a split change keeps the splits in
order (>= 1.25x apart). Reset values of splits are the default splits of the band count in use. "Save as" moves
the deck the window was opened from to the new preset; deleting a preset moves its decks to Medium. The colour
window (`editwnd` with the colour kind, "Edit..." beside the colour choice) is the same window for colours: no stage switches, and
deleting a colour moves its decks to Neutral.

Linked presets (client, 2026-10-25): a switch in the Audio window, "Deck A and deck B use the same preset". When
on, deck B takes deck A's preset at once, the main window shows one set of preset controls (deck A's preset
choice, Edit, Bypass; deck B's strip shows only its
route and meters), and that choice (or "Save as") sets both decks; deck B's processing switch follows deck A's
(saved so). Processing / bypass stays per deck. Saved as `LinkPresets`; a
settings file with the link on and two different presets is read with deck B on deck A's preset.

Two decks or one channel (client, 2026-10-25): the Audio window's first choice, "Processing" - two decks (for DJs:
deck A and deck B, each its own input, output and preset) or one channel (one stereo processor, e.g. for what
Windows plays, through a virtual cable or a loopback input). Saved as `Decks` (2 / 1). With one channel deck B is
not part of any route (ASIO: no buffers for its pairs, `AsioChannels` with 0; other APIs: no stream; the engine
skips a deck without inputs and outputs), the Audio window shows "Input" / "Output" without deck B's rows and the
link switch, the main window shows one strip ("Channel") over the whole width, the analysis window one row; linked
presets have no effect. Changing it while the audio runs starts the audio again.

Windows volume (client, 2026-10-25: "the volume keys on my keyboard do nothing"). The keys, the taskbar and the
volume flyout change the volume of Windows' default playback device; Windows applies it only to streams into that
device - not to ASIO, not to the program's output devices - and if it turned down the cable the program reads, the
leveller would bring the level back up (up to its range). So the program follows that volume itself: a switch in
the Audio window, "The outputs follow the Windows volume", one choice per mode (`FollowWindowsVolumeOneChannel`,
default on; `FollowWindowsVolumeTwoDecks`, default off - a key on the keyboard should not move a DJ's outputs).
When on, the GUI reads the default playback device's level (in dB, as Windows or the driver applies it, at most
0 dB) and mute 20 times a second (`src/app/winvolume.*`; the default device is looked up again every second) and
each deck multiplies its output by it after the clip guard (`DeckProcessor::setVolume`, 20 ms one-pole; only down,
so the ceiling holds; volume 1 = bit-exact). The meters and graphs show the processing before it. When the default
playback device is the deck's own input (same device name - a virtual cable whose two ends share a name) and
Windows applies its volume in software (the driver reports no volume of its own), the deck's input is multiplied
by the inverse (at most +40 dB), so the processing sees the music's own level and the volume counts once; a cable
that handles the volume in its driver is not compensated (the program cannot know what that driver does). The
status bar shows "Windows volume N %" or "muted".

Audio APIs (client, 2026-10-25): one API for the whole program (except deck inputs of another API with ASIO
output, 6.1) - ASIO, WASAPI (shared, or exclusive with a switch),
DirectSound or MME. ASIO: one driver, a channel pair per deck input and output, one synchronous buffer switch for
both decks (below). The other APIs (through miniaudio, `src/app/sysaudio.*`): each deck runs from its own input
device to its own output device (Windows shows most interfaces as stereo devices per pair) as one duplex stream
on its own thread; a deck with "(none)" does not run. The decks run at the sample rate chosen; miniaudio converts
when a device runs at another rate or channel count. These paths add the devices' buffering (input period plus
output buffer, shown as an estimate) and, when input and output are different devices, their clocks drift apart:
miniaudio's duplex stream of these synchronous backends has no drift correction, so a growing or emptying backlog
ends in a glitch now and then (outside review). Hence **Drift correction** (`[Audio] DriftCorrection`: 1 = Auto, the
default; 2 = Always; 0 = Off - client, 2026-10-27: "look at this one especially"): a deck is then a capture stream into its `AsyncInput`
and a playback stream that reads it with the drift loop of 6.1 - the same code as the ASIO case, at the cost of its
buffer (input block + output period + 3 ms margin: about 23 ms at 10 ms periods, about 8 ms at 128 frames; the
input block is a burst - callbacks that come straight after each other count as one, so a 10 ms packet cut into
128-frame callbacks or 1024 frames in two callbacks of 512 is held whole). The playback stream of this route has 2
periods with WASAPI (it only reads the buffer), 3 with DirectSound and MME. WASAPI recognises an input and output of
the same physical device by their container id (not the computer's own container, which every built-in and software
device shares) and, on Auto, leaves them the duplex stream: one clock, nothing to correct (fourth outside review);
Always corrects them too, for a device that holds two clocks after all (fifth review). Off: always the duplex
stream. Any other ini value is Auto. The processing itself still adds only the look-ahead; `sysaudio_test` runs both
routes on miniaudio's null devices, Auto and Always for one device (`SysAudio::assumeOneDevice`), and a start with a
device that is not there.

Setup window (`audiodlg`; client 2026-10-25: "all settings in one window, no Audio / Help menus; a right-click on the main window: Exit and Setup" - the main window has no menu bar, a right-click anywhere on it opens Setup... / Shutdown (called Exit until 2026-10-26); the window was the Audio window): limiter look-ahead (0..5 ms, both decks), start with Windows (a check box; greyed in test runs), and the audio API (client 2026-10-27: Start / Stop right under
Look-ahead / Oversampling, the switches below them - the daily update check among them, without a heading - and one
Health section at the bottom: the state text, as high as its lines, then the counters and Reset counters) and, for WASAPI, the exclusive switch. ASIO:
"Deck inputs" (ASIO input pairs, or WASAPI / DirectSound / MME devices, 6.1), the driver (installed drivers from the
registry), Open /
Close driver (opening asks first: it takes the interface from other programs), Control panel, the input and
output channel pair of each deck (with the driver's channel names once it is open; pairs must differ), sample rate
(the driver's own when it refuses the one asked for), buffer size (the driver's preferred, or the sizes its
granularity allows), Start / Stop audio, the latency (input + output + processing) and the health counters
(periods, heaviest / last % of a period, over half, late, answered with silence, driver resyncs and overloads,
reset). Choices are saved at once; while the audio runs, a new device, channel pair, sample rate, buffer size,
deck-input API or the exclusive switch is applied at once by starting the audio again (client, 2026-10-25: a deck
input changed while running had only been saved, so the deck kept recording the old device); `StartAudio` remembers the user's last Start (1) or Stop / Close driver /
other API (0) and starts the audio with the same choices at the next program start (client, 2026-10-25: "the audio
processing must start with the interfaces used last"); a failed start there is tried again every 5 s for 2 minutes
(devices not there yet just after Windows starts) and does not clear `StartAudio`.

Start with Windows (client, 2026-10-25: "the application must start automatically after Windows starts"): Audio ->
Start with Windows (a check mark) writes the value `DJAP` = `"<program>" --autostart` under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, or removes it; checked only when the value names this program
and Windows' startup apps list has not switched it off (switching it on clears that mark). The setup offers the same
(ticked) and its uninstall removes the value. With `--autostart` the program starts hidden in the notification area
(the window only when no audio device has been chosen yet). Greyed out in test runs (`--ini`, `--simulate`). A driver reset request (or buffer-size change) fades a running
stream out, closes, opens and starts the driver again with the same choices; when that fails (the driver not ready
yet) it is tried every second for a minute, the status bar saying so; the user's own Start or Stop ends those tries.
A settings file that cannot be read at the start is tried for about 2 s; then the program runs on its defaults and
saves nothing until it is started again (a message says so) - its first save would have written the defaults over
the user's driver and devices. Start with another driver chosen than the one
that is open closes the open one first (it played on the old driver while the new name was saved). Once the driver is open the window shows the rate the interface runs at: Start
switches the rate only when another one is chosen. Compact (client, 2026-10-25: "less vertical white space"):
rows 6 px apart, each text block as high as its lines (driver info 2, state 3 with one channel or 5 with two decks,
health 3, note 2) and the window as high as its content, so it is lower with one channel than with two decks.

Updates (client, 2026-10-25: "check daily, report, install after one click - not while audio runs - and an off
switch in Setup"; 2026-10-26: "after the online update, start the audio again automatically"; `src/core/release.*`, `src/app/updater.*`): with `[Updates] CheckUpdates=1` (default; Setup ->
Updates) the program asks GitHub's API for the latest release of the public repository 15 s after the start and then
once a day (also Check now in the About window). A newer version (`compareVersions`, numeric parts) is reported once per version
by a balloon at the notification-area icon (a click opens About), in the status bar and in the About window; nothing
is downloaded before the user clicks Install update there (client, 2026-10-27: the update buttons moved from Setup to
About; Setup keeps the daily switch). While the audio runs it asks first (the audio stops during the update); the audio then stops
with the program's usual fade-out, `StartAudio` stays 1, and the restarted program starts it again (`startSavedAudio`,
retried every 5 s for 2 minutes). Not during a simulation.
The setup asset `DJAP-Setup-<version>.exe` is downloaded into `%TEMP%\DJAP-update` and checked against the SHA-256
GitHub publishes for it (the asset's `digest`, else a `<setup>.sha256` asset); without a checksum, or when it does
not match, nothing is installed; a `.sha256` file counts only when it is clear which hash is the setup's (the line
naming it - the name compared whole, after a "*" and a folder - or its only hash), and a download not as long as GitHub says is refused. Where WinHTTP's redirects
ended is checked against the same hosts (GitHub's asset storage). `[Updates] Url` (a local test server) counts only
in a test run (`--ini` / `--simulate`): in the user's own settings it would let anything that can edit the file point
the updater elsewhere. Right before starting it, the
program checks the file once more through a handle that lets nobody change or replace it until the setup runs (the
download folder is the user's %TEMP%). The setup is not signed (no code-signing certificate). Then the program starts the setup with `/SILENT /SUPPRESSMSGBOXES /NORESTART
/UPDATE=1 /DIR=<its folder> /LOG=%TEMP%\DJAP-update\setup.log` (`/CURRENTUSER` when not under Program Files; Windows
asks for permission for Program Files) and ends with the usual fade-out. In update mode the setup waits up to 30 s for
the program's mutex to go, keeps the Run value and skips the Media Feature Pack, and starts DJAP again as the original
user. GitHub's API answers 60 questions an hour per network without a login (every program behind the same router
counts; client 2026-10-27: "403"): when it refuses (403 / 429) the check asks the release's web page
(`github.com/<owner>/<repo>/releases/latest`), which has no such limit and redirects to `.../releases/tag/v<version>`;
the setup and its `.sha256` are then taken from their fixed addresses under it (`releaseFromTagUrl`; no size, so no
progress percentage - the checksum is still required). Only https from github.com, api.github.com and
*.githubusercontent.com is fetched; a test ini may set `[Updates]
Url=http://127.0.0.1:<port>/...`, and that host (only 127.0.0.1 / localhost) is then allowed too, and the restarted
program gets the test ini back (`/RESTARTINI`). The next check removes old setups from the folder (the log stays).
A release is made by `tools/publish.ps1 -Release` (the setup built from the published commit and its `.sha256`). The
setup carries no `.pdb` files (they hold build paths; `make_installer.ps1` keeps them in `release\DJAP-<version>-symbols`)
and the programs are linked with `/PDBALTPATH:%_PDB%` (no build path in the exe).

Presets are stored in `DJAP.ini` next to the exe (fallback `%APPDATA%\DJAP\`; at the first start of DJAP the settings file of versions up to 0.1.57 is copied there once and the old file stays), one section per
preset (`[Preset X]`) and per colour (`[Colour X]`), readable and editable by hand, saved at once after every change.

## 9. Test plan (acceptance)

| Area | Test | Pass |
|---|---|---|
| Loudness | EBU Tech 3341 cases 1-5, 9, 10, 12, 13 and LRA at 44.1 / 48 kHz; silence = -inf | +-0.1 LU; LRA +-1 |
| K-weighting | 44.1 kHz re-rated vs 48 kHz response | 0.005 dB to 16 kHz |
| True peak | cases 15-19 | +0.2 / -0.4 dB |
| Compressor | static curve; step response | exact; 63 % in ~14 ms |
| Limiter | single +12 dBFS samples at look 1.5 / 0.2 / 0.1 / 0 ms; +6 dBFS sine; fs/4 square; +9.5 dBFS full-band noise bursts | sample peak <= ceiling; true peak (16x reference) <= ceiling + 0.1 dB |
| Bypass | master off = input delayed by look-ahead | bit-exact |
| Windows volume | volume 0.5 from the start; input / 4 with an input factor of 4; mute and unmute while playing | half of every sample, bit-exact; the original output, bit-exact; no gain step > 0.01, silent after 0.3 s, bit-exact after unmute |
| Neutral | -6 dBFS sine through a neutral chain | unchanged (< 0.01 dB) |
| Density | band-sum flatness for every band count 1..7; tone two bands away | 0.01 dB; 0.001 dB |
| Noise reduction (`noise_test`) | band sum while open (depth 0; depth 12 with the bands a tone does not reach closed); one band open; pink noise 19 dB under the threshold, depth 12 / 30, 4 bands and 1, and the meter; a 500 Hz tone at -20 dBFS with hiss; a tone stepping -20 -> -80 -> -20 dBFS; each of its values changed on a 1 kHz tone it turns down part of the way; the split rule, off in every shipped preset | 0.001 dB; 0.05 dB (the tone's leak into a closed band); bit-exact; the depth within 0.3 dB, the meter within 0.1; tone within 0.05 dB, hiss above 8 kHz down > 18 dB with 4 bands, unchanged with 1; open through the detector's fall and the hold, 63 % of the depth one release time later, open again within 10 ms; click ratio < 1.5 (measured 1.00); rising, 1.25x apart |
| Band count | switch 3 -> 7 -> 1 bands on a sine while playing | no step larger than the signal's own |
| Presets | all 8 shipped presets render a test playlist; reset restores shipped values; own presets survive a restart | output I within 1 LU of each preset's target at -18 and -9 LUFS in (Gentle also -6); TP <= ceiling; Gentle -> Very Loud rising loudness; FM Station and Web Stream / DAB+ within 1 LU of target + output gain, true peak <= their ceiling |
| Width | left tone at 200 % / 0 %; mono stays mono | L 1.5 / R 0.5; 0.5 / 0.5 |
| Graphs | FFT against a DFT; sines 50 Hz - 18 kHz; correlation and width of mono, opposite, unrelated and half-side signals | 1e-12; 20 log10(A) +0 / -1.5 dB within a bin; +-0.05 / 1 % |
| Colour (3.3) | every tone section against its analog prototype; loudness contour at 100 %; neutral again; auto-tone correction and hold; sub bass on a 60 Hz tone and on silence; changing every colour parameter while playing (`deck_test` clicks) | 0.02 dB; +8.9 dB at 30 Hz, +3.9 dB at 16 kHz; bit-exact; reference - balance, clamped; 30 Hz -2.3 dB at 100 %, nothing; no click |
| Colour stages | 50 Hz, 5 kHz, 7 kHz, 1 kHz selectivity | handbook 10.6-10.8 numbers |
| Robustness | NaN / Inf input; FTZ restored | recovers within a block; MXCSR unchanged |
| Real time | global `operator new` counter during processing, parameter changes, switches, profile changes | 0 allocations |
| Clicks | every parameter change on a sine | no step larger than the signal's own |
| Decks | A processing does not change B; equal latency | bit-exact B; same delay |
| CPU | both decks full chain, 48 kHz | goal < 3 %, fail > 7 % of one core (client, 2026-10-24; was < 1 % / > 2 %, measured 2.8-3.0 %: the deck is latency-bound; fail 5 -> 7 % on 2026-10-27, client: "CPU is not a problem" - 4.3 % with 4x oversampling; an outside review measured 5.1-5.9 % on a busier machine) |
| Offline | render synthetic + copied music, check with `audiotool.py measure / compare` | I within 1 LU of T, TP <= ceiling, bypass bit-exact |
| Input on another clock | simulated clocks -500..+500 ppm, late callbacks, capture pause, output pause; read speed 1; fractional speed 1.0001 with tones; engine with one deck from an async input | no xruns in normal running, drift within 3 ppm, fill within 0.5 ms; recovers; bit-exact; +-0.1 dB to 16 kHz, residual < -85 dB to 10 kHz; 0 allocations |

Not verified by Claude: listening, and any live test on audible outputs (only with the client's OK).

## 10. Plug-in

Client, 2026-10-26: plug-in support for DJ software (R6), in the same setup with a choice of components ("1 setup
with a choice"), a small window of its own at once ("directly a small window"), every value a parameter ("all"),
and the format's name only at the downloads, with its logo ("only plug-in in the texts; VST3 at the downloads and
the logo"). Steinberg's plug-in SDK (v3.8.1, MIT since 3.8) in `third_party/vst3sdk` (not committed;
`tools/get_vst3sdk.ps1`); `src/plugin/`.

- **One deck per plug-in**: stereo in, stereo out (other bus layouts are refused), 32-bit float, any sample rate
  and block size. The audio path is `DeckProcessor::process` on the host's buffers in place - the program's deck,
  bit for bit (`plugin_test`). Activation prepares the deck (allocations happen there, never while processing) and
  fades in over 5 ms like a live start.
- **Processor and controller in one object** (the SDK's single-component effect): the window reads the deck's
  meter and tap rings directly (`DeckMonitor`, `src/core/monitor.*`, shared with the program). Such a plug-in
  cannot be split over two computers by a host; DJ software does not do that.
- **Parameters**: the 83 chain values (72 until the noise reduction, 2026-10-29), each with the program's slider positions as its host steps
  (`paramPositions` / `paramValueAt` / `paramPositionOf` in `params.*`: the same log scale for times and
  frequencies, "auto" / "off" at the left end), grouped by stage (14 units). Ids are a hash of the ini key (`pluginids.h`), so
  values can be added or reordered without breaking a host's project. Four more: the preset list and the colour list
  (the presets and colours of `%APPDATA%\DJAP\DJAP.ini`, or the shipped ones; a last entry "(other values)"; they
  grow and shrink with the preset window, announced with `restartComponent(kParamTitlesChanged)`), Bypass (the
  host's bypass flag: the input delayed by the look-ahead) and the look-ahead (a list of the window's choices 0, 1.5,
  2, 3, 5 ms - so the host shows what runs; an old project's 0.5 or 1.0 ms runs as 1.5 - and the latency reported
  follows). All can be automated.
- **Choosing a preset or colour** (window or host): the controller loads that preset's values, tells the host to
  read them again (`restartComponent(kParamValuesChanged)`) and hands them to the processing as a whole (the
  processor never reads the lists: they change on the GUI thread). Choosing the preset that is already chosen loads
  its values again (back after changes). A value set by the host is the value of its step; the controller keeps the
  exact values of a preset or a project (an 800 Hz split stays 800, not the 810 of the nearest step).
- **Preset window** (client, 2026-10-26: "the edit buttons for preset and colour" were missing; the presets saved in
  the program's settings file, shared): the Edit buttons beside preset and colour, and a click on a row of the meter
  panel, open the program's preset window (`editwnd.*`, the same code, the plug-in's module set with
  `EditWindow::setModule`), owned by the host's top-level window, on a `Settings` of the plug-in's own (one deck).
  A change of the chosen preset's or colour's values reaches the plug-in's parameters (only the values that changed
  in the preset: a bass slider or the host's automation of another value stays) and the processing. Saving is a merge
  (`Settings::saveMerged`): the file is read again, this writer's own additions, changes, renames and deletions since
  its last read or write are applied to it, everything else in it stays - so the program and any number of plug-ins
  keep each other's presets. The plug-in saves as a guest: only the preset and colour lists; the program's other
  settings (devices, window, ...) are never written by it. A preset a deck of the writer uses is kept even when the
  other side deleted it. Nothing new = nothing written (opening the window only reads). Each writer writes through
  a temporary file of its own and renames it.
- **Edits reach the processing directly** (client's DJ program, 2026-10-26: the plug-in processed, but no edit in
  its window changed the sound - the program never passes a parameter change to `process()` for a plug-in whose
  controller and processor are one object; found with a temporary log of the host's calls): every edit in the window
  also hands the controller's whole state to the processing through a second sequence-locked slot. Values the host
  sets on the controller go the same way as long as the host has never delivered a parameter change to `process()`
  itself; once it has, its own sample-accurate changes are used alone, so the controller's later copies never
  overtake its automation.
- **The coefficients** (`makeDeckConfig`) are designed on the audio thread when the host changed a value - no
  allocation, a few microseconds, once per block; the program designs them on its GUI thread.
- **Latency**: the look-ahead in samples, reported to the host (`getLatencySamples`), and a new value announced
  (`kLatencyChanged`) when the look-ahead or the sample rate changes, so the host keeps the decks in time.
- **State** (a project, a host preset): a version, the preset and colour names, look-ahead, Bypass and every value
  by its ini key - the values themselves, so a project sounds the same on a computer where that preset does not
  exist or was changed; unknown keys of a newer version are skipped.
- **Window** (`pluginview.*`, 820 x 640 at 96 DPI, scaled to the host window's DPI): the program's deck strip -
  DJAP and the version, oversampling, look-ahead, Graphs, Bypass, About; preset and colour with their Edit buttons; the four bass sliders (as in the program: bass level
  in the colour, enhancer and width in the preset, bass frequency in both); the meter panel (`meterview.*`, the same
  code). Graphs (client, 2026-10-29: "I miss the Graphs button in the plug-in") opens the program's analysis window
  (`analysiswnd.*`) with one row: it reads its decks through `AnalysisSource` - the program's settings and audio, or
  the plug-in's `DeckMonitor` and parameters; refreshed by the plug-in's 50 ms timer, closed with the plug-in's window. On the brushed metal (`theme.*`); tooltips on every control. Its own common controls 6 through the DLL's
  manifest and `ISOLATION_AWARE_ENABLED`. The window classes and GDI+ live while a window of the DLL is open and are
  removed with the last one (a class left behind would point into an unloaded DLL).
- **About and updates** (client, 2026-10-28: "an About (with the update) in the plug-in" - an installation of the
  plug-in alone has no program to report updates): an About button at the right of the top row opens the program's
  About window (`aboutwnd.*`, plug-in mode: not modal, its own placement `PluginAbout`) with the version, the settings
  file, the licences, the GitHub link and the update check. One `Updater` per DLL (created with the window classes,
  removed with them); it asks GitHub once per load of the DLL, when a window first opens and `[Updates]
  CheckUpdates` in the settings file is on (default; hosts open and close a plug-in often, and GitHub answers 60
  questions an hour per network), and on Check now. A newer version turns the About button into a lit "Update".
  The plug-in never downloads or runs a setup (it lives inside the DJ software, whose module the setup can only
  replace at the next Windows start): Download opens the release's GitHub page, only a `https://github.com/` page.
  `DJAP_NO_UPDATE_CHECK` in the environment stops the automatic check (tests with the editor host).
- **Build**: `src/plugin/CMakeLists.txt` in its own directory, so the SDK's compiler options (`/fp:fast`, `/W3`) do
  not reach the program; the plug-in's own files are compiled with `/W4 /WX /fp:precise`. The SDK is added with
  `EXCLUDE_FROM_ALL`: its validator and editor test host are built only for the tests. The build never copies or
  links anything into the computer's plug-in folder (`SMTG_CREATE_PLUGIN_LINK` off). Output:
  `build/VST3/Release/DJAP.vst3` (a bundle folder: the module, `moduleinfo.json`, the program's icon; the setup
  installs only the module).
- **Setup**: a "Select Components" page - DJAP (the program) and the plug-in, both chosen by default; the plug-in
  only in an installation for all users, as one file in a folder of its own: `{commoncf64}\VST3\DJAP\DJAP.vst3`
  (client, 2026-10-26: the bundle folder `VST3\DJAP.vst3` of 0.1.106 hid the module three folders deep; hosts read
  the single-file form too; the setup removes that old folder, a module still loaded at the next Windows start). Restart Manager closes only DJAP's own
  programs (`CloseApplicationsFilter`); a plug-in module held by DJ software is replaced when Windows starts again
  (`restartreplace`), never by closing that software.
- **Tests**: `plugin_test` loads the built bundle like a host (the SDK's hosting classes) - bit-exact against a
  `DeckProcessor` with the same values for 8 s at block sizes 512 / 333 / 1024 / 64 / 1 with a target change
  half-way; a state stored and loaded into a new instance (83 of 83 values, bit-exact output); Bypass = the input
  delayed by the look-ahead, bit-exact; the latency at 1.5 and 3.0 ms. The SDK's validator: 47 tests, and 537 in
  its extended mode, all passed. The window was driven in the SDK's editor host (no audio): preset, Bypass, a slider;
  the two Edit buttons and a click on the meter's density row open the preset window (on its Density tab) and the
  colour window, and the settings file stays untouched. `settings_test` (`merged`) checks the merge: both sides'
  presets kept, renames and deletions on either side, a preset in use kept, the program's settings never written by
  the guest, no write when nothing is new.
  Not tested by Claude: a real DJ program, an installation into the computer's plug-in folder (needs
  administrator rights).
