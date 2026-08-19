# Changelog

What changed in each release, newest first.

## v1.4.0 — unreleased

- **Tap Select for the playlist.** Browse it on screen, **Up**/**Down** to move,
  **Left**/**Right** to page, **A** to play, **Select** or **B** to close.
  Opens on the track that's playing, and **Y** jumps back to it. Holding
  **Select** still shows and hides the album art.
- **1.2x speed is harder to trigger by accident, and says when it's on.** It's
  still a hold on **A**, but a noticeably longer one — the old timing was shared
  with the seek scrub and was short enough to reach just by lingering on the
  play button. While the mode is on, a small **1.2x** sits under the track
  counter; at normal speed nothing is shown.
- Fixed: **volume had no effect on FLAC.** Turning it to zero still played at
  full level.
- Fixed: **seeking a FLAC put the clock in the wrong place**, and the error was
  permanent — seek near the end of a track and the counter reached the total
  while the music kept playing. Seeking now measures where it lands instead of
  assuming, so the time shown is the time you are at. Present since 1.3.0.
- Fixed: **a FLAC played from a playlist would barely seek at all**, while the
  same file opened with Load MP3 seeked normally. The core was measuring the
  file's size too early and reading it far too small — a 30 MB track came out
  as 5 MB — which also showed as a nonsense bitrate in the header. Both are
  right now.
- Fixed: **resume returned to the wrong track in playlists over 128 entries.**
  Track 200 of 240 came back as track 72.
- **Playlists can have long names again.** The core could only remember twelve
  characters of a playlist's filename, so an album-length name played fine but
  was never the list that came back next launch — and nothing on screen said
  so. Add a `playlists.m3u` listing your playlists and the limit is gone;
  `tools/make_playlist_index.py` writes it for you. Cards without one behave
  exactly as before.
- **The meters have more headroom.** On loud material they were running close
  to the top of their range and flattening out there. They behave exactly as
  before, just lower.
- **FLAC tracks load faster.** Measuring the file's length used to block the
  load for about half a second; it now happens quietly during playback, so a
  track starts sooner. The first seek of a track no longer pays for it either.
- The playlist limits are now written down in the README.

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
