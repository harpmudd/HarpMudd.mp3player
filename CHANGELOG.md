# Changelog

What changed in each release, newest first.

## v1.3.0 — unreleased

- **FLAC playback.** Lossless files play alongside MP3s, with their tags,
  album art and meters, and the same seeking. Up to 48 kHz and 24-bit, which
  covers CD rips and most of what people have — see the README for what plays
  and what doesn't.
- Hi-res FLAC above 48 kHz says so on screen, naming the file's own rate,
  instead of playing badly.
- **Playlists work in subfolders.** Keep an album in its own folder with a
  playlist beside it and it plays — an `Artists/Album/tracks` library needs no
  rearranging. Previously the first track played and the rest failed to open.
- `tools/make_album_playlists.py` writes a playlist into every album folder of
  a library, so a collection of any size needs one command rather than one
  playlist typed per album.
- Playlists can hold **256 tracks**, up from 128.

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
