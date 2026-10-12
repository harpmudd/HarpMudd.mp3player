# QA test plan — v1.7.0

**SIGNED OFF 2026-10-12.** Tested throughout the build rather than as one
pass at the end, which is how v1.6.0 was taken too. No items open.

Firmware **144,044 bytes** (`2f2c8cf4`), bitstream **rev 24** (`6dccf2c3`),
font **1,600,736 bytes** (`28d0d019`).

Splash reads **v1.7.0**, with NO "(Dev)" -- the card now carries a RELEASE
build (`-DAPP_RELEASE=1`), so that the artifact being tested is the artifact
that would ship. A build reading "v1.7.0 (Dev)" is the development firmware
and is 8 bytes larger; if the splash says (Dev), you are not testing the
release.

**All three files must be installed together.** The interlock is at rev 24
(`EXPECT_VERSION 0x4D503318`) and the v1.6.0 firmware will not run on this
bitstream or vice versa. The font is interlocked separately and by SIZE:
`FEXT_BYTES` must equal 1,600,736 exactly, or every native 24/32 px glyph
silently falls back to the scaled one and the release's headline change
disappears without a word.

## What this document is, and is not

**Most of 1.7.0 has already been tested on hardware, continuously, as it was
built** — the native font cells across four rounds of glyph corruption, lyrics
(signed off explicitly), the USLT fallback, Japanese lyrics, VBR seek on the
file that exposed it, the `#EXTINF` duration fix, and the file-size guard.
Those are **not** open questions and are listed here only where a deliberate
pass would catch something incidental use would not.

What this is for: the items that prolonged use does **not** target, plus the
regression checks for things 1.7.0 could have broken from a distance.

Ordered by risk, not by feature.

**Filing a failure:** note *which file* was playing, *what you pressed*, and
*which meter was showing*. All three have decided a diagnosis on this project —
the meter twice, the previously-loaded file once, and that last one was the
only thing that ever cracked the seek wedge.

---

## 0. Setup

The card needs:

- a `playlist.m3u` with `#EXTINF` lines — regenerate with
  `python tools/gen_m3u.py E:/Assets/mp3player/common` after adding anything
- at least one **headerless** MP3 (no Xing/Info frame). On the current card:
  Stockholm Syndrome, Goodpeople, LCD Soundsystem, Interstate Love Song
- at least one MP3 **with** a Xing header, as the control
- one **24-bit** and one **16-bit** FLAC — different decoder paths, and only
  the 16-bit one is covered by tooling
- at least one `.mp3` **not** in the playlist (the standalone path differs)
- a `.lrc` beside at least one track, and one track with a `USLT` frame
- an album with cover art, and the Japanese-titled FLAC

Run `python tools/card_check.py` first. It reproduces the core's own file-size
cross-check over every file at once and sweeps for covers the core cannot
decode. Expect no size mismatches; it currently reports one progressive cover
(Circles Around The Sun — Language), which is known and deliberate.

No debug switch is on in this build, so **no red diagnostic row should appear
anywhere**. If one does, you have the wrong firmware.

---

## 1. The M10K diet — CLEARED 2026-10-12

**Why it leads:** it ships no feature, so nothing in normal use points at it,
and its failure mode is the worst kind — the CPU register file and cache tags
moved out of block RAM, so a fault means *intermittent wrong results* rather
than a clean crash. It was deliberately held out of v1.6.0 for exactly this
reason.

It is, however, **in the bitstream that has been running all session** —
`b8678f6` and `36b7a95` both precede the rev 24 build (`2e413a8`) with no
later changes to `VexRiscv_Full.v` or `sync_fifo.v`. So it has had hours of
real use already without a deliberate target. ROADMAP.md still calls it
"untested on hardware"; that line is stale.

What a fault would look like, and so what to watch for:

1. **A wrong number that corrects itself.** A clock that jumps and comes back,
   a duration that reads wrong once and is right on reload, a progress bar
   that lurches. Note it even if it looks like a one-off — *especially* then.
2. **Decode errors with no audio symptom.** Play a dense 24-bit FLAC for a
   full track and watch for frame errors where none occurred on v1.6.0.
3. **The same track twice.** Play one file, note the readouts, reload it and
   compare. Register-file corruption shows up as non-repeatability.
4. **Long soak.** The one thing no in-session testing covered: leave it playing
   a long playlist for an hour or more and come back. Intermittent faults need
   time, not pressing.

If anything here is odd, say so before interpreting it — this is the change
most likely to present as an unrelated bug somewhere else.

---

## 2. The splash, at native 32 px — CLEARED 2026-10-09

**Seen and confirmed good by the user.** Nothing further needed here; kept
for the record and because the measurements explain what to expect if the
splash is ever touched again.

This was the only item in the release that had not been on screen. The splash
heading was the last scaled text in the core and now draws from the native
32 px cell.

- Boot and read "MP3 PLAYER". Letters should be as crisp as the track title,
  with no doubled or ragged pixel edges.
- It is 209 px wide now against 218 px before, so it sits very slightly
  narrower. Confirm it is still centred/aligned as intended and not clipped.
- The fade-in still runs (the scale is chosen once, the colour mixes per
  frame) — confirm it still fades rather than popping in.

Measured before the change, against a 360 px budget: scaled 218 px, native
209 px. Both fit, so this cannot be a clipping regression — but look anyway.

---

## 3. Font interlock and the fallback path — CLEARED 2026-10-12

- Install **only** the new firmware, keeping the old 833,536-byte font, and
  boot. Every 24/32 px glyph should fall back to the scaled rendering and the
  core should otherwise work normally. **It must not show a blank or corrupt
  title.** Put the right font back afterwards.
- Install a **mismatched bitstream/firmware pair** and confirm you get the
  "UPDATE INCOMPLETE" screen rather than a black one.

---

## 4. The font page clamp (RTL) — CLEARED 2026-10-09

**The user judges this fixed, and the evidence supports it.**

`sim/tb_fontload.v` re-run against the shipping RTL reports **0 wrong, 0
unwritten of 4096 words**, where before the clamp it miscompared 18 with the
first at word 1024 exactly. The fix is structural rather than probabilistic --
a burst cannot cross a page edge by construction -- so a clean sim is strong
evidence rather than a lucky run.

Alongside that, the rev 24 bitstream has been through roughly a dozen cold
boots during this session: every firmware revision copied to the card is a
core relaunch, and a relaunch reloads the font. No recurrence in any of them.

Kept for the record because the signature is worth recognising again:
corruption is baked in at LOAD, so a bad glyph is wrong for a whole session
and right after a restart. If it ever returns, restarting is the test and
`tb_fontload.v` is where to look first.

The original plan follows.

### What it was, and how to retest it

The glyph corruption chased for four rounds ended at a font-load write burst
crossing an SDRAM page edge. The fix is in the bitstream, not the firmware, so
it cannot be checked host-side.

- Cold-boot several times and each time read a title containing the characters
  that failed: **L**, **C**, lowercase **d**, and a **5** in the clock.
- The giveaway is that it was *per-session*: a glyph renders wrong for a whole
  session and comes good after a restart, because a restart reloads the font.
  So **restarting is the test**, not staring at one session.
- Do at least ten cold boots. The last recurrence was seen once after the
  fix was believed complete.

---

## 5. Headerless MP3 duration, and the stutter

Already confirmed working, listed because the failure is quiet and the cause
is now data (the playlist) rather than code.

- Play each headerless MP3 **from the playlist**. The duration must appear with
  the first clock, and there must be **no stutter** a few seconds in.
- Play one of them with **Load MP3** instead. Duration should also be
  immediate, by a different route (`R_SLOT_SZ` from the menu pick).
- Add a track to a playlist **without** re-running `gen_m3u.py` and play it:
  it should fall back to the old behaviour — late duration — rather than break.

---

## 6. The file-size guard

- Nothing on a healthy card should ever show
  `! FILE SIZE WRONG - CHECK SD CARD`. `tools/card_check.py` agreeing is the
  precondition; this confirms the core agrees.
- It can only appear on a **Load MP3** pick of an MP3 with a Xing header.
  Playlist opens cannot reach it.

---

## 7. Lyrics — regression only

Signed off already. Check only the interactions that a later change could have
disturbed:

- `#EXTINF` now sets `track_secs` for headerless files, and lyrics sync off the
  clock. Confirm lyrics stay in sync on a **headerless** track with a `.lrc`.
- Seek on a VBR MP3 with lyrics, and confirm the lyric line follows the new
  position rather than the old one.
- Hide the album art while lyrics are showing, and tap **B** to restart.

---

## 8. Regression checks — things 1.7.0 could have broken from a distance

- **Crossfeed and mono** (Select + Y), **sleep timer** (Select + Up) — v1.6.0
  features, untouched but close to the settings path.
- **Album art**, including the slide animation and the rounded corners.
- **Playlist overlay**: scroll it, pick from it, switch with shuffle on.
- **Remembered playlist** across a power cycle — `pl_name_hash` agrees with the
  tool host-side, so a failure here is the APF side, not the hash.
- **The meter carousel**, all the way round. `interact.json`'s `max` widened
  from 11 to 12 for the lyrics meter; if the **12th** meter fails to persist
  across a restart, that is the one file to look at.
- **Non-ASCII titles** — Japanese, Cyrillic, Greek. Verified host-side by
  `text_check.py`, but the font path changed underneath it.

---

## 9. Host-side results for this build

Run and passing at `eaaef78`; no need to repeat, recorded so a failure on
hardware can be read against them.

| harness | result |
|---|---|
| `lrc_check` | PARSER OK |
| `trunc_check` | TRUNCATION HANDLED |
| `wrap_check` | WRAP OK — every row fits |
| `uslt_check` | USLT DECODER OK |
| `toc_check` | TOC LOOKUP OK — 0 of 47 synthetic, 0 of 45 real |
| `extinf_check` | EXTINF OK — 14 cases + negative control |
| `vbr_check` | VBR GUARD OK — 12 cases, both directions |
| `text_check` | PASSED (0 failures) |
| `hash_check` | firmware and tool agree on every name |

---

## 10. Before release — not part of testing

- `core.json` still says `date_release: 2026-10-06`, which is v1.6.0's date.
  Set it to the actual release date at tag time, not before.
- ~~CHANGELOG.md has no 1.7.0 entry yet.~~ **DONE 2026-10-12**, dated
  "unreleased" — the date goes in with the tag.
- ~~README body text describing features is not checked by anything.~~
  **REVIEWED 2026-10-12.** It had no mention of lyrics at all, said twelve
  meters when there are thirteen, and told readers that 8192-sample FLAC
  blocks need more buffer than the decoder has — which stopped being true in
  this release. All three corrected. The "Current version" line is checked by
  `fw/build.sh` and was already right.
- `diag.m3u` points at the pre-rename `The Blue Hearts - 12 - Linda Linda.flac`.
  Card-side only; it is not in the repo.
- The `(Dev)` marker comes off at release. Nothing to edit: the default is
  DEV and a release build asks for `-DAPP_RELEASE=1`, deliberately that way
  round.
