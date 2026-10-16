# v1.8.0 release notes

Paste-ready body for the GitHub release. The CHANGELOG is the detailed
record; this is the shorter version someone reads on the release page
before deciding to download.

---

## MP3 Player v1.8.0 — scrobbling, and the meters grow up

**Scrobbling to Last.fm, without a network.** The Pocket has no radio, so
the player keeps a list of what you have listened to and shows it as a QR
code — **hold both shoulder buttons**. Scan it with your phone and a web
page submits the plays to your account. A long session splits across
several codes; **Left** and **Right** move between them, and **B** closes.

A play is recorded once you have heard half of it, or four minutes,
whichever comes first, and never for a track under 30 seconds. Those are
Last.fm's own rules, so what you submit is what the service would have
counted anyway.

Submitting needs a Last.fm API key of your own — about two minutes, once.
There is deliberately no key built into the page: it is a single file
served from GitHub, so anything inside it is readable by anyone, and one
shared key would stop working for everybody the first time it was abused.
Your key lives in your own browser.

**Setup guide:**
<https://harpmudd.github.io/HarpMudd.mp3player/scrobble/setup.html>

**Every meter is taller.** They were drawing into a 72 px band while the
cassette and the lyrics view used the full 96; now all thirteen do. The
bars are wider with it, and the VU meter has been rebuilt as a real
instrument — recessed face, graduated logarithmic scale, white needle,
machined pivot, red zone above 0. The magic eye is bigger and sits on its
base again, and the cassette's label now carries the artist.

**The boot screen does something.** The title catches a travelling
highlight as it arrives and the meter fills at a pace you can see.

### Also fixed

- Elapsed time stopped while the screen was blanked and never caught up.
- Album art stored without a description field showed **COVER ERROR** when
  the picture was fine.
- The scrolling meters were dimmer on one side than the other.

### Installing

Unzip to the root of your Pocket's card, keeping the folder structure —
`Cores/`, `Assets/`, `Platforms/`. Existing settings, playlists and resume
points are kept.

Copy **all** of it. The firmware and the bitstream ship together and the
core checks they match: a partial update shows **UPDATE INCOMPLETE** with
the two revisions rather than failing quietly.

### Known limitations

- FLAC up to 48 kHz; hi-res files are turned away with the reason on
  screen.
- Baseline JPEG album art only.
- A listening session is forgotten when you quit the core, and holds 128
  plays. Scan the code before you leave if you want to keep them.
