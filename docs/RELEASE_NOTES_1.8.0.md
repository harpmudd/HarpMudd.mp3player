# v1.8.0 release notes

The GitHub release body, verbatim. House format, the same as every release
since v1.5.0: bold section labels, one flat bullet an item, each opening
with the change itself in bold. Short on purpose — the detail lives in
`CHANGELOG.md` and the README, and a release page is read once. If the
changelog and this disagree, the changelog is right.

---

**New**

- **Scrobbling to Last.fm, without a network.** The player keeps a list of what you have listened to and shows it as a QR code — **hold both shoulder buttons**. Scan it with your phone and a web page submits the plays to your account. It needs a Last.fm API key of your own, which takes about two minutes once — [setup guide](https://harpmudd.github.io/HarpMudd.mp3player/scrobble/setup.html).
- **A controls cheatsheet, on a hold of Start.** Every button and combo on one screen; **B** closes it.
- **The file format shows as a badge** — **MP3** or **FLAC** — on the track card and in the playlist browser, in place of the codec name the format row used to carry.

**Changed**

- **Meter refinements.** The meters are taller and the bars wider, the VU meter has a new needle and dial face, and the magic eye is bigger.
- **Start stops on release rather than on press**, so holding it for the cheatsheet does not stop the track.
- **Lyrics are held in PSRAM**, which is what made room for scrobbling. Nothing changes in use.

**Fixed**

- **Switching to the lyrics meter mid-track sometimes said NO LYRICS** on a track that has them.
- **Elapsed time stopped while the screen was blanked** and never caught up.
- **Album art stored without a description field showed COVER ERROR** when the picture was perfectly fine.
