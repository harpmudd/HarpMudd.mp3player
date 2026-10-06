# QA test plan — v1.6.0

Firmware **163,596 bytes** (`619cf8a1`), bitstream **rev 23** (`7b7335c7`).
Splash reads **v1.6.0 (Dev)** — if it does not say "(Dev)", you are running a
release build and this plan does not apply to it.

Ordered by risk, not by feature. §1–§6 cover what changed; §7–§11 are
regression checks on things this release could plausibly have broken from a
distance, which is the only reason they earn a place.

**Both files must be installed together.** The interlock is at rev 23 and the
v1.5.1 firmware will not run on this bitstream or vice versa.

**Filing a failure:** note *which file* was playing, *what you pressed*, and
*which meter was showing*. All three have decided a diagnosis on this project —
the meter twice, the previously-loaded file once, and that last one was the
only thing that ever cracked the seek wedge.

---

## 0. Setup

The card needs:

- a `playlist.m3u` with several entries, including at least one **24-bit**
  FLAC and one **16-bit** FLAC — they take different paths through the
  decoder and only one of them is verified by tooling (see §2)
- at least one `.mp3` that is **not** in the playlist (the standalone path
  differs on purpose)
- at least one track with **hard-panned stereo** for §5 — 60s/70s rock, drums
  entirely in one ear. Modern mixes will show almost nothing and prove nothing
- an album with **cover art**, for §8

No debug switch is on in this build, so **no red diagnostic row should appear
anywhere**. If one does, you have the wrong firmware.

---

## 1. Boot and the interlock — do this first

Everything below assumes the pair is matched.

| | check | pass |
|---|---|---|
| 1.1 | Core boots to the player | no black screen, no message |
| 1.2 | Splash version | reads **v1.6.0 (Dev)** |
| 1.3 | Audio plays at all | any track |

**1.4 — the mismatch screen.** Optional but cheap: rename `mp3player.rom`,
boot, and confirm you get a readable message rather than a black screen. Put
it back afterwards. This was new in v1.5.1 and `EXPECT_VERSION` moved to rev 23
in this release, so the path has changed since it was last exercised.

---

## 2. FLAC decoding — the largest change in the release

The decoder is about a third faster: a new 32-bit predictor path, an unrolled
64-bit path, and a fused Rice reader. Every sample the core produces goes
through code that was rewritten this cycle.

**What tooling already proves, and what it does not.** `flac_bench.py`
confirmed bit-exactness against a reference decode for **16-bit** files, over
921,600 samples. It **cannot check 24-bit** — its reference packs samples into
`array('h')` and overflows. So for 24-bit material, *listening is the only
verification that exists.*

| | check | pass |
|---|---|---|
| 2.1 | A 16-bit FLAC, start to finish | no stutter, no clicks, no artefacts |
| 2.2 | A **24-bit** FLAC, start to finish | same — and listen harder, nothing else is checking |
| 2.3 | The same 24-bit file on **bars**, then **cassette**, then **spectrum** | clean on all three |
| 2.4 | A 24-bit 48 kHz file specifically | this is the heaviest case the core accepts |

§2.3 is the one that used to fail. Before this release the demanding meters
broke up on 24-bit material and the README told users to switch meters. That
advice has been removed, so if it breaks up here the removal was premature.

---

## 3. The playlist path — now in PSRAM

`pl_text` moved out of CPU RAM into PSRAM. Every playlist name is now read
back over a hardware window rather than from memory. A fault here looks like a
*corrupt playlist*, not like a memory error.

| | check | pass |
|---|---|---|
| 3.1 | Boot with a playlist | track names correct on the player |
| 3.2 | Open the browser (**Select**) and scroll the whole list | every name correct, none truncated, no garbage characters |
| 3.3 | Play a track from the browser | it opens and plays the right track |
| 3.4 | **Load Playlist** from the Analogue menu | full reparse, names still correct |
| 3.5 | Next / previous several times | each resolves the right name |
| 3.6 | The longest playlist you have | exercises the far end of the 12 KB |

---

## 4. Seeking

Three independent bugs were fixed here. Each produced the same symptom, so
testing one does not cover the others.

**4.1 — the reliable repro.** Load an MP3 with **Load MP3**, then load a
playlist, then hold **seek+** on a long FLAC. Before the fix the seek stopped
advancing and jumped backwards, at a point that depended on the *previously
loaded file's size*. It should now run to the end of the track.

**4.2 — same thing with a large FLAC loaded first** instead of an MP3. The
failure point used to move with the file size; it should now not exist.

**4.3 — a long sustained hold** on any FLAC. The hold accelerates to a 30 s
step after about eight repeats, which is where the first bug lived.

**4.4 — short seeks and taps.** Tap left/right to change track, hold briefly
to seek a few seconds, **Select + left/right** for one second. The release
handler was modified, so a tap could in principle have broken.

**4.5 — seek after changing ReplayGain mode** is *not* testable; that feature
is compiled out. Skip it.

---

## 5. Crossfeed and mono — new code in the audio path

**Select + Y** cycles off → low → high → mono. This inserts a filter into
*both* the FLAC and MP3 sample paths, so it is also a risk to §7.

| | check | pass |
|---|---|---|
| 5.1 | Hard-panned track, cycle off → low → high | the sound moves from inside your head toward in front of you; high is obvious |
| 5.2 | Cycle through all four and back | no clicks or pops at any transition |
| 5.3 | **Mono** on a hard-panned track | both ears get the whole mix |
| 5.4 | Crossfeed **high** + **spectrum** meter + a demanding 24-bit FLAC | no break-up — this costs ~1.2% CPU on top of a decoder that only just clears its deadline |
| 5.5 | Crossfeed on an **MP3** | works there too; same code, different caller |

---

## 6. Sleep timer

**Select + Up** cycles off → 15 → 30 → 60 minutes.

| | check | pass |
|---|---|---|
| 6.1 | Cycle it | toast shows each value, then OFF |
| 6.2 | Set 15 minutes and leave it | playback stops at ~15 min |
| 6.3 | Set it, then power cycle | it is **off** again — this must not persist |

6.2 is the only test here that needs patience. The counter is copied verbatim
from the screen-blank timeout, which has shipped since v1.2.0, so the risk is
low — but nothing has watched it actually fire.

---

## 7. MP3 regression

Helix is untouched, but the **crossfeed filter sits in the MP3 push path** and
the volume stage was restructured when ReplayGain was gated out. MP3 has had
almost no deliberate testing this cycle.

| | check | pass |
|---|---|---|
| 7.1 | An MP3 start to finish | clean |
| 7.2 | A VBR MP3 | elapsed clock tracks correctly |
| 7.3 | **Load MP3** on a file not in the playlist | plays; cassette label is **blank**, not the last playlist's name |
| 7.4 | Volume up and down while playing | smooth, no zipper noise |
| 7.5 | 1.2× speed (**hold A**) | no distortion — this limitation was retired in v1.5.1 and should still be gone |

---

## 8. Artwork and resume

Neither was touched directly, but both sit downstream of the playlist text
move and the load path.

| | check | pass |
|---|---|---|
| 8.1 | A track with cover art | art appears, correct image |
| 8.2 | **Select** held | art panel toggles |
| 8.3 | A track with no art | no panel, no error |
| 8.4 | Enable **Resume playback**, play into a track, power cycle | returns to the right track and position |

---

## 9. Controls regression

**X** and **Y** are now guarded by Select, which is new. The plain presses must
still behave.

| | check | pass |
|---|---|---|
| 9.1 | **X** alone | cycles the meter through all twelve |
| 9.2 | **Y** alone | cycles the EQ through all eight |
| 9.3 | **Select + X** | nothing (ReplayGain is compiled out) |
| 9.4 | **Select + L**, **Select + R** | repeat, shuffle |
| 9.5 | **Select + Down** | screen-blank timeout still cycles |
| 9.6 | **B**, **Start** | restart track, stop |

---

## 10. Settings persistence

`interact.json` gained a 13th entry this release. That file class once broke
*all* input on this core, so this section is not optional.

| | check | pass |
|---|---|---|
| 10.1 | Set volume, accent colour, meter, EQ preset; power cycle | all four survive |
| 10.2 | Set crossfeed to **mono**; power cycle | comes back as mono |
| 10.3 | Core Settings menu | lists **Crossfeed**, 0–3, between Color and Repeat |
| 10.4 | Every button still responds after a settings change | the input.json failure mode |

---

## 11. Multi-script text

The font path was not changed, but `glyphbuf` and `fw_mem` are involved in
drawing and the whole UI depends on them.

| | check | pass |
|---|---|---|
| 11.1 | A track with Japanese or accented tags | renders, not `?` |
| 11.2 | Long titles | scroll rather than clip |

---

## 12. If something fails

Diagnostic builds exist for the subsystems most likely to be at fault. Each is
a flag on the build, and **each changes timing**, which has suppressed two
defects on this project — so reach for them only after a symptom is described
precisely, never as a first move.

```
EXTRA_CFLAGS="-DFIFO_DEFICIT=1 -Os" bash fw/build.sh   # audio underruns: L/E/X/T row
EXTRA_CFLAGS="-DPSRAM_TEST=1   -Os" bash fw/build.sh   # PSRAM window self-test at boot
EXTRA_CFLAGS="-DSEEK_TRACE=1   -Os" bash fw/build.sh   # seek recorder, stores only, draws nothing
EXTRA_CFLAGS="-DUI_SHOW_SPEED_DIAG=1 -Os" bash fw/build.sh
```

For audio stutter, **X** on the `FIFO_DEFICIT` row is the number that matters:
it is the longest stretch the FIFO sat empty, in milliseconds, against the
46 ms the buffer holds.

Before reaching for any of them, try **varying one input** — the previous
file, the active meter, the order of operations — and see whether the symptom
moves with it. That is what found the seek wedge after two instrumented builds
had failed to reproduce it.
