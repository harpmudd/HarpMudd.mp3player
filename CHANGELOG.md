# Changelog

What changed in each release, newest first.

## v1.4.0 — unreleased

### New

- **Playlist browser.** Tap **Select** to browse the list on screen: **Up** /
  **Down** to move, **Left** / **Right** to page, **Y** to jump back to the
  playing track, **A** to play, **Select** or **B** to close. It opens on
  what's playing. Holding **Select** still shows and hides the album art.
- **A 16-band spectrum meter**, bass on the left through treble on the right,
  green through amber to red as the columns climb. Eleven meters now.
- **A volume icon** beside the repeat and shuffle indicators — muted, low,
  medium or high. The level used to appear only as a message that had gone by
  the time you wondered why the music was quiet.
- **MPEG-2 files play** — the 16, 22.05 and 24 kHz rates common in audiobooks
  and spoken word, previously listed as unsupported.
- **Playlists can have long names again.** Only twelve characters of a
  playlist's filename could be remembered, so an album-length name played fine
  but was never the list that came back next launch. List your playlists in a
  `playlists.m3u` and the limit is gone; cards without one behave as before.

### Improved

- **FLAC tracks load faster.** Measuring the file's length no longer blocks the
  load, and opening a FLAC now steps over the embedded cover art instead of
  reading through it — a quarter of a megabyte on a typical album.
- **The meters have more headroom, and the spectrum reads in decibels.** Loud
  material used to flatten against the top of the range. A linear scale spends
  nearly all of itself on the loudest few decibels; the spectrum now shows
  about 19 dB, so quiet detail is visible and different albums look alike.
- **1.2x is harder to hit by accident, and says when it's on.** Still a hold on
  **A**, but a longer one — the old timing was shared with the seek scrub. A
  small **1.2x** sits under the track counter while the mode is active.
- **A cover that can't be shown says so**, instead of looking exactly like a
  track with no artwork. The art panel shows **PROG. JPEG** for a progressive
  JPEG, or **COVER ERROR** for anything else that won't decode. Re-saving as a
  baseline JPEG fixes it.
- Small visual tidying: the format line shows the full codec (**FLAC 16-bit**
  was appearing as *FLAC 16-*), the selected playlist row has rounded corners,
  and the progress bar has softened ends and a lit top edge.
- The playlist limits are now written down in the README.

### Fixed

- **A light stutter about two seconds into every FLAC.** Measuring a file's
  length meant reading at far-apart offsets on the file being streamed, which
  disturbed the stream and made the decoder resynchronise. Only files with no
  other way to report their length are measured this way now.
- **Volume had no effect on FLAC.** Turning it to zero still played at full
  level.
- **Seeking a FLAC put the clock in the wrong place,** permanently — seek near
  the end and the counter reached the total while the music kept playing. It
  now measures where it lands rather than assuming. Present since 1.3.0.
- **A FLAC played from a playlist would barely seek at all,** while the same
  file opened with Load MP3 seeked normally. Its size was being read far too
  small — a 30 MB track came out as 5 MB — which also showed as a nonsense
  bitrate in the header.
- **Resume started tracks from the beginning,** and **returned to the wrong
  track in playlists over 128 entries** — track 200 of 240 came back as 72.
- **The encoding line often stayed blank for FLAC.** It's now worked out from
  playback itself and appears a few seconds in.
- **Elapsed time ran at double speed on MPEG-2 files.** Playback speed was
  always correct; this was the clock and the progress bar.

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
