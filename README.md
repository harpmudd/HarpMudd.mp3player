# MP3 Player — Analogue Pocket

An MP3 player for the Analogue Pocket. Pick a track or a playlist and the core
decodes and plays it straight off the SD card, with album art, ID3 tags, nine
switchable meters, an eight-preset equalizer and a progress bar.

The decoding is done in software, by a RISC-V CPU built into the Pocket's FPGA
running this project's own firmware.

> **Version 1.0.0.** Playback, seeking, playlists, shuffle and repeat, tags,
> album art, nine meters, the equalizer, the transport and saved settings are
> all confirmed working on real hardware.

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
| **Y** | Cycle the EQ preset (eight) |
| **Select** | Show / hide the album art panel |
| **L** / **R** | Cycle the accent color (12 shades) |
| **Select** + **L** | Repeat: off → all → one |
| **Select** + **R** | Shuffle on / off |

Track changes and seeking work while paused or stopped. Changing track takes a
moment — the new file has to be opened, its tag read and its artwork decoded
before anything plays; restarting the current one is instant.

Every setting is remembered between sessions: volume, accent color, repeat,
shuffle, the meter, the art panel and the EQ preset. Change them with the
controls or from **Core Settings** in the Analogue menu — they stay in step.
The Pocket stores them itself, so nothing is written to your music folder.

## Equalizer

**Y** cycles eight presets. The current one is named in the mode row, dimmed
on `FLAT`.

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

<img src="docs/screenshot.png" width="280" align="right" alt="Player screen: Feel Good Inc. by Gorillaz, track 6 of Demon Days 2005, encoded 128 kbps 44.1 kHz by LAME3.90, above a bar meter with the album cover at the right; below, a PLAYING label with repeat and shuffle indicators and the EQ preset ROCK, track 3 of 10, 02:31 of 03:41, and a progress bar">

- **Title and artist** from the ID3v2 tag, in a real proportional typeface with
  anti-aliased text.
- **Album art** decoded from the tag's embedded image — JPEG only. Tracks with
  no embedded art simply don't show the panel.
- **Nine meters**, cycled with **X** and remembered between sessions: bars,
  waterfall, L/R levels, phase scope, oscilloscope, twin analogue VU
  needles, scrolling waveform, mirrored bars and peak dots. None is a
  spectrum — the decoder doesn't expose frequency bins, so these show
  loudness, waveform and stereo instead.
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

There's no MP3 decoder chip in the Pocket, so the FPGA is loaded with a RISC-V
CPU and the decoder runs on it as software, with the audio queue and the
equalizer built as hardware around it.

If that sounds interesting, the longer version — including the two Analogue
framework bugs that had to be found first — is in
[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).

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
- **The core never writes to your card.** A deliberate property rather than a
  limitation: settings live with the Pocket, and nothing here touches your music
  folder.

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
  ([rsms](https://github.com/rsms)) — SIL Open Font License 1.1, bundled at
  [`third_party/font/OFL.txt`](third_party/font/OFL.txt). The font ROM the core
  draws with is generated from it and is a derivative under the same license.
- **Core, firmware, UI and integration** —
  [HarpMudd](https://github.com/harpmudd).

Two more shaped the design without ending up in it. Both decided something, which
is why they are credited at all:

- **[minimp3](https://github.com/lieff/minimp3)** —
  [lieff](https://github.com/lieff) (CC0). Measured against Helix and rejected —
  over three times slower, because it is floating point and this CPU has no FPU.
  Its test vectors were the measurement input either way.
- **[PicoRV32](https://github.com/YosysHQ/picorv32)** — Claire Xenia Wolf
  ([clairexen](https://github.com/clairexen)) (ISC). The first CPU tried. It
  needed 114–351 MHz to decode in real time depending on configuration, which is
  what sent the design to VexRiscv.

## License

The code written for this project — the firmware, the RTL, the tools and the
docs — is [MIT licensed](LICENSE).

Everything under `third_party/` keeps its own, and MIT here relicenses none of
it. Two carry real obligations:
[Helix](third_party/libhelix-mp3/docs/RPSL.txt) is RPSL 1.0, a per-file
source-disclosure license, so it is vendored in full and unmodified;
[Inter](third_party/font/OFL.txt) is SIL OFL 1.1, and the font ROM generated
from it is a derivative under the same terms. The rest are MIT, ISC or public
domain — see [Credits](#credits).

## About / Support

I'm into retro games and the Analogue Pocket, always cooking up something new.
I love being part of a community built on sharing and the love of games — so if
any of my projects bring you joy, grab me a coffee; it fuels the next thing.

☕ **[buymeacoffee.com/harpmudd](https://buymeacoffee.com/harpmudd)**
