# MP3 Player — Analogue Pocket

An MP3 player for the Analogue Pocket. Pick a track or a playlist and the core
decodes and plays it straight off the SD card, with album art, ID3 tags, nine
switchable meters, an eight-preset equalizer and a progress bar.

The decoding is done in software, by a RISC-V CPU built into the Pocket's FPGA
running this project's own firmware.

> **Status: in development (0.1.0).** Playback, seeking, playlists, shuffle and
> repeat, tags, album art, nine meters, the equalizer, the transport and saved
> settings all work on real hardware. Not released yet.

## Installing

Copy the `Cores`, `Platforms` and `Assets` folders onto the root of your
Pocket's SD card, merging with what's already there. Then drop your `.mp3`
files into:

```text
/Assets/mp3player/common/
```

They can live anywhere on the card, but keeping them there puts them next to
the core and makes them quick to find in the file browser.

`mp3player.rom` in that folder is the firmware — the core won't start without
it. That and your music are the only things that belong there; your settings
are kept by the Pocket itself, so there is nothing else to copy or back up.

## Playing

If a playlist is on the card, the core loads its first track at launch and waits
— press **A** to play.

Otherwise press the **Analogue** button and choose **Load MP3** for a single
track or **Load Playlist** for an `.m3u`. The same menu switches tracks or
playlists at any time.

| Pocket | Action |
|---|---|
| **A** | Play / pause |
| **Start** | Stop — returns to 0:00 |
| **Left** / **Right** | *Tap* — previous / next track |
| **Left** / **Right** | *Hold* — seek back / forward |
| **Up** / **Down** | Volume, in 5% steps |
| **B** | Restart the current track from the beginning |
| **X** | Cycle the meter (nine styles) |
| **Select** + **X** | Cycle the meter backwards |
| **Y** | Cycle the EQ preset (eight) |
| **Select** + **Y** | Cycle the EQ preset backwards |
| **Select** | Show / hide the album art panel |
| **L** / **R** | Cycle the accent color (12 shades) |
| **Select** + **L** | Repeat: off → all → one |
| **Select** + **R** | Shuffle on / off |

Track changes and seeking both work while paused or stopped, so you can move
around a playlist without playing anything.

Changing track ends the current one straight away and there is a short silence
before the next begins — the core has to open the new file, read its tag and
decode its artwork before it can play a note. Restarting a track, by contrast,
is immediate, because nothing needs opening.

Left and Right do one thing tapped and another held, and both resolve on
release, so a tap can never also trigger the hold. Select works the same way: a
Select used as a modifier doesn't toggle the art panel.

Volume, accent color, repeat, shuffle, the meter, the art panel and the EQ
preset are all remembered between sessions. Change them with the controls, or
from **Core Settings** in the Analogue menu — either way they persist, and the
two stay in step.

The Pocket stores them itself, so nothing is written to your music folder.

Hiding the album art is a preference, not a per-track state: a track with no
artwork hides the panel without forgetting you want it, so the next track that
has some brings it back.

## Equalizer

**Y** cycles eight presets; **Select**+**Y** goes back. The current one is named
in the mode row, dimmed on `FLAT`.

| | |
|---|---|
| **FLAT** | true bypass — bit-identical to no EQ at all |
| **BASS** | low shelf lift, gentle upper-mid dip |
| **ROCK** | smile curve — lows and highs up, mids back |
| **POP** | presence lift around 2–4 kHz |
| **JAZZ** | warm lows, relaxed upper-mid |
| **CLASSICAL** | gentle warmth, honest mids, eased upper mids, air |
| **VOCAL** | mid forward, lows trimmed |
| **TREBLE** | high shelf lift |

It runs in the FPGA, not on the CPU: five cascaded biquads per channel on one
time-multiplexed multiplier, using 116 of the 1,250 clocks available between
output samples — under 10%. The decoder never knows it is there, so changing
preset is seamless: no gap, no reload, and no interruption to playback.

It works while paused too — the preset changes and the name updates, there is
just nothing to hear until you press play.

Presets are loudness-matched rather than peak-matched, so switching between them
changes the tone without changing how loud the music seems. `FLAT` is a true
bypass — a multiplexer, not a filter set to neutral — so with the EQ off you get
exactly the audio you would get without it.

## Playlists

Make a plain text file with one track per line and save it as `playlist.m3u` in
`/Assets/mp3player/common/`:

```text
Feel Good Inc.mp3
Rhinestone Eyes.mp3
/Music/Albums/Demon Days/01 Intro.mp3
```

Bare names are relative to that folder; a leading `/` is from the root of the
card, so tracks can live anywhere. Lines starting with `#` are ignored, so
playlists exported from other players work as-is.

`playlist.m3u` loads at boot. For any other name — or to switch playlists while
running — press the **Analogue** button and choose **Load Playlist**.

Tracks advance automatically. **Repeat** decides what happens at the end of the
list: off stops, *all* loops, *one* repeats the current track. **Shuffle** plays
everything once before repeating any.

A misspelled or missing filename costs you that one track and nothing else — the
core steps over it and carries on, and says how many it skipped. The track count
on screen counts what will actually play, not how many lines are in the file.

## What it shows

<img src="docs/screenshot.png" width="280" align="right" alt="Now-playing screen: Feel Good Inc. by Gorillaz, track 6 of Demon Days 2005, encoded 128 kbps 44.1 kHz by LAME3.90, above a bar meter with the album cover at the right; below, a PLAYING label with repeat and shuffle indicators and the EQ preset ROCK, track 2 of 5, 02:31 of 03:41, and a progress bar">

- **Title and artist** from the ID3v2 tag, in a real proportional typeface with
  anti-aliased text.
- **Album art** decoded from the tag's embedded image — JPEG only. Tracks with
  no embedded art simply don't show the panel.
- **Nine meters**, cycled with **X** (**Select**+**X** goes back) and remembered
  between sessions:
  - **Bars** — scrolling loudness history with peak-hold markers.
  - **Waterfall** — a strip scrolling left, color tracking loudness, building a
    picture of the track's dynamics.
  - **L/R levels** — horizontal bars per channel.
  - **Phase scope** — a goniometer with fading trails. Vertical is mono, wide is
    a wide mix, horizontal is out of phase.
  - **Oscilloscope** — the waveform itself, from a short window triggered on a
    zero crossing so the trace holds still.
  - **VU** — twin analogue needles with real ballistics, a 100° sweep, and a
    fall to rest when you pause.
  - **Waveform** — a scrolling envelope mirrored about a center line, like a
    DAW's overview of a track.
  - **Mirrored bars** — the bar history grown up and down from the center.
  - **Peak dots** — only the peak markers, tracing the loudness contour.
  None of them is a spectrum: the decoder doesn't expose frequency bins, so
  these show loudness, waveform and stereo rather than frequency content.
- **Elapsed and total time**, with a progress bar.
- **Repeat and shuffle indicators**, dimmed rather than hidden when off, the
  **EQ preset name**, and the track position in the playlist when one is loaded.

- **The encoder that made the file**, on the format line beside the bitrate and
  sample rate — `128 kbps - 44.1 kHz - LAME3.100`. It comes from the LAME tag
  that sits alongside the duration header, so it costs nothing to read. Files
  without that tag simply show the format, as before.

CBR and VBR MPEG-1 Layer III are supported at every standard bitrate and sample
rate, mono or stereo.<br clear="right">

## How it works

There's no MP3 decoder chip in the Pocket, so the FPGA is loaded with a
RISC-V CPU running at 60 MHz and the decoder runs on it as software. Simulation
put the real-time floor around 46 MHz before any hardware was built, which is
where the headroom comes from.

Decoded audio goes into a hardware queue that drains at the file's own sample
rate, so the CPU can spend ~20 ms on a frame without the sound breaking up. The
display works the same way: the CPU sends drawing commands to a framebuffer
engine rather than writing pixels itself, keeping the interface out of the
decoder's way.

Reading files uses Analogue framework commands no core had driven before, for
random reads and for opening a file by name. Making them work meant fixing a
handshake bug — the "command finished" signal stays asserted until the *next*
command starts, so a naive reader sees the previous command's completion and
every read after the first returns nothing.

It also meant finding that the framework keeps its record of every data slot's
size in the same small memory a core uses to talk to it, at the very start. This
core had been writing its own scratch there, so the first track change destroyed
that record — which is what corrupted `.mp3` files whenever settings were saved.
The scratch now lives above it. **Select**+**A** shows that table as the core
saw it at boot against its live value, with a plain intact/clobbered verdict —
a diagnostic, but the one that proves the fix rather than assuming it.
**Select**+**B** dumps the framework's file descriptor for the same reason.

The equalizer is FPGA logic rather than software, sitting between the audio
queue and the DAC. Five biquads per channel share one multiplier, taking 116 of
the 1,250 clocks between output samples, so it costs the decoder nothing.
Its coefficients are generated and checked against a bit-exact model before
anything is compiled, and the hardware is verified sample-for-sample against
that model.

Switching tracks in a playlist leans on the same path: the core asks the
framework to describe the file already in the slot, then hands that description
back with one path component changed. Nothing about the layout is assumed.

## Known limitations

- **Total track time on files with no Xing/Info/VBRI header** is computed from
  the file size and the first frame's bitrate — exact for CBR files, which is
  nearly all of them, approximate for a VBR file whose encoder wrote no header.
- **MPEG-1 Layer III only.** MPEG-2/2.5 low-sample-rate files and Layer I/II
  are not handled.
- **JPEG album art only.** PNG cover art is detected and skipped rather than
  shown wrong. Worth doing if PNG covers turn out to be common; a scan of a real
  library found none, so it is not near the top of
  [ROADMAP.md](ROADMAP.md).
- **Playlists are capped at 128 tracks**, and the file itself at 8 KB.
- **No spectrum display.** The decoder doesn't expose frequency bins, so the
  meters show loudness, waveform and stereo instead.
- **The core never writes to your card.** Not a limitation so much as a
  deliberate property, and worth stating plainly: settings are stored by the
  Pocket itself, and nothing this core does touches your music folder. An
  earlier version saved settings by writing a file, and that write damaged
  `.mp3` files three times before the cause was found — the core was
  overwriting the framework's own record of every file's size. Both are fixed,
  and the write is gone entirely rather than repaired.

## Credits

This core stands on other people's work. Where code is included, the name comes
from that source file's own copyright header.

- **[Helix MP3 decoder](https://github.com/ultraembedded/libhelix-mp3)** —
  © 1995–2002 [RealNetworks, Inc.](https://www.realnetworks.com), released to
  the [Helix Community](https://helixcommunity.org) under the
  [RPSL 1.0](https://helixcommunity.org/content/rpsl). Included in full, under
  its own license, unmodified.
- **[VexRiscv](https://github.com/SpinalHDL/VexRiscv)** soft CPU — Charles Papon
  ([Dolu1990](https://github.com/Dolu1990)) (MIT).
- **[PicoRV32](https://github.com/YosysHQ/picorv32)** — Claire Xenia Wolf
  ([clairexen](https://github.com/clairexen)) (ISC). Used for the early
  feasibility benchmark; not in the shipped design.
- **[minimp3](https://github.com/lieff/minimp3)** —
  [lieff](https://github.com/lieff) (CC0). Benchmarked against Helix and not
  used, but its test vectors were the measurement input.
- **[picojpeg](https://github.com/richgel999/picojpeg)** — Rich Geldreich
  ([richgel999](https://github.com/richgel999)) (public domain).
- **SDRAM controller and i2s audio bridge** — Adam Gastineau
  ([agg23](https://github.com/agg23)) (MIT).
- **[Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/)** — Robert
  Bristow-Johnson. The equalizer's shelf and peaking filter formulas are his;
  the coefficients here are generated from them.
- **[openFPGA framework](https://www.analogue.co/developer)** —
  [Analogue](https://www.analogue.co).
- **[Inter typeface](https://rsms.me/inter/)** — Rasmus Andersson
  ([rsms](https://github.com/rsms))
  ([SIL Open Font License 1.1](https://github.com/rsms/inter/blob/master/LICENSE.txt)).
- **Core, firmware, UI and integration** —
  [HarpMudd](https://github.com/harpmudd).

## About / Support

I'm into retro games and the Analogue Pocket, always cooking up something new.
I love being part of a community built on sharing and the love of games — so if
any of my projects bring you joy, grab me a coffee; it fuels the next thing.

☕ **[buymeacoffee.com/harpmudd](https://buymeacoffee.com/harpmudd)**
