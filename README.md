# MP3 Player — Analogue Pocket

A music player for the Analogue Pocket. It plays MP3 and FLAC straight off the
SD card, with album art, tags and meters.

Decoding runs in software, on a RISC-V CPU built into the Pocket's FPGA.

Release history: [CHANGELOG.md](CHANGELOG.md).

## Quick start

1. Copy the `Cores`, `Platforms` and `Assets` folders to the root of your SD
   card, merging with what's there.
2. Put some `.mp3` or `.flac` files in `/Assets/mp3player/common/`. Subfolders
   are fine.
3. Open the core, press **Analogue**, and choose **Load MP3**. It starts
   playing.

Four controls cover everything you need:

| Pocket | Action |
|---|---|
| **A** | Play / pause |
| **Left** / **Right** | Previous / next track — *hold* to seek within one |
| **Up** / **Down** | Volume |
| **Analogue** | Load a different track or playlist |

That's the whole player. Everything below is optional: meters on **X**, the
equalizer on **Y**, playlists, resume, and the settings in the Analogue menu.
Come back for them when you want them — none of it needs setting up first.

## Installing

Copy the `Cores`, `Platforms` and `Assets` folders onto the root of your
Pocket's SD card, merging with what's already there. Then drop your `.mp3` and
`.flac` files into:

```text
/Assets/mp3player/common/
```

They can live in subfolders under that path — an `Artist/Album` layout works
without rearranging. `mp3player.rom` is the firmware and has to stay in that
folder — the core won't start without it.

## Playing

At launch the core loads **`playlist.m3u`** — that name specifically, not any
playlist it finds. Choose a different one with **Load Playlist** and it becomes
the one that loads from then on, so you only have to pick it once.

With no `playlist.m3u` and nothing remembered you get a getting-started screen;
press **Analogue** and choose **Load MP3** or **Load Playlist**. The same menu
switches either at any time, and whatever you pick starts playing.

The controls:

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
| **Select** | *Tap* — playlist; *Hold* — show / hide the album art panel |
| **L** / **R** | Cycle the accent color (12 shades) |
| **Select** + **L** | Repeat: off → all → one |
| **Select** + **R** | Shuffle on / off |
| **Select** + **Down** | Screen blank: off → 1 → 5 → 10 → 30 min |

Track changes and seeking work while paused or stopped. Changing track takes a
moment — the file has to be opened, its tag read and its artwork decoded;
restarting the current one is instant.

Volume, accent color, repeat, shuffle, the meter and the EQ preset carry over
between sessions, in step with **Core Settings**. **Where you were in a
playlist can be remembered too** — switch on **Resume playback** in Core
Settings. It holds one place, for the last playlist you used, and **Load MP3**
records no position at all — so for an audiobook, use a playlist; a one-line
`.m3u` is enough.

The album art panel and screen-blank timeout reset each launch. Everything
saved lives in `/Settings/HarpMudd.Mp3Player/` — delete that folder to reset.
Nothing is written to your music folder.

## Playlists

A plain text file with one track per line, saved as `playlist.m3u` in
`/Assets/mp3player/common/`:

```text
Feel Good Inc.mp3
Rhinestone Eyes.mp3
Demon Days/01 Intro.mp3
```

Names are relative to the folder the playlist is in, so a playlist can sit
beside its tracks in an album folder or in `common/` naming tracks below it.
Either works, so an `Artist/Album` library needs no rearranging. Lines starting
with `#` are ignored, so exported playlists work as-is.

Any other filename is picked with **Load Playlist**, and becomes the one that
loads at launch from then on — provided its name is short enough to be
remembered.

### Limits

| | Limit | What happens past it |
| --- | --- | --- |
| Tracks per playlist | 256 | Says how many were dropped |
| `.m3u` file size | 12 KB | Same — about 48 characters per line at 256 tracks |
| Remembered playlist name | any length, with `playlists.m3u` — otherwise 12 characters | Falls back to `playlist.m3u` next launch |
| Line length | 12 KB ÷ number of tracks | — |

### Remembering which playlist you were using

The core reopens the list you last used at the next launch. It has one
settings word to remember it in, which holds twelve characters — so on its
own, `Shenanigans.m3u` comes back and `Goose - Shenanigans Nite Club.m3u`
does not.

**Add a `playlists.m3u` and the limit goes away.** It's a plain list of the
playlists on the card:

```text
Crash Test Dummies - God Shuffled His Feet.m3u
Goose - Shenanigans Nite Club.m3u
Live/Phish - Hampton 1997.m3u
```

The core searches it by name at boot, so a playlist can be called anything you
like. Order doesn't matter and you can add or remove lines freely — entries are
matched by name, not by position. Generate it with:

```text
python tools/make_playlist_index.py "D:/Assets/mp3player/common"
```

Without the file nothing changes: names of twelve characters or fewer are still
remembered on their own, so an existing card keeps working exactly as it did.

Resume follows the same path. The core remembers the track and the second you
stopped on, but it finds them through the playlist it reopens — so if the
playlist can't be reopened, resume comes back at the start of `playlist.m3u`
instead. Resume covers the whole playlist, all 256 tracks.

**Tap Select** for the playlist: the list opens on the track that's playing,
**Up** / **Down** moves the cursor, **A** plays what's under it, and **Select**
or **B** closes without changing anything. Rows show filenames rather than tags
— a tag lives inside its file, so naming every row would mean opening every
file. With shuffle on the list is the play queue, so scrolling down shows
what's actually coming.

Tracks advance automatically. **Repeat**: off stops at the end, *all* loops,
*one* repeats the current track. **Shuffle** plays in a random order and never
repeats a track until the rest have played; with **Repeat all**, each pass round
the list is freshly shuffled.

A misspelled or missing filename costs that one track — the core steps over it
and says how many it skipped.

## What it shows

<img src="docs/screenshot.png" width="280" align="right" alt="Player screen: Feel Good Inc. by Gorillaz, track 6 of Demon Days 2005, encoded 128 kbps 44.1 kHz by LAME3.90, above a bar meter with the album cover at the right; below, a PLAYING label with repeat and shuffle indicators and the EQ preset ROCK, track 3 of 10, 02:31 of 03:41, and a progress bar">

- **Title and artist** from the file's tag. One with no readable tag shows its
  filename, which is usually the song name anyway.
- **Album art** from the tag's embedded image — baseline JPEG only; tracks
  without it don't show the panel.
- **Eleven meters**, cycled with **X**: bars, waterfall, L/R levels, phase
  scope, oscilloscope, twin analogue VU needles, scrolling waveform, mirrored
  bars, peak dots, a magic eye and a 16-band spectrum analyser.
- **Elapsed and total time**, with a progress bar.
- **Repeat and shuffle indicators**, dimmed rather than hidden when off, the
  **EQ preset name**, and the position in the playlist.
- **Bitrate and sample rate**, with the encoder that made the file where it
  says so — `128 kbps - 44.1 kHz - LAME3.100`.

CBR and VBR **MPEG-1 and MPEG-2** Layer III at every standard bitrate and sample
rate, mono or stereo, plus FLAC — see [below](#flac). MPEG-2 covers the lower
sample rates common in spoken-word recordings. Layer I and II — `.mp1` and
`.mp2` — are a different format and are not handled.<br clear="right">

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

Presets are loudness-matched, so switching changes the tone without changing how
loud the music seems.

## Playback speed

Hold **A** for 1.2×, hold again for normal. It's meant for spoken word: pitch
rises with the speed, so music sounds wrong. Off every launch. 1.2× is the
whole range — double speed would mean decoding twice as many frames a second,
past what the CPU can do.

## Screen blanking

**Select + Down** cycles the timeout: off, 1, 5, 10, 30 minutes. The screen
goes black after that long without a button press, and any button wakes it
without doing anything else — reaching for a sleeping player shouldn't pause
it. Playback carries on regardless. Resets to off each launch, and it blacks
the picture rather than powering down: a core can't reach the Pocket's
backlight, so it's for a dark room, not for battery.

## FLAC

Drop `.flac` files in with everything else and they play the same way — tags,
album art, meters, seeking.

| | supported |
|---|---|
| Sample rate | up to **48 kHz** |
| Bit depth | 8, 16, 20 and 24-bit |
| Channels | mono and stereo |

That covers CD rips and most libraries. Hi-res — 88.2, 96, 176.4 and 192 kHz —
is out, along with 32-bit and multichannel. Anything the core can't play says
so on screen and names the file's own format, so you aren't left guessing.

The limit is the CPU, not a setting: a 24-bit 44.1 kHz track already uses about
80% of the time available, and the same music at 96 kHz needs nearly twice what
the chip can do. Converting a hi-res album to 44.1 kHz is still lossless, and
on headphones from a handheld it isn't a difference you're going to hear.

## How it works

There's no audio decoder chip in the Pocket, so the FPGA is loaded with a
RISC-V CPU and the decoders run on it as software, with the audio queue and the
equalizer built as hardware around it.

If that sounds interesting, the longer version — including the two Analogue
framework bugs that had to be found first — is in
[docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).

## Known limitations

- **FLAC up to 48 kHz.** Hi-res files are turned away with the reason on
  screen; see [FLAC](#flac) for why, and what to convert them to.
- **Baseline JPEG album art only.** PNG and *progressive* JPEG covers are
  skipped rather than shown wrong — re-save as baseline if a cover doesn't
  appear. See [ROADMAP.md](ROADMAP.md).
- **Playlists are capped at 256 tracks**, or 12 KB of `.m3u` text — whichever
  comes first, which allows about 48 characters per line. A playlist that runs
  past either says so instead of quietly playing fewer.
- **A playlist with a name longer than 12 characters needs a `playlists.m3u`
  to be remembered.** Without one it plays fine but won't be the list that
  loads next launch, and resume won't follow it. See
  [Remembering which playlist you were using](#remembering-which-playlist-you-were-using).
- **Sometimes a playlist or track pick doesn't register straight away.** It
  loads on its own a few seconds later; if it doesn't, pick it again. Seen when
  switching from one playlist to another.
- **A track with no duration header may tic once, a couple of seconds in.**
  Files carrying no Xing/Info/VBRI header have no other source for their
  total time, so the core measures the file during playback and the reads
  can disturb the stream briefly. Everything else — FLAC, and any MP3 with
  a header, which is nearly all of them — is unaffected.
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
any of my projects bring you joy, chip in below; it fuels the next thing.

💛 **[Support this project via PayPal](https://www.paypal.com/donate/?hosted_button_id=S22WV924XU2ME)**
