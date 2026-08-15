# Changelog

What changed in each release, newest first.

## v1.3.0 — unreleased

**New**

- **FLAC playback** — up to 48 kHz, 16 or 24-bit. Lossless files play
  alongside MP3s with their tags, album art, meters and seeking, and the core
  reads the file itself rather than trusting the extension. That range covers
  CD rips and most libraries.

  FLAC is new in this release. If you have a file that won't play, please open
  an issue with its format — it helps.
- **A file the core can't play now says why**, on the track card, naming the
  file's own format — `HI-RES 88.2kHz - PLAYS 48kHz MAX` — instead of failing
  with a generic message or playing badly. Covers sample rate, bit depth,
  channel count and block size.
- **Playlists work in subfolders.** Keep an album in its own folder with a
  playlist beside it, or keep the playlist in `/Assets/mp3player/common/` and
  name tracks in folders below it. An `Artist/Album/tracks` library needs no
  rearranging.
- **Playlists can hold 256 tracks**, up from 128.

**Changed**

- Meters are more accurate on both formats. Loudness peaks were being sampled
  rather than accumulated, so about half of them never reached the display;
  they all count now.
- Long titles scroll in more cases. A title could sit just inside the width
  budget, have its last character clipped anyway, and never scroll — because
  the check measured the text's spacing rather than the space it actually
  paints into.

**Fixed**

- A playlist or track pick that didn't register should now be rarer. The track
  slot is re-checked the way the playlist slot already was, and a timing fault
  that could leave both checks inactive for the first several seconds after a
  load is fixed. Still listed under Known limitations — it was never
  reproducible on demand, so it is not being declared gone.
- The loading `...` animation was invisible about half the time, and the meters
  could run slowly and out of time for the first several seconds of a track.
  The same timing fault in four places: a deadline that read as *never* rather
  than *now*, depending on where a free-running counter happened to be.
- The first press of **A** on a freshly loaded track said STOPPED instead of
  PAUSED, and took another play/pause to correct itself.
- Faint flicker on the mirrored-bars and peak-dots meters when the bars moved
  quickly. Both were blanking a whole column and painting the bar back into it,
  so each bar was drawn twice a frame.
- FLAC tracks showed nothing on the format line where MP3s show bitrate and
  encoder. They now show `1635 kbps - 44.1 kHz - FLAC 24-bit`.
- The previous track's album art stayed on screen behind the message for a file
  that couldn't be played.

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
