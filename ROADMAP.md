# Roadmap

Things worth doing next, with enough context that picking one up doesn't mean
re-deriving why it's on the list. Ordered by value per effort, not by ambition.

## Settings persistence — RELEASE BLOCKER

`SETTINGS_WRITE` is **0**. The core reads `settings/settings.bin` at launch and
writes nothing to the card. This is the one item standing between the core and a
release: everything else is HW-confirmed.

Three times, every `.mp3` sharing the card with it came back reporting the same
size — 21,037,825, then 21,365,505, then 20,382,465 — while `settings.bin` itself
stayed 32 bytes.

What the third event established, by measurement rather than inference:

- **`chkdsk` passes.** This was never FAT corruption. The files really are that
  size: they were *extended*, and the extension was recorded correctly.
- **The originals are fine.** The card copies had been grown from good files, so
  this is damage rather than a bad copy.
- **Our magic is inside someone else's file.** `"SPM3"` (0x53504D33) appears in
  one `.mp3` at offset 5,505,024 — a 128 KB cluster boundary immediately past
  that file's real audio.
- **The audio does not survive.** exFAT doesn't zero newly allocated clusters, so
  the added region carries unrelated data; one file's audio no longer began with
  a frame header. The earlier reading — "sizes wrong, audio intact" — is retired.

Two mitigations were reasoned from that signature and **both failed**: deferring
writes to paused/stopped, then isolating `settings.bin` in its own subdirectory.
A third mitigation is not the answer.

One recorded theory was **wrong** and is called out so it isn't rebuilt: four
files landing on one size looks like a write to a fixed offset, where the size
becomes offset + length. But `settings_store()` passes **offset 0, length 32**,
and a write at offset 0 cannot extend a file to 20 MB. The fault lies either in
what the target registers carry by the time APF reads them, or in APF's own
handling — not in the offset we pass.

**Next step, cheap and decisive, not yet run:** record every `.mp3` size, perform
ONE write with nothing else running, re-record. If any `.mp3` grows, `0184` is
unusable by this core and settings need an approach that never writes the card —
which likely means giving up on saving them from the UI and treating
`settings.bin` as a hand-edited preferences file, which already works.

Details and the full reasoning live at the `SETTINGS_WRITE` definition in
[fw/settings.inc](fw/settings.inc).

## PNG album art

The cover-art decoder is [picojpeg](third_party/picojpeg/), baseline JPEG only.
`art_find_apic()` checks the APIC frame's MIME type and skips anything that
isn't JPEG, so a PNG cover shows the placeholder rather than failing oddly —
correct, but a real gap: PNG is common in tags written by iTunes and by several
taggers.

What it needs:

- **A PNG decoder that fits.** PNG is DEFLATE plus per-line filters. The
  decoder must stream, because the art path already streams JPEG — nothing here
  can hold a 300×300 RGB image in RAM (that's 270 KB against a 256 KB budget
  already 63% used). [uPNG](https://github.com/elanthis/upng) is the usual
  small-target choice; it decodes to a full buffer, so it would need adapting
  to emit rows, or a bounded-window rewrite.
- **A ~32 KB inflate window.** DEFLATE back-references reach 32 KB, so unlike
  JPEG's 8×8 blocks there is no way to decode PNG with a small fixed buffer.
  That window is the real cost and the reason this isn't a quick job. The MP3
  ring is 32 KB and is free during a load — worth checking whether it can be
  borrowed, since art decoding happens in the silent gap before playback.
- **Row-wise downscale, matching the JPEG path.** `art_emit_row()` already
  takes a row of RGB565 and RLE-encodes it into the SDRAM stash; a PNG decoder
  that produces rows can reuse it unchanged.
- **No 1/8-scale shortcut.** picojpeg's reduce mode decodes one pixel per 8×8
  block, which is why a 600×600 cover costs almost nothing. PNG has no
  equivalent — every pixel must be inflated and unfiltered before it can be
  discarded. Expect the decode to be substantially slower than JPEG's ~207 ms.

Where it goes: `fw/art.inc`, beside `art_find_apic()`. The MIME check already
distinguishes the two formats, so the dispatch point exists.

Worth measuring first: how many covers in a real library are actually PNG. If
it's a handful, the effort is better spent elsewhere.

## Gapless playback

Track changes currently have a short silence — the new file has to be opened,
its tag read and its art decoded before a note plays. Removing it means
decoding the next track's opening frames *while* the current one is still
playing, which needs a second decoder instance (~34 KB heap) and a second ring.
Memory is the constraint, not logic.

## Spectrum display

The decoder doesn't expose frequency bins, so every meter shows loudness,
waveform or stereo. Real frequency content needs either an FFT or a few
one-pole IIR filters over the PCM buffer to split bass/mid/treble. The filter
bank is affordable (a few hundred MACs a frame, subsampled) but lands in the
budget that keeps the decoder fed — the one item on this list that could bring
audio tics back. Prototype behind a compile-time flag.

## MPEG-2 / 2.5 and Layer I/II

Helix decodes them; the core rejects them. Mostly a matter of not assuming
MPEG-1 Layer III in the frame-size arithmetic (`144 * bitrate / samplerate`),
the duration estimate and the oscilloscope's trigger window. Low risk, low
excitement, widens the library.

## Playlist browsing on screen

`0192` opens a file by name and works. The missing piece is showing the list:
the names are already parsed into `pl_text[]`, so this is a UI job — a
scrollable list, and a decision about whether it overlays the now-playing
screen or replaces it.

## Cleanup: retire what the click hunt left behind

Fixing the track-change click took many attempts, and several mechanisms
survive that may no longer earn their place — the decoder reservoir warm-up and
the async EOF discovery are the likeliest candidates, now that every cold load
finishes through the reposition body. Others are load-bearing: the FIFO
glide/fade pair, the cut-at-press transition, and the confirmed-EOF guard all
fix real faults. Worth a pass, one at a time, each verified on hardware —
not a bulk tidy.
