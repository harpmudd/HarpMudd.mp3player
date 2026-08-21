# Changelog

What changed in each release, newest first.

## v1.4.0 — 21 August 2026

- **Playlist browser** — tap **Select**. **Up**/**Down** moves,
  **Left**/**Right** pages, **Y** jumps to the playing track, **A** plays.
- New meter: a **16-band spectrum**, bass to treble. Eleven meters now.
- **A volume icon** beside the repeat and shuffle indicators.
- **MPEG-2 files play** — the low sample rates common in spoken word.
- **Playlists can have long names again.** List them in a `playlists.m3u`; only
  twelve characters were remembered before, so a long name never came back.
- **FLAC tracks load faster** — measuring the length no longer blocks the load,
  and opening one steps over the embedded cover art instead of reading it.
- **The meters have more headroom**, and the spectrum reads in decibels, so
  loud albums no longer flatten against the top.
- **1.2x needs a longer hold on A**, and shows a marker while it's on.
- **A cover that can't be shown says so** — **PROG. JPEG** or **COVER ERROR**.
- Fixed: a light **stutter two seconds into every FLAC**.
- Fixed: **volume had no effect on FLAC**.
- Fixed: **seeking a FLAC left the clock permanently wrong**. Since 1.3.0.
- Fixed: **a FLAC from a playlist barely seeked**, and showed a wrong bitrate.
- Fixed: **resume started at the beginning**, or on the wrong track past 128
  entries.
- Fixed: **the encoding line often stayed blank for FLAC**.
- Fixed: **elapsed time ran at double speed on MPEG-2 files**.
- Small tidying: the full codec name shows (**FLAC 16-bit** was appearing as
  *FLAC 16-*), rounded corners on the selected playlist row, softened ends on
  the progress bar, and the playlist limits are written down in the README.

## v1.3.0 — 15 August 2026

- **FLAC playback** — up to 48 kHz, 16 or 24-bit, with tags, album art, meters
  and seeking. Covers CD rips and most libraries.
- A file the core can't play now says **why**, naming its own format, instead of
  failing generically.
- **Playlists work in subfolders**, so an `Artist/Album` library needs no
  rearranging.
- Playlists can hold **256 tracks**, up from 128.
- Meters are more accurate — about half the loudness peaks never used to reach
  the display.
- Long titles scroll in more cases; some were clipped instead.
- Fixed: a playlist or track pick that didn't register should now be rarer.
- Fixed: the loading `...` animation was invisible about half the time.
- Fixed: the first press of **A** on a freshly loaded track said STOPPED
  instead of PAUSED.
- Fixed: faint flicker on the mirrored-bars and peak-dots meters.

## v1.2.0 — 13 August 2026

- Resume now works with **any** playlist, not just `playlist.m3u` — it
  remembers which list you were in. Switch it on in Core Settings.
- New meter: a **magic eye**, a pair of EM84 tubes that light the panel. Press
  **X** to reach it.
- Files with no ID3 tag show their **filename** instead of a placeholder.
- Switching playlists shows a loading indicator and pauses while it works, so a
  slow load no longer looks like a failed one — and a pick the core misses
  recovers on its own within a few seconds instead of needing a retry.
- Fixed: long titles painted through the info panel's border.
- Fixed: the meters sat on a flat panel instead of the background gradient.
- Fixed: the startup screen flickered.

## v1.1.0 — 11 August 2026

- **Resume where you left off** in a playlist — the track and your position in
  it. Switch it on in Core Settings.
- **1.2× playback speed** for spoken word. Hold **A**.
- Screen blanking moved to **Select + Down**.
- The album art panel and the screen-blank timeout are no longer remembered
  between launches — their saved slots went to resume. Everything else carries
  over from v1.0.0.
- Fixed: seeking landed in the wrong place on files with no Xing header, and
  their total time was wrong too.
- Fixed: Load MP3 could lose your pick to a playlist reload.

## v1.0.0 — 10 August 2026

First public release. Plays your own MP3s off the SD card, with album art, ID3
tags, nine meters, an eight-preset equalizer and playlists.
