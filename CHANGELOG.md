# Release notes

What changed in each release, newest first.

## v1.8.3 — 19 October 2026

- Fixed: **long lyrics stopped part-way through the song.** A sheet of more
  than about 80 lines was cut short with nothing to say so — on one track
  here it stopped two thirds of the way in. Lyrics now hold well over twice
  as much, and a sheet that still does not fit says so instead of just
  ending.

## v1.8.2 — 18 October 2026

- Fixed: **the previous song's lyrics stayed on screen through a track
  change**, until the new track's artwork had finished loading.
- Fixed: **resume came back at the wrong track on a playlist longer than
  128 songs** — it returned to one exactly 128 earlier in the list.
- Fixed: **LOADING TRACK appeared on a blanked screen and stayed there.**
  A track change is not supposed to wake a sleeping player.

## v1.8.1 — 17 October 2026

- **The controls cheatsheet closes with Start as well as B.** Start is what
  opens it, so it is the button you reach for to put it away.
- Fixed: **a track change painted over the controls cheatsheet** while it was
  open. It stays in front now until you close it.

## v1.8.0 — 17 October 2026

- **Scrobbling to Last.fm.** The player now keeps track of what you have
  listened to and shows it as a QR code — hold both shoulder buttons. Scan it
  with your phone and a web page submits the plays to your account. Long
  sessions split across several codes; **Left** and **Right** move between
  them. Needs a Last.fm API key of your own, which takes about two minutes to
  set up once and is explained in the
  [guide](https://harpmudd.github.io/HarpMudd.mp3player/scrobble/setup.html).
- **Lyrics moved to PSRAM**, which is what made room for scrobbling. They
  are parsed as a track loads rather than held in the block RAM the decoders
  need. Nothing changes in use.
- **Meter refinements.** The meters are taller and the bars wider, the VU
  meter has a new needle and dial face, and the magic eye is bigger.
- **A controls cheatsheet, on a hold of Start.** Every button and combo on
  one screen; **B** closes it. Stop now happens when you release Start
  rather than when you press it.
- **The file format shows as a badge** — **MP3** or **FLAC** — in the corner
  of the track card and of the playlist browser, where the browser's follows
  the cursor. It used to be spelled out in the format row; a FLAC's row now
  gives its bit depth on its own.
- Fixed: **elapsed time stopped while the screen was blanked**, and did not
  catch up when the screen came back. Blanking is off unless you switch it on
  with **Select** + **Down**, so most people will never have seen it.
- Fixed: **album art stored without a description field showed COVER
  ERROR** when the picture was perfectly fine.
- Fixed: **switching to the lyrics meter mid-track sometimes said NO
  LYRICS** on a track that has them.

## v1.7.0 — 12 October 2026

- **Lyrics** — cycle to them with **X**. The words scroll in time with the
  song, the line being sung picked out. They come from a `.lrc` file beside
  the track, or from lyrics already inside the file's own tag.
- **Sharper titles and headings**, drawn at their real size instead of scaled
  up from a smaller one. The startup screen too.
- **Seeking a VBR MP3 lands where the clock says** — it used to finish a few
  seconds out.
- **Files with no header show their length straight away**, from any playlist
  with `#EXTINF` lines. It used to appear several seconds late.
- **Album art loads faster**, up to about 1.6× on large covers.
- **Playlist browser polish** — it fades in and out, and a pulsing play
  triangle marks the playing track in place of the `>`.
- **FLAC files written in 8192-sample blocks now play.**
- Fixed: **"FILE SIZE WRONG - CHECK SD CARD" on a healthy card.**
- Fixed: **a file the core won't play now says why in plain words, and Next
  and Previous still work** — a playlist no longer stops dead on one.

## v1.6.0 — 6 October 2026

- **Crossfeed and mono**, on **Select** + **Y**. Crossfeed moves hard-panned
  records out of your head; mono sends the whole mix to both ears, so nothing
  is lost if you hear better on one side. Setting remembered between sessions.
- **A sleep timer.** **Select** + **Up** cycles off, 15, 30 and 60 minutes.
- **FLAC decoding is about a third faster**, so the files that used to break
  up now play cleanly — on any meter, including the demanding ones.
- Fixed: **holding seek on a FLAC could jump backwards and stop advancing.**

## v1.5.1 — 1 October 2026

- **FLAC files that were at the edge now play cleanly** — the ones that used
  to break up hold together, on any meter.
- **Text is sharper**, with cleaner edges on every letter.
- **An incomplete update now says so** instead of showing a black screen.
- Fixed: **1.2× playback distorted in dense passages.**
- Fixed: **the cassette's label showed the last playlist's name** when you
  played a single track with Load MP3. It is blank now, like a tape nobody
  has written on.

## v1.5.0 — 18 September 2026

- **Titles and artists now display in Japanese, Chinese, Greek, Cyrillic and
  European accents** — é ö ł ş as well as 初恋 — on the player and in the
  playlist browser. Ships as `mp3font.bin`, beside the firmware.
- New meter: an animated **cassette**. The hubs turn and coast to a stop when
  you pause, the label carries the playlist's name, and the tape and the shell
  edge react to the music. Twelve meters now.
- **Boot and track changes are quicker.**
- Fixed: **picking a track in the playlist browser could start it at 1.2x**.

## v1.4.0 — 21 August 2026

- **Playlist browser** — tap **Select**. **Up**/**Down** moves,
  **Left**/**Right** pages, **Y** jumps to the playing track, **A** plays.
- New meter: a **16-band spectrum**, bass to treble. Eleven meters now.
- **A volume icon** beside the repeat and shuffle indicators.
- **MPEG-2 files play** — the low sample rates common in spoken word.
- **Playlists can have long names again.** List them in a `playlists.m3u`; only
  twelve characters were remembered before, so a long name never came back.
- **FLAC tracks load faster.**
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
- Fixed: **seeking landed in the wrong place on some files**, and their total
  time was wrong too.
- Fixed: Load MP3 could lose your pick to a playlist reload.

## v1.0.0 — 10 August 2026

First public release. Plays your own MP3s off the SD card, with album art, ID3
tags, nine meters, an eight-preset equalizer and playlists.
