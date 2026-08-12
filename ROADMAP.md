# Roadmap

Things worth doing next, with enough context that picking one up doesn't mean
re-deriving why it's on the list.

**Defects come first.** Everything under Enhancements waits until the list above
it is empty, however much more interesting the enhancement is. Within each
section, ordered by value per effort rather than by ambition.

A closed defect moves to **Fixed** at the bottom rather than staying put with a
note — leaving it in place makes the rule above unreadable, since "the list above
is empty" stops meaning anything. The write-ups go with them; they are kept for
the reasoning, not the status.

# Defects

Fixed before anything in Enhancements, regardless of how interesting the
enhancement is.

## Resume only works with the DEFAULT playlist — branch `resume-playlist-name`

Reported by a user against v1.1.0. Resume restores track and position fine with
`playlist.m3u`, but a user running `audiobook.m3u` does not get their place
back. Multiple named playlists is a reasonable thing to do and the feature
should support it.

### What is already established, without a hardware round

**The firmware never names a playlist.** `pl_load()` reads whatever APF has put
in slot 3; nothing in `fw/` contains the string `playlist.m3u` outside a
getting-started caption. So the behaviour CANNOT differ between two playlists
because of our code -- the difference has to be in what APF puts in the slot.

**And `data.json` is the only thing that distinguishes them:**

| slot | declared `filename` | retained across launches? |
|------|--------------------|---------------------------|
| 2 MP3 | none | YES -- proven; resume depends on it |
| 3 Playlist | `playlist.m3u` | reported NO |

So the leading theory is that **a slot with a declared `filename` is reset to
that file at core load, while a slot without one keeps the user's pick.** That
fits both observations exactly and needs no new mechanism to explain.

**NOT yet confirmed.** It is a deduction from one report plus the slot table.
Confirm before building on it.

### The confirmation, which is also a feature

`pl_name_read()` asks 0190 for slot 3's filename and the splash summary now
prints it in place of the word PLAYLIST -- `AUDIOBOOK.M3U  12 TRACKS`. Useful
on its own for anyone running more than one list, and it settles the theory in
a single boot:

- launch after picking `audiobook.m3u`, and the splash says **PLAYLIST.M3U**
  -> the slot was reset, theory confirmed
- it says **AUDIOBOOK.M3U** -> the slot IS retained and the fault is elsewhere,
  theory dead, look at the saved track index instead

### The fix, if the theory holds

**Remove `"filename": "playlist.m3u"` from slot 3**, so it retains the last
pick like the MP3 slot does. That alone breaks first-run behaviour -- a card
with `playlist.m3u` on it would no longer auto-load -- so pair it with: at
boot, if the slot is empty, open `playlist.m3u` into it BY NAME with 0192.
`pl_open_name()` already does that shape of open; it would need a variant
targeting slot 3.

Net effect: the default still loads for a new user, and a user who picks
another list keeps it. **No settings slot, no RTL, no CORE_VERSION bump.**

### Why NOT to store the playlist name in a setting

The obvious alternative is saving the name alongside the resume point. It is
much worse:

- Settings words are 32-bit ints. A name needs four words for sixteen
  characters, and **there are no free slots** -- widening the index means RTL,
  a CORE_VERSION bump and a timing round.
- APF has no directory listing, so a stored name only helps if we can reopen it
  by name -- which is exactly what the cheap fix does anyway, without storing
  anything.

Only worth revisiting if the slot genuinely cannot be made to retain.

### Version

**v1.1.1.** It is a defect in a shipped feature, not new capability: resume is
documented as remembering your place in a playlist and does not, for anyone
not using the default name. Naming the playlist on the splash is a small
visible addition riding along, which is a patch-release judgement call rather
than a minor bump.

# Enhancements

## PNG album art — MEASURED AND DEPRIORITIZED

**Measured 2026-08-04, as this entry asked: 19 `.mp3` files across the card,
`Desktop\songs` and `Music`. Seven carry embedded art. All seven are
`image/jpeg`. Zero PNG.**

A 19-file sample is small and one PNG-heavy album would move it, so this is a
deprioritisation rather than a deletion. But the cost below is a ~32 KB inflate
window against a RAM budget that does not obviously have one, and nothing in the
library being carried around would benefit. Re-measure before picking it up
(`tools/` has no scanner; the throwaway one read APIC frame MIME types straight
out of the ID3v2 tag, ~80 lines, and handles the v2.2/v2.3/v2.4 size encodings).

The design notes below stand for whenever that changes.

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

## Read this before adding any high-fidelity format

Two facts bound the whole question, and neither is CPU speed.

**The output is fixed at ~16-bit / 48 kHz.** `sound_i2s` hands the APF audio
interface 15 magnitude bits plus sign at 48 kHz. That is the Pocket's interface,
not a choice this core makes and not something a better source format can lift.
Every format is downconverted to that before it reaches the DAC, so lossless
audio arrives at the same 16/48 output as a 320 kbps MP3, through a handheld
headphone amplifier. The honest expected gain is small.

**The buffering runs the wrong way as bitrate rises.** The compressed ring is a
fixed 32 KB, so it holds 0.82 s at MP3's 320 kbps but only 0.29 s at FLAC's
~900 kbps: protection against an SD stall falls ~2.8x at the same moment demand
rises ~2.8x. Stalls are not hypothetical -- APF drops its fragment cache on every
track change, and random reads are what made the old size probe cost 480 ms.

**And the margin cannot be bought back in RAM.** Restoring 0.8 s of protection at
FLAC rates needs a ~90 KB ring. There is ~76 KB of heap, and a FLAC decoder wants
~32 KB of it for a decoded block. The room does not exist without taking it from
something else.

So the risk is a dropout in exchange for a difference most listeners would not
hear on this hardware. That trade is bad, and it is the reason the EQ below is
ranked first: EQ improves what you actually hear *within* the fixed ceiling, costs
no CPU and carries no throughput risk at all.

None of this makes lossless impossible. It means the case for it should be made
with the throughput measurement in hand, and with eyes open about the payoff.

## FLAC — plausible, gated on one measurement

The decode itself is the easy part: FLAC is Rice decoding plus an LPC filter,
with no MDCT and no synthesis filterbank, so it is materially cheaper than MP3
and integer throughout. No FPU needed, which is the usual killer on a soft core.

**The binding constraint is SD throughput, and we have not measured it.** FLAC
runs ~700-1000 kbps against MP3's 128-320, so roughly 112 KB/s sustained where
40 KB/s is the most this core has ever had to hold. 40 KB/s is proven; 112 KB/s
is unknown. That number decides the feature and nothing else should be built
until it exists.

Measuring it is cheap: time N sequential `REFILL_CHUNK` reads on the existing
path and divide. Note that the figure must come from SEQUENTIAL reads — the
~480 ms the old size probe spent on ~20 reads is not representative, because
those were random offsets that made APF re-walk the cluster chain each time.

RAM is tight but survivable: a 4096-sample stereo block is 32 KB decoded, against
~76 KB of heap of which Helix currently takes ~34. Only one decoder need be
resident, so this is a swap rather than an addition. Limit scope to 16-bit /
44.1 kHz — 24/96 is not worth attempting.

## AAC — the hardest of the three, and the decoder is not why

The decoder is the good news. Helix ships a fixed-point AAC decoder alongside the
MP3 one, same lineage, same RPSL license already vendored here, same integration
shape. That part is close to a solved problem.

Two things make it hard anyway.

**CPU headroom.** AAC-LC decode is comparable to MP3 and often somewhat above it,
against 24% free at worst case. It may simply not fit at 60 MHz. There is a real
escape route — STAGE0 measured VexRiscv's fmax at 141 MHz, so the CPU itself has
plenty of room — but clk_sys is not the CPU's private clock, and moving it drags
the video, SDRAM and bridge timing along with it. Not free.

**The container, which is the actual cost.** AAC in the wild means `.m4a`, i.e.
MP4: atom parsing, sample tables, and random access into the file to resolve
them. Random access is the expensive operation on this platform, for the reason
noted above. Raw ADTS `.aac` streams would be trivial by comparison and are rare
in real libraries, so supporting only those would be a feature almost nobody
could use.

Reasonable order if all three are wanted: EQ (self-contained, RTL, no format
risk), then FLAC (one measurement decides it), then AAC.

## The persisted-settings register file is FULL

`mp3_soc.v` takes the settings index as `set_idx <= dDAT_MOSI[2:0]`. Three bits,
eight slots, and all eight are spoken for: volume, accent, repeat, shuffle, art,
meter, EQ, blank. **Any new remembered setting needs that widened to 4 bits.**

The edit itself is one line plus the `set_reg` array bound. What it costs is the
iteration model: everything since rev 20 has been firmware-only, rebuilt in
seconds, and this drops back to a full Quartus compile, a `CORE_VERSION` bump
and a timing-closure round. So widen it ONCE, to 16 slots, and land anything
that needs a slot in the same pass rather than paying that twice.

Currently blocks resume-position below. **Variable speed deliberately avoids it
by not persisting** — see that entry; deciding a setting does not need to be
remembered is the cheapest way past this.

## Resume where you left off (requested 2026-08-10)

Restore the playlist position, and optionally the offset within the track, on
next launch. The better of the two requests from that day: it works exactly as
asked and fits the architecture.

Persistence goes through the `interact.json` vars APF stores in its own file —
the one mechanism here that has never touched the user's music, unlike the two
that did. A position update is two MMIO writes and no SD access, so ticking it
once a second is free. Restoring is `load_track()` plus a seek, and both the
seek and the frame resync already exist.

Needs two slots (track index, byte offset) and probably a third for an on/off
switch, since resuming mid-track surprises people who did not ask for it. See
the register-file item above.

Three things to design around:

- **The `.m3u` may have changed.** Store a hash of the track's filename beside
  the index and fall back to track 1 when it does not match, or the saved index
  silently points at a different track.
- **APF may not flush its persist file on a hard power-off**, so the last few
  seconds of position can be lost. Acceptable; do not build a mechanism to
  defeat it.
- Resuming into the middle of a file must go through the same start path every
  other route uses — see the reload handler's note about that being the whole
  reason track changes stopped clicking.

Roughly 5-8 hours plus the compile: two hardware sessions.

## Variable playback speed — 1.2x is reachable, 2x is NOT (requested 2026-08-10)

The mechanism already exists. `R_PCM_RATE` sets the FIFO drain rate and the
firmware already computes it per file, so scaling it is one line. That buys
speed WITH pitch shift — the chipmunk effect — which is tolerable for spoken
word and unacceptable for music.

**The decoder is the ceiling, and it rules out the speeds people actually ask
for.** Playing at N x means decoding N x as many frames per second, against the
Stage 0 measurements of what decode costs of the 60 MHz budget:

| speed | typical (128k/44.1) | worst case (320k/48) |
|-------|---------------------|----------------------|
| 1x    | 33.9 MHz            | 45.7 MHz             |
| 1.2x  | 40.7                | 54.8 — tight         |
| 1.5x  | 50.9                | 68.6 — OVER          |
| 2x    | 67.8 — OVER         | 91.4                 |
| 3x    | 102                 | 137                  |

So 1.2x works, 1.5x works on ordinary files and breaks on 320 kbps, and 2x/3x
cannot work at all. There is no clocking out of it either: clk_sys is 60 MHz
because the worst slack observed across fits implied fmax around 66, which is
why 70 was judged a coin flip (see the rev 13 entry).

Pitch preservation would need WSOLA or a phase vocoder on top of that, adding
DSP to the budget that is already the binding constraint — and it would eat the
same headroom the speed-up needs. Not viable as the design stands.

**If this is built, build it for spoken word and say so**: 1.2x/1.5x only,
pitch-shifted, and leave 2x off the list rather than shipping something that
stutters.

### BUILT AND PARKED on branch `speed-1.2x` (2026-08-10)

Implemented, HW-tested, **not merged**. User verdict: "works, but is a little
rough." Two open defects, and the first is the reason it is parked.

**1. Seek/elapsed wrong at 1.2x on some files — FIXED 2026-08-11**, and it was
not a speed bug at all. See the Fixed entry below; the short version is that
three files were CBR with no Xing header, so the exact rate was available and
being discarded in favour of a measurement. HW-confirmed at 1.2x and 1.0x.

**2. Occasional distortion when engaging 1.2x — STILL OPEN.** Consistent with
the budget table above: 1.2x needs 40.7 MHz typical and 54.8 MHz worst case of
60, so a dense passage can miss and underrun the FIFO. Likely inherent, and a
disclosable limitation rather than a fault -- which is what makes the
experimental label below honest rather than a hedge for something broken.

**A method note worth more than either defect.** Before the hardware test I
asserted that speed could not disturb the elapsed clock, having grepped
`ui_sec *=` and found only two resets and the seek path — all file-domain.
But the steady-state update is `ui_sec++` at line 2497, driven by decoded
frames, and an assignment search cannot find an increment. The check was
structurally incapable of contradicting the conclusion it was run to test,
which is the same failure as seeding a simulation at its own fixed point.
**When a search comes back clean, ask what shape of code it could not have
matched.**

### Shipping it as EXPERIMENTAL is on the table (user, 2026-08-11)

If 1.2x never reaches "correct on every file", the user may ship it labelled
an experimental feature rather than hold it back. Their call, not to be made
by whoever picks this up. What that would require:

- **README says so plainly** -- experimental, pitch rises with speed, seek can
  misbehave on some files, and it is meant for spoken word.
- **Off by default and unpersisted**, which it already is: hold A each session.
  Nothing about a bad session survives into the next one.
- **Distortion is disclosed, not fixed.** 1.2x needs up to 54.8 MHz of the
  60 available, so a dense passage can underrun. That is the budget, not a bug.

The seek defect is the one that decides it. Distortion is a known cost a user
can hear and accept; a clock that walks backwards looks broken.

### Capturing the seek defect properly -- do this BEFORE attempting a fix

`UI_SHOW_SPEED_DIAG 1` in `fw/player.c` brings back the row:

    1.0x T241 M16003 R16003 K2048

For each file that misbehaves, note T FIRST, then M and R over ten seconds at
1.0x, then the same held at 1.2x.

- **T non-zero** kills the leading theory outright: the file has a Xing header,
  `ui_byte_rate()` returns the exact rate, and `meas_rate` is never consulted.
- **T zero** is the path the 2026-08-10 hold-to-seek bug lived in, and "wrong on
  some files, fine on others" is that bug's signature. If M drifts at 1.2x while
  holding steady at 1.0x on the SAME file, that is the fault located.

Also record the file's bitrate and whether it is CBR or VBR. Three files that
fail and three that do not is worth more than a long session with one.

### Agreed design (2026-08-10): hold A, no persistence

Hold **A** for 1.2x, hold again for normal. Deliberately NOT a Core Setting and
deliberately not remembered — which is what makes this cheap. No persist slot
means no register-file widen, so no Quartus compile, no `CORE_VERSION` bump and
no timing-closure round: **firmware only, rebuilt in seconds.** Resetting to
normal speed on every launch is also the right default for something you engage
per-listen rather than per-library.

A currently fires on the EDGE (press), because it is the one control with no
hold action. Adding one means moving it to resolve on RELEASE, which is already
the house convention -- see the comment above `poll_input()`: Left/Right and
Select "both resolve on RELEASE, so the tap action cannot fire and then be
followed by the hold action for the same press." Reuse that discriminator
(`PL_HOLD_MS`, 400 ms) rather than inventing a second one, or a long press will
pause AND change speed.

Two things to get right: the toast has to say which speed is now active, since
an unlabelled 1.2x just sounds like a bad file; and the seek arithmetic reads
`ui_byte_rate()`, so confirm a speed change does not make the elapsed clock or
the seek distance wrong -- that feedback loop is what the FIXED entry on seek
below is about.

Roughly 2 hours, one hardware test.

## Scrobble log (.scrobbler.log) — requested by a user 2026-08-11

"Could the core write played tracks to a log file like Rockbox does, so I can
feed it to a last.fm scrobbler?" Rockbox writes `.scrobbler.log`: a header plus
one tab-separated line per track (artist, album, title, track number, length, a
listened/skipped flag, and a Unix timestamp). Check the format against
Rockbox's own docs before writing any of it — do not reconstruct it from
memory.

**The timestamp problem is already solved, which was the surprise.** The Pocket
exposes a real-time clock and the frozen shell already carries it:
`rtc_epoch_seconds`, `rtc_date_bcd`, `rtc_time_bcd` and `rtc_valid` are wires in
`core_top.v`, connected to `apf_top` and used by nothing here. A Unix epoch
second is exactly what the format wants. Routing it to the SoC is one more MMIO
register — an RTL change, so it should ride along with the settings-register
widen rather than pay for its own compile.

Everything else about the feature is easy. **The write is not.**

### The blocker is 0184, and it is a POLICY decision, not an engineering one

There is exactly one way for a Pocket core to write a file: the `0184` dataslot
write. **That is the mechanism that destroyed the user's music library three
times**, and it is off permanently by their decision. Read the forensics under
Fixed before going near this.

The root cause WAS found and fixed — this core was writing its 0190 response
struct over APF's dataslot ID/size table at word 0, so APF read a garbage size
and applied it to whatever file occupied the MP3 slot. So the path is not
inherently unsafe any more. But "we understand why it broke last time" is not
the same as "it is safe to point at the user's library again", and that call
belongs to the user, not to whoever picks this ticket up.

If it is ever attempted:

- **APF cannot CREATE a file.** A placeholder `scrobbler.log` has to ship in
  `Assets/mp3player/common/` and be declared as its own slot. Design for that;
  do not assume create-on-first-write.
- **Never point the write at the MP3 slot.** A separate slot, and assert the
  slot id at the call site rather than trusting a variable.
- **A write drops APF's fragment cache** for the MP3 slot, so the next refill
  re-walks the cluster chain — an audible tic. Defer writes to paused, stopped
  or menu-open, exactly as the settings code learned to.
- **Test against a throwaway card with copied music**, never the real library.

A safer half-measure worth considering first: keep the log in RAM and show it on
screen, so the feature can be proven end to end — timestamps, track identity,
the listened/skipped rule — with nothing written to the card at all. Only the
final step needs 0184.

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

## Tune the EQ presets by ear

The only item on this list that needs a person rather than a change. The curves
are numerically verified and each shape matches the name it carries, but nobody
has judged whether ROCK actually *sounds* like rock. Expect to adjust the gains
in `tools/gen_eq_coeffs.py` and regenerate; the engine itself should not need
touching.

## Cleanup: retire what the click hunt left behind

Fixing the track-change click took many attempts, and several mechanisms
survive that may no longer earn their place — the decoder reservoir warm-up and
the async EOF discovery are the likeliest candidates, now that every cold load
finishes through the reposition body. Others are load-bearing: the FIFO
glide/fade pair, the cut-at-press transition, and the confirmed-EOF guard all
fix real faults. Worth a pass, one at a time, each verified on hardware —
not a bulk tidy.

# Fixed

Kept for the reasoning, not the status. Nothing here is outstanding.

## Shuffle produced the same order every boot — FIXED 2026-08-10

Asked as a question -- "is shuffle actually shuffling?" -- and the answer was
yes and no. pl_reorder() is a correct Fisher-Yates, so every permutation is
equally likely. But `pl_rng` is a static 1, and the ONLY thing that ever seeded
it from cycles() was the Select+R toggle handler.

Shuffle persists across sessions. So a user who turns it on once and then simply
leaves it on gets pl_load() -> pl_reorder() running on seed 1 at every launch,
and the identical permutation forever. Verified by running the same shuffle
three times from seed 1: byte-for-byte the same order.

pl_rng is now seeded in pl_load() as well, immediately before pl_reorder(). That
point has real entropy -- the playlist reads just above walk the card's cluster
chain and their timing varies run to run, so the cycle counter there is not the
fixed value it would be earlier in an otherwise deterministic boot.

All three pl_reorder() call sites are covered: the toggle seeds itself, pl_load
now seeds, and the wrap-around reshuffle in pl_advance_auto inherits a state
already advanced by every pl_rand() since.

Worth noting what made it invisible: it is not a shuffle that fails to shuffle,
which anyone would spot. It is a shuffle that works perfectly and produces one
answer, and the only way to see it is to notice the SAME wrong-feeling order
twice across power cycles.

## Holding seek walked the clock backwards — FIXED, HW-confirmed 2026-08-10

Reported after seek acceleration went in: hold the seek and eventually the
position sticks, the clock counts DOWN, it cannot be recovered, and the track
then jumps to the next one. Only on some songs.

The cause was a feedback loop. `ui_sec` is computed FROM `meas_rate` on every
seek, and `meas_rate` was recomputed FROM `ui_sec`:

    meas_rate = (file_pos - buffered - audio_start) / sec

Taking both as absolutes made each seek feed the other. Held down at four
repeats a second the pair diverged, the rate inflated, and the clock derived
from it collapsed. `track_secs` short-circuits that branch entirely, which is
why files WITH a Xing header were fine and files without were not -- the "only
some songs" was the whole diagnosis, and it took too long to act on.

`meas_rate` now measures bytes and seconds across the SAME interval, with the
baseline reset whenever a seek moves the position. It is observed throughput
again rather than a number defined in terms of its own output.

### The process failure is the more useful record

Five attempts. Four were wrong, and each was shipped:

1. clamp the forward seek — real gap, not the fault
2. guard against no-op repositions — real waste, not the fault
3. derive the limit from a rate that cannot move — real hazard, not the fault
4. window the rate measurement — THE FIX, and shipped labelled "not claimed as
   the fix" because the model had not reproduced it
5. stop, and build a diagnostic readout

Every one of the first four was defensible in isolation, which is exactly what
made it easy to keep going. The simulations were the problem: each was seeded
with the true byte rate, and a feedback loop seeded at its own fixed point is
stable. The models kept agreeing with themselves and were read as evidence of
absence. A model that cannot fail is not a test.

The fix was already on the card, unrecognised, inside the diagnostic build --
found only because the user tested that build and reported the fault gone.

## Core Settings shows numbers, not names — MEASURED, not fixable as designed

The menu renders Color, Repeat, Meter and EQ preset as numeric sliders with a
value bubble that overlaps its label. `interact.json` has a `list` type that
would show named options instead, which is plainly what these four want.

It was tried, and it broke settings persistence. The card's own persist file
records the experiment cleanly, four treatments against three controls:

| variable            | type    | before -> during |
|---------------------|---------|------------------|
| Volume              | slider  | 35 -> 40, saved  |
| Shuffle, Album art  | check   | 1 -> 1, held     |
| Color               | list    | 3 -> 0, stuck    |
| Repeat              | list    | 1 -> 0, stuck    |
| EQ preset           | list    | 2 -> 0, stuck    |
| Meter               | list    | 0 -> 0, stuck    |

Volume changed AND saved during the same window the four lists sat dead at
zero. So this is not a formatting mistake in the option values: `slider_u32`
and `check` accept a value written by the CORE and persist it, and `list` does
not. APF writes a menu selection to the core but never reads the core's own
change back.

Every setting here is changeable with buttons, so a list would give a menu whose
value silently stops matching reality the moment L/R or Y is pressed. That is
worse than an ugly bubble, so the sliders stay.

It also cost more than a revert. The Pocket had already written `"type":"list"`
into interact_persist.json, so restoring interact.json was not enough on its
own -- the persist file had to be deleted before persistence recovered. A data
file change is not automatically cheap to undo once the OS has stored state
based on it.

Only route worth trying later, on a card that can be reset: a variable that the
core never writes back, driven from the menu alone. None of the current seven
qualifies.

## Seek wrong at 1.2x on three files — FIXED 2026-08-11, and it was never about speed

The failures were exactly three of eleven files, and `tools/xing_check.py`
named the shape they share: **no Xing/Info/VBRI header, and CBR**. Stockholm
Syndrome (256), Stone Temple Pilots (128), Widespread Panic (160). Every file
WITH a frame count was fine at any speed.

That is the one combination the rate logic handled worst. `ui_byte_rate()` read

    track_secs ? exact : meas_rate ? meas_rate : bytes_per_sec

so a headerless file fell to `meas_rate`, a throughput ESTIMATE -- while for CBR
the first frame's bitrate is the byte rate, exactly. A 5 s seek on the 128 kbps
file is precisely 80000 bytes. meas_rate had to converge on that number, and
1.2x perturbed the convergence.

`vbr_seen` is now set the moment a decoded frame disagrees with the first
frame's bitrate. Until then the exact value is used, and `meas_rate` serves only
what it was ever for: a headerless VBR file, where no constant exists to read.

**Two things worth keeping.**

The hold-to-seek bug fixed on 2026-08-10 lived in this same branch, and
windowing the measurement made it *survivable* rather than removing the need to
measure at all. 1.2x did not introduce a defect; it exposed one that had always
been there, latent, on those same three files at normal speed.

And the diagnosis cost one round because the prediction was made falsifiable
first: "the failures are exactly these three files and no others." A file with a
header failing would have killed the theory outright. Compare the four wrong
fixes on 2026-08-10, each shipped on a plausible story with nothing that could
have contradicted it.

## Hiding a Core Settings entry — MEASURED, not possible

`interact.json` has an `enabled` field, and `"enabled": false` looked like the
way to keep a variable persisted without showing it in the menu. The saved
resume position wants exactly that: it is storage, not a setting, and as a
draggable slider full of a huge number it is noise among real options.

Tested alone on 2026-08-11, against a working resume so the result could not be
confused with anything else. **It disables rather than hides.** The entry stays
in the menu, greyed out, AND the value stops working -- the persist file held
352321669 before the test and exactly 352321669 after a play session, when it
should have tracked the position every second.

So APF stops reading a disabled variable back from the core. That is the SAME
failure as the `list` type above: two different fields, one underlying rule --

  **A variable APF will not read back cannot be persisted from the core.**
  Only `slider_u32` and `check`, enabled and visible, accept a core-written
  value. There is no way to have persistence without a menu entry.

Better than the `list` attempt in one respect: the persist file was not
poisoned. Value and type survived intact, so reverting `enabled` to true was
enough and the file did not have to be deleted.

**What this closes.** The menu cannot be tidied by hiding. Color, Repeat, Meter
and EQ preset are enumerations rendered as meaningless numeric sliders and
would all read better gone, but the only way to remove an entry is to remove
the variable -- which costs its persistence, exactly as it did for album art
and screen blank. That is a real trade per setting, not a free cleanup, and
each one has a button with a named toast already. Worth considering
deliberately, one at a time; not worth doing wholesale.

## Button labels in input.json — WORKING, after three wrong combinations

The Analogue controls screen now names all eight buttons. Getting there took
four attempts because the first three each changed two things at once:

| type      | names | analog_stick | result                        |
|-----------|-------|--------------|-------------------------------|
| `default` | yes   | yes          | ALL INPUT DEAD                |
| `gamepad` | yes   | yes          | input fine, Controls blank    |
| `gamepad` | no    | yes          | input fine, Controls blank    |
| `default` | yes   | **no**       | **works**                     |

So the fault was never the `"default"` type the spec requires — it was MIXING
named `{id,name,key}` mappings with the `{type:"analog_stick"}` entry in one
`mappings` array. The docs describe each separately and never together, and the
Pocket will not accept both.

The cost is the analog-stick-to-dpad mapping, which is gone. On the dock a
connected controller's left stick no longer acts as a d-pad; its own d-pad is
unaffected. Worth it for a player whose bindings are not guessable.

Also worth recording: the Controls screen was blank BEFORE any of this, and a
misdiagnosis blamed the labels for it. It was blank because no core here has
ever supplied button names — every other core in the workspace ships the same
minimal input.json and would be equally blank.

## Background ramp: dithered, and tinted from the accent — HW-confirmed 2026-08-05

User asked for a smoother gradient and for it to pick up the accent colour.
Both wanted the same thing fixed.

**40 bands were already 13 colours.** Between 0x2124 and black, RGB565 has 10
green levels and 5 red/blue, so the 40 bands collapse to 13 distinct colours
holding ~28 rows each — ~111 panel rows after the 4x scale. Rendering 40 bands
against 360 side by side showed them identical: **more bands cannot help**, the
colour space has no values in between. Dither is the only lever, one rect per
row, alternating adjacent levels. Texture is 4 panel rows against a band's ~110.

**The same shortage is why the tint needed luma headroom.** At luma 35 there is
no room for hue: cream quantised to exactly the old neutral and several accents
landed within one level of it. Normalising to luma 45, half-saturated, gives 10
distinct ramp tops from 12 accents (mint/seafoam and blush/cream share, adjacent
hues either way). Normalising to a FIXED luma rather than scaling the accent is
what keeps text contrast constant as the palette cycles.

**`ui_grad_at(y)` is the structural point.** One function owns what colour the
background is at a row. Nine call sites previously rebuilt it with their own
`ui_mix()` arithmetic — harmless against flat bands, but against a dithered ramp
they would have disagreed with what was drawn and left patches behind text, the
toast and the rounded corners. Anything that needs a background colour must come
through it.

Two coupling bugs found while wiring, both silent: the settings restore set
`ui_accent` without the ramp (a saved colour would boot onto the default
background until L/R was pressed — fixed by deriving it once after
`settings_load()`, which also covers the early-return defaults path), and an
accent change invalidated individual elements but never the background. The
latter now forces a full repaint, which is the one place this costs anything —
~360 rects plus chrome, pushed while the decoder is idle. Listen there first if
a colour change ever ticks.

`tools/gradient_preview.py` renders the options at 1:1 AND at 4x vertical, the
latter being what the panel actually does. Judging dither at 1:1 flatters it.

## Cover art soft, large covers invisible, tags missing — FIXED, HW-confirmed 2026-08-05

Three faults in the same load path, all found by measuring the user's real
library with `tools/library_check.py` rather than by reasoning about typical
files. That tool now predicts what the core will do with a card before booting
it, and its limits are read out of `fw/art.inc` so it cannot drift.

**Art was always decoded at 1/8.** picojpeg's reduce mode yields one pixel per
8x8 block, which is sharp for a big cover and useless for a small one: measured,
300 px became 37x37 and 455 px became 56x56, both then MAGNIFIED to the 92 px
panel. Mode is now chosen per image. The gate pays for itself — full decode
costs an IDCT per block, so it is affordable only on small images, which are
exactly the ones reduce mode fails; anything from 736 px up keeps the cheap
path. Scaling is a box filter now, not a nearest-pixel pick.

**Large covers were rejected outright.** `ART_MAX_BYTES` was 384 KB, commented as
refusing the absurd, and was refusing the good art: a 1494x1497 cover is an
847 KB frame. Symptom was a track with no art and nothing to say why. The cap
never protected memory — the image streams through a window and is never held
whole. Raised to 2 MB.

**Tags behind artwork were unreachable.** Three tracks put APIC FIRST, with
41 KB, 34 KB and 847 KB of picture ahead of every text frame. The in-memory
parser stops at the first frame overrunning what is loaded, and the fallback
caps at the 32 KB ring, so no amount of pulling more in could reach them.
`id3_walk_collect()` walks the tag off the card, skipping a picture by
arithmetic — cost scales with the NUMBER of frames, not the tag size, and a
sliding 512-byte window keeps it to 3 reads. Runs only where the cheap path
failed. Separately, UTF-16 text frames now decode instead of being refused.

**Two bugs were caught by modelling before they reached hardware**, which is the
part worth keeping. `tools/art_scale_model.py` found that accumulating into 92
rows from a source shorter than 92 lets one MCU row span 23 accumulator slots
against 20 — two rows silently adding into each other. And modelling the tag
walk against real files found an off-by-one that put the next frame's first
letter on the end of every walked title. Neither would have announced itself.

**A latent bug surfaced on the way:** block offsets were `(by*bxn + bx)*64`,
which agrees with picojpeg for H1V1/H2V1/H2V2 but not H1V2, where the second
block is written to 128 (`picojpeg.c`: `copyY(0)`, `copyY(128)`).

### The open item: loads are slower, and the cause is NOT what it looks like

User's verdict: "a tad longer, but not unbearable". Accepted for now. The cause
splits in two and it is worth not conflating them.

Small covers (300-500 px, eight of the ten on the test card) read the SAME bytes
as before — 15 to 41 KB — and are slower purely because of the IDCT. Buying that
back means a scaled IDCT inside picojpeg: a 4x4 transform from the top-left
coefficients gives half scale at roughly a quarter the cost, and a 455 px cover
at half scale is still 227 px, comfortably above the panel. That is surgery on
third-party code and should not be attempted on a hunch.

The 847 KB cover is a different problem: it streams through a 4 KB buffer
(`_tag_size` in `fw/link.ld`), so it costs **207 separate target reads**. Raising
that buffer is a one-line change and should be the FIRST thing tried, but it
needs confirming that a target read larger than 4 KB works — no core here has
issued one, so it is unproven rather than safe.

**Select+Start now shows the load breakdown** (head, size, art, prefill, total,
in ms). Those phases have been measured since the click hunt and never
displayed, so every question about a slow load was previously answered by
estimating. Read them before touching either lever.

## Text rendered thin and hazy — FIXED, HW-confirmed 2026-08-05

Reported as "the mp3 information could be in a sharper more crisp font". The
typeface was not the problem. Two independent defects were, both of which made
every glyph on screen lose light it was owed.

**The blend was gamma-wrong.** `mp3_fb.sv` interpolated foreground to background
linearly over RGB565 code values. RGB565 is sRGB — gamma-encoded, roughly a 2.2
power law — so averaging code values does not average *light*. A pixel at half
coverage, weighted 8/16, emits about 22% of the foreground rather than 50%.
Every partially covered pixel was therefore under-lit. On light type over a dark
ramp that reads as thin, unevenly weighted stems: the perceptual signature is
"blurry", never "too dark", which is why it survived this long.

Fixed by replacing the coverage-as-weight assumption with a 16-entry table
fitted to the palette this core actually draws, over the background it actually
draws on, luma-weighted — `tools/gen_text_gamma.py`, which also has a `--check`
mode asserting the RTL still carries the derived table. RMS error per coverage
level drops between 3× and 16×. The largest corrections are at LOW coverage
(1→4, 2→6), which is the faint edge pixel that carries perceived sharpness.
Cost: 16 five-bit constants in logic. No M10K, no DSP, no new multiplier.

**The atlas quantiser threw away the faint pixels.** `gen_font_rom.py` reduced
8-bit coverage to 4-bit with `>> 4`, wrong at both ends: it saturates from 240
up, so near-solid pixels stored as fully solid, and it floors everything under
16 to zero, so the faintest coverage was *deleted*. Those are exactly the pixels
the eye integrates into a smooth stem edge. Now rounds properly. 27% of atlas
words changed; the ROM is the same 3040 words and the same 10 M10K.

**Glyph advances are byte-identical**, verified by diffing `font_metrics.h`
across the regeneration — the advance is measured from the 8-bit raster before
quantisation, so nothing about text layout, clipping, or the toast
end-position logic moves. That is why this was safe to take in one step.

**Weight and size were evaluated and deliberately not changed.** Medium 500 and
Bold 700 and a 16px body were all rendered through a bit-exact model of the
compose path (`tools/font_preview.py`, `--focus` for the body-text case). Bold
closes the counters in `0 b p`; Medium reads hazier on a dark background, which
is the fault being fixed; 16px widens every glyph and would move layout. Those
three change advances and carry layout risk the two correctness fixes do not.

**Confirmed on hardware 2026-08-05.** The same build also carries the guard-band
removal, and `docs/screenshot.png` now proves that one numerically rather than by
eye: row 0 of a native 400×360 capture reads (33, 36, 33), the top of the ramp,
where a guard band would have left pure black.

**What this does NOT fix: the 2× title.** It is a magnified 16px master, and no
blend or raster change makes that sharp. The only real fix is a natively-sized
large atlas, which does not fit — a 32px 4bpp atlas needs ~43 more M10K and 8
are free. Moving the atlas to SDRAM (~48 KB, against 360 KB used of 32 MB) would
buy native large text *and* hand back the 10 M10K the current atlas occupies,
which is the constraint binding everything else. Not attempted; it is a project,
not a change.

## Playlist stops dead at the first bad filename — FIXED, HW-confirmed

Fixed in `5986761`, confirmed on hardware 2026-08-04: the core boots and plays
past a bad entry. A follow-up (`95e9eda`) stops a bad entry counting toward the
track total — see the note at the end of this section.

**Symptom.** A single misspelled or missing entry in the `.m3u` failed the whole
load. If it was the first entry, the core never reached the player at all.

**What it was.** `pl_play_at()` made exactly one attempt and no caller retried,
so the failure propagated out to the boot path.

**What replaced it.** `pl_play_span(pos, dir, span)` in
[fw/playlist.inc](fw/playlist.inc) walks past entries that will not open, bounded
by the number of positions it may visit. Failures are classified rather than
lumped together: a `0192` errcode 4 or an over-long name is permanent and the
entry is marked dead for the session (indexed by *file* index, so it survives a
reshuffle); a missing 0190 template or a 0192 that never answers is not about the
entry at all and stops the walk — each timeout is a full 3 s, so walking 128 of
them would freeze the core for six minutes; anything else is transient and is
stepped over without being condemned. One toast per walk, not one per bad entry.

Repeat OFF passes a shorter span so a missing *last* entry ends the playlist
instead of wrapping round to track 1. Skipping a bad file may cost a track; it
must not silently change what the repeat mode does. The first version had this
wrong and the exhaustive check caught it.

**The boot symptom was half a UI bug, and that part is worth remembering.**
`ui_idle_screen()` is `ui_splash()` plus three lines of instructions — the same
gradient and title as the boot screen — and the main loop does
`if (idle) continue;` *before* it reaches `ui_draw_dynamic()`, so **no toast is
ever painted in idle mode**. The core had left the boot screen; the replacement
was indistinguishable from it and the explanation was raised into a void. It now
takes a reason line and paints it statically.

**Fully confirmed on hardware 2026-08-05**, including mistyped entries in any
position. One bad line costs one track and nothing else.

### The track count, and the limit that is deliberate

A bad entry used to count toward the total: five lines with one typo displayed
as five tracks. `95e9eda` derives both the total and the position from the
**live** entries — `pl_count` minus the dead set, and the position's rank among
live entries. Both halves must count the same way or the position can exceed the
total; that is checked exhaustively over every list length, dead-set and shuffle.

**The count is exact for entries something has tried, and optimistic beyond
them.** This is a property of the platform, not a shortcut: nothing in APF
answers "does this file exist". The only test is `0192`, which *opens* the file,
costs a directory walk and switches the MP3 slot as a side effect. So a typo at
or before the first playable track is found during the boot walk and the total is
right from the first frame; a typo further down reads high until the walk reaches
it, then corrects.

**Decided 2026-08-04: leave it self-correcting.** The alternative — probing every
entry at load — buys an exact count from the first frame at the cost of one file
open per entry on every playlist load (~1 s behind the splash for a 30-track
list, worse for longer). Not worth a boot delay on every launch for a number that
converges on its own. A third option, probing into a spare scratch slot so it
could run during playback, was set aside because it depends on unverified APF
behavior (whether `0192` can open into an empty `deferload` slot) and this
project has been bitten before by building on unconfirmed APF assumptions.

If it ever becomes worth revisiting, the scratch-slot route is the one to prove
first — it is the only one with no boot cost.

## Preset EQ — BUILT AND SHIPPED (rev 18)

Eight presets on `Y`, `Select+Y` to reverse, the preset named in the mode row,
choice persisted in the settings record. Coefficients generated and verified in
python, the RTL checked bit-exact against that model over 576 samples on all 8
presets and both channels, M10K unchanged at 300/308.

Two things the design worried about, resolved by measurement rather than
assumption:

- **Preset changes do not click.** The design called for a ~4 ms duck and said
  "do not ship it and hope". Extended hardware use produced no audible click, so
  the duck is not built. See `docs/EQ_DESIGN.md`.
- **The curve-over-the-meter display was built and rejected** by the user;
  the mode row names the preset instead.

Left undone: the presets are numerically correct but have never been tuned by
ear. That is a listening pass, not an engineering one.

The design notes below are kept for the reasoning.

### Original design entry

Worth doing, and the obvious implementation is the wrong one.

**Not in firmware.** The free CPU budget is 8.7 M instr/s at worst case (36.4 M
total at 60 MHz and 1.65 CPI, less the 27.7 M Helix needs for 320 kbps / 48 kHz).
A 5-band stereo biquad at 44.1 kHz is 441,000 biquad evaluations a second, and a
Direct Form I biquad on RV32 is ~15-20 instructions once loads, stores and
saturation are counted — 6.6 to 8.8 M instr/s, i.e. essentially all of it. It
would also be spending the exact budget that keeps the decoder fed, which is the
one way to bring the audio tics back.

**In the RTL it is nearly free.** Output is 48 kHz and clk_sys is 60 MHz, so there
are ~1250 clocks per output sample. Twenty biquads (10 bands x 2 channels) at 5
MACs each is ~100 MACs, which one pipelined multiplier retires in ~150 cycles —
about 12% of the time available. Cost is one DSP block, a small coefficient ROM
and 20 x 4 words of state. Zero CPU, and the decoder never knows it exists.

Where it goes: between `pcm_fifo` and `sound_i2s`, which is already a clean
16-bit stereo hand-off. Presets rather than per-band control keeps it to a ROM of
coefficient sets and one selector.

Two cautions. This is the first change in a long time that needs a **Quartus
recompile** rather than a firmware rebuild, so budget a timing closure round.
And generate the coefficients offline in python and paste the table in — a
hand-written filter table is exactly the mistake that cost a hardware round on
the VU needle's sine table.

Full design outline, including the fixed-point formats, the 97%-full block RAM
constraint and the verification plan: **[docs/EQ_DESIGN.md](docs/EQ_DESIGN.md)**.

## Settings persistence — SOLVED via interact.json (rev 20)

The long one. Three mechanisms tried, three destroyed libraries along the way,
and the answer turned out to be a mechanism that never touches a file.

**What works now.** `interact.json` declares one `persist` variable per setting
at a word of a register file at `0x20000000`. APF reads those words back from
the core every frame, lets the user adjust them in Core Settings, writes them
back, and saves them itself to
`/Settings/HarpMudd.Mp3Player/Interact/_core/interact_persist.json`.

**The core issues no write and no data slot is involved at any point** — which
is exactly why it is safe. Both earlier mechanisms had a path to the user's
`.mp3` files; this one has none.

Because APF reads back *from* the core rather than only writing to it, changes
made with the **buttons** persist too, not just menu ones.

Confirmed on hardware 2026-08-05: settings survived a quit and relaunch, and
**5 of 5 `.mp3` files byte-for-byte unchanged.**

### The three attempts, and what each one taught

**1. `0184` target write — destroyed three libraries.** Its bridge address is
parsed as a struct whose second word APF reads as a SIZE, applied to whatever
occupies the MP3 slot. Compounded by the root cause below.

**2. `nonvolatile` data slot — hung the Pocket.** First at `0xF8002200`, inside
APF's reserved command region; then at `0x20000000`, where it hung on boot with
the flag as the only variable. Isolated by decomposition: the same config
*without* `nonvolatile` booted and loaded fine, so the flag itself is
incompatible with this shell. Never explained further.

**3. `interact.json` — works.**

### The root cause found on the way, worth more than the feature

Reading APF's datatable off hardware before the core touched it showed
`{slot_id, size}` pairs at stride 2 from word 0 — APF's **dataslot ID/size
table**, which Analogue's docs mention but never locate. **Our `0190` response
struct sat at word 0**, and APF writes 64 words there on every `getfile`, which
`pl_open_name()` issues on every track change. So the first skip destroyed
APF's record of every slot's size, and `0184` takes the size it writes from
that table.

Structs relocated to words 64 / 128 / 192. Verified: after a track change every
slot id and fixed size is unchanged and slot 2's size correctly tracks the
playing file. `Select+A` still shows that check.

That fix was necessary but not sufficient — a later write still rounded two
files up to whole clusters — which is why `0184` stays off for good.

### Cleaned up afterwards

Slot 4, the shipped `settings.bin`, `tools/settings_edit.py` and
`tools/settings_probe.md` are all gone — nothing read them once APF took over
the storing. `fw/settings.inc` lost 200 lines of forensics with them; that
history lives in this file and in git rather than at the top of a source file
that no longer does any of it.
