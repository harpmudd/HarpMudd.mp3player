# MP3 Player — Analogue Pocket

An MP3 player for the Analogue Pocket. Pick a track and the core decodes and
plays it straight off the SD card, with album art, ID3 tags, a spectrum meter
and a progress bar.

The decoding is done in software, by a RISC-V CPU built into the Pocket's FPGA
running this project's own firmware.

> **Status: in development (0.1.0).** Playback, seeking, tags, art and the UI
> all work on real hardware. It has not been released — see
> [Known limitations](#known-limitations).

## Installing

Copy the `Cores`, `Platforms` and `Assets` folders onto the root of your
Pocket's SD card, merging with what's already there. Then drop your `.mp3`
files into:

```text
/Assets/mp3player/common/
```

They can live anywhere on the card, but keeping them there puts them next to
the core and makes them quick to find in the file browser.

`mp3player.rom` in that same folder is the core's firmware. It ships with the
core and the core won't start without it — leave it alone.

## Playing

Launch the core and the Pocket's file browser opens; pick a track and it plays.

To change tracks without leaving the core, press the **Analogue** button to open
the menu, choose **Load MP3**, and pick another file. Playback switches as soon
as you select it.

| Pocket | Action |
|---|---|
| **A** / **Start** | Play / pause |
| **Left** / **Right** | Seek −5 s / +5 s |
| **Up** / **Down** | Volume, in 5% steps |
| **B** | Restart the current track |
| **Select** | Show / hide the album art panel |
| **L** / **R** | Cycle the accent colour (12 shades) |

Volume, accent colour and the art panel's state persist while the core is
running; they reset on relaunch.

## What it shows

<img src="docs/screenshot.png" width="280" align="right" alt="Now-playing screen: title, artist, album and bitrate above a spectrum meter, with cover art at the right and a progress bar below">

- **Title and artist** from the ID3v2 tag, in a real proportional typeface with
  anti-aliased text.
- **Album art** decoded from the tag's embedded image — JPEG only. Tracks with
  no embedded art simply don't show the panel.
- **A spectrum meter** driven by the decoder's own subband data, so it follows
  the actual audio.
- **Elapsed and total time**, with a progress bar.

CBR and VBR MPEG-1 Layer III are supported at every standard bitrate and sample
rate, mono or stereo.

<br clear="right">

## How it works

There's no MP3 decoder chip in the Pocket, so the FPGA is loaded with a small
RISC-V CPU running at 60 MHz, and the decoder runs on it as software. Before any
hardware time was spent, simulation measured the decoder at ~664,000
instructions per frame of audio — putting the real-time floor around 46 MHz, so
60 MHz leaves comfortable headroom. The method and numbers are in
[STAGE0_RESULTS.md](STAGE0_RESULTS.md).

The audio itself never passes through the CPU sample by sample: decoded audio
goes into a hardware queue that drains at the file's own sample rate, which is
what lets the CPU spend ~20 ms decoding a frame without the sound breaking up.

The display is a 400×360 framebuffer in SDRAM with a small drawing engine
alongside it, so the CPU sends commands — fill this rectangle, draw this
character, copy this block — instead of writing pixels itself. That is what
keeps the interface from stealing time the decoder needs.

Reading the file relies on two Analogue framework commands that let a running
core read any part of a user-chosen file. They had never been used before, and
making them work required fixing a subtle handshake bug: the "command finished"
signal is a level that stays asserted until the *next* command starts, so a
naive reader sees the previous command's completion and every read after the
first silently returns nothing. The fix and its regression test are in
[`src/fpga/core/tgt_cmd.v`](src/fpga/core/tgt_cmd.v) and
[`sim/tb_tgt_cmd.v`](sim/tb_tgt_cmd.v).

## Known limitations

- **No playlists.** One track at a time; use **Load MP3** to switch. Playlist
  support is the next planned feature.
- **Total track time can be wrong** on files with no Xing/Info/VBRI header. The
  fallback estimates from a measured byte rate, which is approximate for VBR.
- **MPEG-1 Layer III only.** MPEG-2/2.5 low-sample-rate files and Layer I/II
  are not handled.
- **JPEG album art only.** PNG cover art is detected and skipped.
- Playback state doesn't survive a relaunch.

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
