# MP3 Player — Analogue Pocket

An MP3 player for the Analogue Pocket. Pick a track or a playlist and the core
decodes and plays it straight off the SD card, with album art, ID3 tags, six
switchable meters and a progress bar.

The decoding is done in software, by a RISC-V CPU built into the Pocket's FPGA
running this project's own firmware.

> **Status: in development (0.1.0).** Playback, seeking, playlists, shuffle and
> repeat, saved settings, tags, album art and the UI all work on real hardware.
> It has not been released — see [Known limitations](#known-limitations).

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
it. `settings/settings.bin` holds your volume, colour and playback modes.
Neither is yours to edit, and nothing else belongs in `settings/`.

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
| **X** | Cycle the meter: bars, waterfall, L/R levels, phase scope, oscilloscope, VU |
| **Select** | Show / hide the album art panel |
| **L** / **R** | Cycle the accent colour (12 shades) |
| **Select** + **L** | Repeat: off → all → one |
| **Select** + **R** | Shuffle on / off |

Track changes and seeking both work while paused or stopped, so you can move
around a playlist without playing anything.

Left and Right do one thing tapped and another held, and both resolve on
release, so a tap can never also trigger the hold. Select works the same way: a
Select used as a modifier doesn't toggle the art panel.

Volume, accent colour, repeat, shuffle, the meter style and the art panel are
remembered between sessions, saved to `settings/settings.bin` beside the core.
Writing happens when playback is paused or stopped, or as you open the Analogue
menu, so it never interrupts a track.

Hiding the album art is a preference, not a per-track state: a track with no
artwork hides the panel without forgetting you want it, so the next track that
has some brings it back.

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

## What it shows

<img src="docs/screenshot.png" width="280" align="right" alt="Now-playing screen: title, artist, album and bitrate above a spectrum meter, with cover art at the right; below, a PLAYING label with repeat and shuffle icons, the track position, elapsed and total time, and a progress bar">

- **Title and artist** from the ID3v2 tag, in a real proportional typeface with
  anti-aliased text.
- **Album art** decoded from the tag's embedded image — JPEG only. Tracks with
  no embedded art simply don't show the panel.
- **Six meters**, cycled with **X** and remembered between sessions:
  - **Bars** — scrolling loudness history with peak-hold markers.
  - **Waterfall** — a strip scrolling left, colour tracking loudness, building a
    picture of the track's dynamics.
  - **L/R levels** — horizontal bars per channel.
  - **Phase scope** — a goniometer with fading trails. Vertical is mono, wide is
    a wide mix, horizontal is out of phase.
  - **Oscilloscope** — the waveform itself, from a short window triggered on a
    zero crossing so the trace holds still.
  - **VU** — twin analogue needles with real ballistics, a 100° sweep, and a
    fall to rest when you pause.

  None of them is a spectrum: the decoder doesn't expose frequency bins, so
  these show loudness, waveform and stereo rather than frequency content.
- **Elapsed and total time**, with a progress bar.
- **Repeat and shuffle indicators**, dimmed rather than hidden when off, and the
  track position in the playlist when one is loaded.

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
random reads, opening a file by name and writing one back. Making them work meant
fixing a handshake bug — the "command finished" signal stays asserted until the
*next* command starts, so a naive reader sees the previous command's completion
and every read after the first returns nothing.

Switching tracks in a playlist leans on the same path: the core asks the
framework to describe the file already in the slot, then hands that description
back with one path component changed. Nothing about the layout is assumed.

## Known limitations

- **Files with no Xing/Info/VBRI header show `--:--` for total time** and an
  empty progress bar. Such a file never states its length, and measuring it
  up front meant blocking SD reads that could be heard — the length is learned
  when the end of the file is actually reached instead.
- **MPEG-1 Layer III only.** MPEG-2/2.5 low-sample-rate files and Layer I/II
  are not handled.
- **JPEG album art only.** PNG cover art is detected and skipped.
- **Playlists are capped at 128 tracks**, and the file itself at 8 KB.
- **No spectrum display.** The decoder doesn't expose frequency bins, so the
  meters show loudness, waveform and stereo instead.
- **The saved-settings file lives in its own folder**, `settings/`, and is the
  only thing this core ever writes. It is kept apart from your music
  deliberately: earlier builds wrote it alongside the `.mp3` files and twice
  every file in that folder came back reporting the same wrong size, with the
  audio itself intact — the signature of a damaged directory entry rather than a
  damaged file. Keep `.mp3` files out of `settings/`.

## Credits

This core stands on other people's work. Where a name appears below it came
from the source file's own copyright header.

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
