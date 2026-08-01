# MP3 Player — Analogue Pocket

A working MP3 player for the Analogue Pocket: pick a track with the Pocket's own
file browser, and the core decodes and plays it straight off the SD card, with
album art, ID3 tags, a spectrum meter and a scrubbing progress bar.

There is no MP3 hardware to emulate here — no arcade board, no MiSTer core to
adapt. The decoder is a real software MP3 decoder running on a RISC-V CPU
synthesised into the Pocket's FPGA, which makes this the first non-arcade build
in this collection and the first to run firmware rather than a ROM.

> **Status: in development (0.1.0).** Playing, seeking, tags, art and the UI all
> work on real hardware. It has not been released — see
> [Known limitations](#known-limitations) before expecting it to behave like a
> finished product.

## Installing

Copy the three top-level folders onto the root of your Pocket's SD card,
merging with what's already there:

```text
/Cores/HarpMudd.Mp3Player/
/Platforms/mp3player.json
/Platforms/_images/mp3player.bin
/Assets/mp3player/common/mp3player.rom
```

`mp3player.rom` is the core's **firmware**, not a game ROM — it is this
project's own compiled code and it ships with the core. The core will not start
without it.

Then put your own `.mp3` files anywhere on the card. On launch the core opens
the Pocket's file browser; pick a track and it plays. `Load MP3` from the
in-game menu switches tracks at any time.

CBR and VBR MPEG-1 Layer III are supported at every standard bitrate and sample
rate, mono or stereo.

## Controls

| Pocket | Action |
|---|---|
| **A** / **Start** | Play / pause |
| **Left** / **Right** | Seek −5 s / +5 s |
| **Up** / **Down** | Volume, in 5% steps |
| **B** | Reload the current track from the top |
| **Select** | Show / hide the album art panel |
| **L** / **R** | Cycle the accent colour (12 shades) |

Volume, accent colour and the art panel's state persist while the core is
running; they reset on relaunch.

## What it shows

- **Title and artist** from the ID3v2 tag (`TIT2` / `TPE2`), rendered in Inter
  with anti-aliased glyphs.
- **Album art** decoded from the tag's embedded `APIC` frame — JPEG only, up to
  384 KB. Tracks with no embedded art simply don't show the panel.
- **A spectrum meter** driven by the decoder's own subband data, so it tracks
  the actual audio rather than an approximation of it.
- **Elapsed and total time**, with a progress bar. Total time comes from the
  Xing/Info/VBRI header when the file has one, and from a measured byte-rate
  when it doesn't.

## The Build

Everything below was written for this core; nothing was adapted from an
existing MP3 implementation, because none exists for this class of hardware.

**The CPU.** A [VexRiscv](https://github.com/SpinalHDL/VexRiscv) RV32IM soft
core at 60 MHz with 256 KB of on-chip RAM. Feasibility was settled before any
hardware time was spent: a cycle-accurate simulation of the decoder against the
worst realistic input (320 kbps @ 48 kHz) measured ~664,000 instructions per
1152-sample frame, putting the real-time floor at ~45.7 MHz. The numbers and
method are in [STAGE0_RESULTS.md](STAGE0_RESULTS.md).

**The decoder.** RealNetworks' [Helix](https://datatype.helixcommunity.org/Mp3dec)
fixed-point MP3 decoder, chosen over the float-based `minimp3` precisely
because a small integer-only soft core would have had to emulate every
floating-point operation in software. Both were benchmarked; Helix won on
measurement, not on reputation.

**Reading the file.** The Pocket's APF framework exposes `target_dataslot_read`
(command `0180`) and `target_dataslot_openfile` (`0192`), which let a running
core read any byte range of a user-chosen file. They are fully implemented in
the APF bridge but had never been driven by any core here — this build is the
first, and it needed a bug fix to use them at all:
`target_dataslot_done` is a **sticky level**, not a pulse, so a naive
implementation sees the *previous* command's completion and every read after
the first returns nothing. [`src/fpga/core/tgt_cmd.v`](src/fpga/core/tgt_cmd.v)
handles the handshake properly and exposes a sequence counter, with a
[regression test](sim/tb_tgt_cmd.v) that reproduces the original failure.

**The display.** A 400×360 RGB565 framebuffer in SDRAM, with a small 2D drawing
engine (filled rectangles, anti-aliased glyph blits, SDRAM→SDRAM copies) so the
CPU issues drawing commands instead of touching pixels. Text is a 4-bit
coverage atlas generated from the Inter typeface by
[`tools/gen_font_rom.py`](tools/gen_font_rom.py), blended against the
background — the reason the type looks like type and not like a bitmap font.

**Album art** is decoded by picojpeg in reduce mode, which produces one pixel
per 8×8 block. A 600×600 cover collapses to ~75×75 — already about panel size —
for a fraction of the work, and it streams row-by-row so the full image is
never held in RAM.

## Building from source

Requires Quartus Prime Lite (for the bitstream) and the xPack
`riscv-none-elf-gcc` toolchain (for the firmware). Firmware-only changes don't
need Quartus at all, which is the point of putting the player in software:

```bash
bash fw/build.sh            # -> dist/Assets/mp3player/common/mp3player.rom
```

The RTL and the firmware are version-interlocked — the firmware refuses to run
against a bitstream whose `CORE_VERSION` it doesn't recognise, so a mismatched
pair fails loudly instead of misbehaving subtly.

## Known limitations

- **No playlists.** One track at a time; use `Load MP3` to switch. Playlist
  support is the next planned feature.
- **Total track time can be wrong** on files with no Xing/Info/VBRI header. The
  fallback measures a byte rate from what has been decoded so far, which is
  approximate for VBR encodes.
- **MPEG-1 Layer III only.** MPEG-2/2.5 low-sample-rate files and Layer I/II
  are not handled.
- **JPEG album art only.** PNG cover art is detected and skipped rather than
  decoded.
- Playback state doesn't survive a core relaunch.

## Credits

This core stands on other people's work. Where a name appears below it came
from the source file's own copyright header.

- **MP3 decoder** — Helix, © 1995–2002 RealNetworks, Inc.
  ([RPSL 1.0](http://www.helixcommunity.org/content/rpsl)). Included in full,
  under its own license, unmodified.
- **VexRiscv soft CPU** — Charles Papon, SpinalHDL project (MIT).
- **PicoRV32** — Claire Xenia Wolf (ISC). Used for the Stage 0 feasibility
  benchmark; not in the shipped design.
- **minimp3** — Lion / lieff (CC0). Benchmarked against Helix in Stage 0; not
  in the shipped design. Its CC0 test vectors were the measurement input.
- **picojpeg** — Rich Geldreich (public domain).
- **APF SDRAM controller and i2s audio bridge** — Adam Gastineau (MIT).
- **openFPGA framework and APF reference core** — Analogue.
- **Inter typeface** — Rasmus Andersson
  ([SIL Open Font License 1.1](https://github.com/rsms/inter/blob/master/LICENSE.txt)).
- **Core, firmware, UI and integration** — HarpMudd.

## About / Support

I'm into retro games and the Analogue Pocket, always cooking up something new.
I love being part of a community built on sharing and the love of games — so if
any of my projects bring you joy, grab me a coffee; it fuels the next thing.

☕ **[buymeacoffee.com/harpmudd](https://buymeacoffee.com/harpmudd)**
