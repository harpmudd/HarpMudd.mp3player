# MP3 Player — Analogue Pocket

An MP3 player for the Analogue Pocket. Pick a track or a playlist and the core
decodes and plays it straight off the SD card, with album art, ID3 tags, nine
switchable meters, an eight-preset equalizer, a progress bar, settings
persistence and resume — it remembers where you were in a playlist.

Decoding runs in software, on a RISC-V CPU built into the Pocket's FPGA.

## Installing

Copy the `Cores`, `Platforms` and `Assets` folders onto the root of your
Pocket's SD card, merging with what's already there. Then drop your `.mp3`
files into:

```text
/Assets/mp3player/common/
```

They can live anywhere on the card; keeping them there makes them quick to find
in the browser.

`mp3player.rom` is the firmware — the core won't start without it. That, your
music and a `playlist.m3u` if you want one are all that belong there.

The Pocket lists the core as **MP3 Player**; open that to launch it.

## Playing

At launch the core looks for **`playlist.m3u`** specifically, and starts
playing it. Other playlists are loaded by name rather than found automatically,
so a card holding only `audiobook.m3u` won't start on its own the first time —
pick it once with **Load Playlist** and it becomes the one that loads at every
launch after that, picking up where you left off.

With no `playlist.m3u` and nothing remembered, you get a short getting-started
screen; press **Analogue** and choose **Load MP3** or **Load Playlist**. The
same menu switches either at any time, and whatever you pick starts playing.

| Pocket | Action |
|---|---|
| **A** | *Tap* — play / pause |
| **A** | *Hold* — 1.2× speed; hold again for normal |
| **Start** | Stop — returns to 0:00 |
| **Left** / **Right** | *Tap* — previous / next track |
| **Left** / **Right** | *Hold* — seek, faster the longer you hold |
| **Select** + **Left** / **Right** | Seek one second |
| **Up** / **Down** | Volume, in 5% steps |
| **B** | Restart the current track from the beginning |
| **X** | Cycle the meter (ten styles) |
| **Y** | Cycle the EQ preset (eight) |
| **Select** | Show / hide the album art panel |
| **L** / **R** | Cycle the accent color (12 shades) |
| **Select** + **L** | Repeat: off → all → one |
| **Select** + **R** | Shuffle on / off |
| **Select** + **Down** | Screen blank: off → 1 → 5 → 10 → 30 min |

The Pocket's own **Controls** screen names every button too, so you don't have
to keep this table to hand.

Track changes and seeking work while paused or stopped. Changing track takes a
moment — the file has to be opened, its tag read and its artwork decoded;
restarting the current one is instant.

Volume, accent color, repeat, shuffle, the meter and the EQ preset are
remembered between sessions, and the controls and **Core Settings** stay in
step. **Where you were in a playlist is remembered too** — the track and your
position in it, so a long listen picks up where it stopped. Turn that off with
**Resume playback** in Core Settings.

It is **one bookmark, not one per playlist**. The core remembers the last
playlist you used and your place in it; switching to another replaces what it
was holding. Leave `audiobook.m3u` partway through, listen to `music.m3u`, and
the audiobook starts again from the beginning next time.

That applies to playlist playback only. A file opened with **Load MP3** plays
without recording a position, which also means a quick listen to something else
won't cost you your place. For an audiobook, put it in a playlist — a one-line
`.m3u` is enough.

The album art panel and the screen-blank timeout are *not* remembered; both
reset each launch and are set with buttons.

The Pocket keeps all of this under `/Settings/HarpMudd.Mp3Player/` on the card;
delete that folder to reset. Nothing is written to your music folder.

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

It works while paused; there is just nothing to hear until you press play.

Presets are loudness-matched, so switching changes the tone without changing how
loud the music seems.

### Playback speed

Hold **A** for 1.2×, hold again for normal. It's meant for spoken word: pitch
rises with the speed, so music sounds wrong. Off every launch — it isn't
remembered.

1.2× is the whole range, and that's the CPU rather than a choice. Playing at
double speed means decoding twice as many frames per second, which needs more
than the 60 MHz available at any bitrate.

### Screen blanking

**Select + Down** cycles the timeout: off, 1, 5, 10, 30 minutes. The screen
goes black after that long with no button pressed. Any button wakes it, and
that press does nothing else — reaching for a sleeping player to see what's on
shouldn't pause it.

Playback carries on while the screen is black, and nothing else brings it
back — track changes, meters and toasts all stay dark until you press
something.

It resets to off each launch, and it dims rather than powers down: the Pocket's
screen is a backlit LCD and a core can't reach the backlight, so this is for
a dark room rather than for saving battery.

## Playlists

Make a plain text file with one track per line and save it as `playlist.m3u` in
`/Assets/mp3player/common/`:

```text
Feel Good Inc.mp3
Rhinestone Eyes.mp3
/Music/Albums/Demon Days/01 Intro.mp3
```

Bare names are relative to that folder; a leading `/` is from the card root.
Lines starting with `#` are ignored, so exported playlists work as-is.

`playlist.m3u` loads at boot. Pick a different one with **Load Playlist** and
that becomes the one it remembers — so `audiobook.m3u` stays loaded across
launches, along with your place in it.

Tracks advance automatically. **Repeat**: off stops at the end, *all* loops,
*one* repeats the current track. **Shuffle** plays in a random order and never
repeats a track until the rest have played; with **Repeat all**, each pass round
the list is freshly shuffled.

A misspelled or missing filename costs that one track — the core steps over it
and says how many it skipped. The count on screen is what will actually play,
not how many lines the file has.

## What it shows

<img src="docs/screenshot.png" width="280" align="right" alt="Player screen: Feel Good Inc. by Gorillaz, track 6 of Demon Days 2005, encoded 128 kbps 44.1 kHz by LAME3.90, above a bar meter with the album cover at the right; below, a PLAYING label with repeat and shuffle indicators and the EQ preset ROCK, track 3 of 10, 02:31 of 03:41, and a progress bar">

- **Title and artist** from the ID3v2 tag, falling back to ID3v1 on older
  files that carry nothing else. A file with no readable tag shows its
  filename, which is usually the song name anyway.
- **Album art** from the tag's embedded image — JPEG only; tracks without it
  don't show the panel.
- **Ten meters**, cycled with **X**: bars, waterfall, L/R levels, phase scope,
  oscilloscope, twin analogue VU needles, scrolling waveform, mirrored bars,
  peak dots and the magic eye — see below.
- **Elapsed and total time**, with a progress bar.
- **Repeat and shuffle indicators**, dimmed rather than hidden when off, the
  **EQ preset name**, and the position in the playlist.
- **The encoder that made the file**, beside the bitrate and sample rate —
  `128 kbps - 44.1 kHz - LAME3.100`. Files without a LAME tag just show the
  format.

CBR and VBR MPEG-1 Layer III at every standard bitrate and sample rate, mono or
stereo.<br clear="right">

### Magic eye

The tenth meter, and the one worth looking for: a pair of **EM84 indicator
tubes**, the bar-type magic eye fitted to tube amplifiers and tape decks either
side of 1960. One tube per channel, left and right, each with a fluorescent
strip that rises with its own signal.

They behave like the valves they are imitating. The strips move on the same
ballistics as the VU needles — quick to rise, slow to fall — and never go fully
dark, because a real tube's heater is always on. Each tube throws light across
the panel beside it, brightest where the pair face each other. The etched scale
follows your accent color; the phosphor stays its own cyan-green, because in
any other color it would just be a bar meter.

## How it works

There's no MP3 decoder chip in the Pocket, so the FPGA is loaded with a RISC-V
CPU and the decoder runs on it as software, with the audio queue and the
equalizer built as hardware around it.

If that sounds interesting, the longer version — including the two Analogue
framework bugs that had to be found first — is in
[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).

## Known limitations

- **Total time is approximate on a VBR file with no Xing/Info/VBRI header** —
  an unusual combination, since VBR encoders normally write one. Everything
  else is exact.
- **MPEG-1 Layer III only.** MPEG-2/2.5 and Layer I/II are not handled.
- **JPEG album art only.** PNG covers are skipped rather than shown wrong —
  see [ROADMAP.md](ROADMAP.md).
- **Playlists are capped at 128 tracks**, or 16 KB of `.m3u` text — whichever
  comes first, which allows about 128 characters per line. A playlist that runs
  past either says so instead of quietly playing fewer.
- **No spectrum display.** The decoder doesn't expose frequency bins, so the
  meters show loudness, waveform and stereo instead.
- **Very occasionally, picking a playlist does nothing.** No loading message,
  no change — the core is never told the pick happened. Pick it again and it
  loads. Rare, and not something a normal session tends to run into.
- **1.2× speed can distort in dense passages.** It needs up to 54.8 MHz of the
  60 available, so the decoder occasionally can't keep up. Normal speed is
  unaffected.

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
  ([richgel999](https://github.com/richgel999)), with changes from Chris
  Phoenix (public domain).
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
