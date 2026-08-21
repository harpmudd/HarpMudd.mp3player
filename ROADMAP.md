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

# Releasing

**Release notes come FROM the changelog, restructured.** `CHANGELOG.md` is the
running list and stays flat, one line an item. A GitHub release body carries
the same content grouped under **New** / **Changed** / **Fixed**, which earns
its place there because a release is read once, on its own, by someone
deciding whether to update.

Same facts either way. If they disagree, the changelog is right.

**Edit release bodies IN PLACE:**

```
PATCH /repos/{owner}/{repo}/releases/{id}   {"body": "..."}
```

Auth is the cached git credential (`git credential fill`); there is no gh CLI
here. **Never delete and recreate a release to change its text** — the Pocket
core updaters poll constantly, so that zeroes the asset's download count and
briefly removes the file they are fetching. Delete only when the attached zip
itself has to change.

**Version lives in TWO files and both must move:**

- `fw/player.c` — `APP_VER`, which is the version on the splash screen
- `dist/Cores/HarpMudd.Mp3Player/core.json` — `version`, which is what the
  Pocket shows in its core list, plus `date_release`

`fw/build.sh` now compares the two and FAILS the build if they disagree, so the
splash and the core list cannot drift again — they did through the whole of
v1.3.0's development, with a card in hand announcing 1.2.0. It cannot check
`date_release`, since only a human knows the release date; it prints it on
every build instead so it cannot be forgotten quietly.

Also at release time: add the date to the changelog entry, which is left off
until a version is actually tagged, and set `date_release` to the same day.

# Defects

Fixed before anything in Enhancements, regardless of how interesting the
enhancement is.

## Resume only works with the DEFAULT playlist — SHIPPED in v1.2.0

Reported against v1.1.0: resume restored track and position with
`playlist.m3u`, but a user running `audiobook.m3u` never got their place back.

### The theory that was wrong, and how it was killed

The firmware never names a playlist -- `pl_load()` reads whatever APF put in
slot 3 -- so the behaviour could only differ because of what APF put there.
`data.json` declared `filename: playlist.m3u` on slot 3 and nothing on slot 2,
and slot 2 is the one proven to retain a pick. The natural conclusion was that
a declared filename resets the slot.

Half right. Removing the declaration was tested on hardware and made it WORSE:
APF then prompted for a playlist at every boot, and a picked list still did not
come back. **Slot 3 is not retained either way** -- reset to the declared file
with one, empty and prompting without. There is no arrangement of `data.json`
that remembers a pick, which is what forced the real fix.

### What actually shipped

Storing the name, because APF cannot enumerate a directory and `0192` can only
open something already held. That needed slots, and all eight were used:

- **RTL:** settings index 3 bits -> 4, register file 8 words -> 16,
  `CORE_VERSION` -> rev 21. Timing closed at 1.029 ns on the 100 MHz domain,
  down from 1.270 but positive; RAM blocks unchanged at 300/308 because
  `ramstyle = "logic"` keeps the file in fabric.
- **Firmware:** three words hold twelve characters of the playlist STEM, four
  7-bit chars per word with bit 31 clear (APF stores these signed). `.m3u` is
  implied. `pl_load()` reopens the remembered stem by name, at boot only.

Two faults found on the way, both worth keeping in mind for anything similar:
restoring on EVERY `pl_load()` overrode the user's own pick, because that
function runs for a menu choice as well as at boot; and storing the stem
uppercased asked APF for `SHENANIGANS.m3u` against a card holding
`Shenanigans.m3u`.

### SCOPE, stated in the README under Playing

**One remembered playlist and one position, not one per playlist.** There is a
single saved stem and a single packed point, so switching lists overwrites what
was held for the previous one.

The consequence a user will hit: partway through `audiobook.m3u`, switch to
`music.m3u`, and the audiobook's place is gone -- returning to it starts at
track 1. "It remembers where you were" reads as a per-book bookmark, and that
is not what this is.

Per-playlist positions would need a stem plus a point each, four words apiece
against five slots free after this release -- a second one needs another
register widen. Not worth it unless someone asks.

Wording for the README, roughly: "The core remembers the last playlist you used
and your place in it. Switching to another playlist replaces what it was
holding, so it is one bookmark rather than one per list."

### Closed out at release

QA passed and the branch merged 2026-08-13. The double-load bullet that used to
sit here is superseded: that investigation ran to ground and has its own entry
below, where the conclusion is recorded properly rather than as a hopeful note
that the gate "has behaved since".

Shipped as **v1.2.0**, not the v1.1.1 first estimated -- the fix needed new
RTL, a wider register file and a CORE_VERSION bump, which is not a patch.

## Light stutter ~2 s into every FLAC — FIXED in v1.4.0, hardware-confirmed

The in-playback size search was corrupting the stream it was measuring.
`size_probe_pump()` binary-searched with reads at far random offsets — 60 MB
out for the clamp reference, then doubling — **on the same slot the decoder was
streaming from**. That drags the APF fragment cache, the refills that follow
return wrong bytes, and the decoder resyncs at the next frame. It fired at
`fl_first_frame + 256 KB`, about two seconds into a ~1000 kbps FLAC, and
reproduced ~99% of the time.

### Why three fixes missed it

A resync leaves the FIFO **full**, so no underrun is ever recorded. An audio
glitch reads as starvation, and three consecutive fixes went there — gate the
probe on ring occupancy, deepen the ring, remove CPU contention. None of them
touched the mechanism. Worse, the first instrument could not see the fault
either: it latched on `under_shadow`, which is cleared only by a flush, so
exactly **one** underrun per track could ever be counted. `N` rising by exactly
1 per track was a ceiling, not a measurement.

What broke it open was asking whether the glitch was an underrun **at all**,
rather than which underrun it was. `V--` at 2–3 s eliminated starvation and
every CPU-stall theory in one round trip — a stall long enough to hear would
necessarily have emptied the FIFO. Two more candidates were eliminated by
reading rather than shipping: `probe_file_size()` runs only on a seek press,
`art_decode()` runs inside `load_track()` before playback starts.

### The rule this establishes

**Never read the streaming slot anywhere but the sequential read head during
playback.** `0190` metadata queries are safe; slot reads are not. The codebase
already knew — the periodic slot-3 identity check is deliberately a `0190`, and
its comment says so: *"does not drag the MP3 slot's fragment cache down with it
the way a periodic poll of slot 3 would."* One caller honoured it, one did not.

### What shipped

The probe now runs only when `track_secs == 0`. Everything else was already
covered: duration from STREAMINFO/Xing via `ui_total_secs()`, bitrate from
bytes-played ÷ seconds-played, the seek bracket from the same steps run
synchronously at the press (a flush follows, so corruption is inaudible), and
the true size for free from `refill_pump` when a read past EOF fails.

Verified on hardware: stutter gone, FLAC seek unchanged, format row still
populates.

### KNOWN, ACCEPTED: headerless MP3 may still tic

Files with no Xing/VBRI header have no other duration source, so they still
probe during playback and may still tic. The trade is a rare tic against no
progress bar and no total time at all. A load-time probe is **not** available
to them — a file opened by name with `0192` has only just been opened and a far
read still fails, which is how a 30 MB track once measured 5 MB. One line
(`if (track_secs) return 0;`) flips the trade if the tic ever matters more.

## Playlist pick occasionally does nothing — ACCEPTED, likely Analogue-side

Intermittent. The user corrected an earlier "rare" framing on 2026-08-13,
having twice needed three attempts -- **but that estimate predates 03adc3d**,
which found the menu-close fallback had never once completed a load (a
self-cancelling dedupe, and a closing edge that could not fire). Expect the
frequency to be lower than anything measured before that commit; nothing yet
measures how much lower.

The README wording was softened to match on the same day, at the user's call.
Put it back if reports say otherwise -- the frequency claim there is the least
evidenced thing in that file. Picking a playlist from the
Core menu sometimes has no effect at all: no LOADING message, no load, and
waiting does not help. Picking again works.

User decision 2026-08-12: **not a deal breaker** -- the natural user response
(pick again) is also the workaround. That still stands; the frequency does not
change the decision, only how it is described to users.

### What the instrumentation actually proved

A diagnostic build recorded every switch (`UI_SHOW_SPEED_DIAG`, plus
`pl_sw_*`/`tk_hist` — all still in the source, one flag away):

- `N` always equalled `L`: every notification that arrived produced a load.
- Every recorded switch read `G1 R0 P1 F0` — gate confirmed the name change,
  no retry needed, playback taken, nothing blocking, right track count.
- `X` stayed 0: `load_track()` never came back empty.

**There was never a failed load.** The failures are picks that never reach the
core at all, so nothing downstream of the notification can see them — which is
why three separate firmware fixes changed nothing.

### Two theories killed on the way

- **"Slow load, no feedback."** Plausible and wrong. UI_BOOT_Y and
  UI_TRANSPORT_Y are both row 262, so the transport repainted over
  LOADING PLAYLIST during the wait and the user re-picked out of impatience.
  That was a REAL bug and is fixed — but the fault survived the fix.
- **"Playback was blocked by an armed reload."** `P1 F0` killed it outright.

### Where it points now

`0190` says the slot still holds the OLD playlist, so APF appears not to have
performed the assignment — not a dropped message, a pick that did not take.
The core-side CDC was read and is correct: `dataslot_update` is a one-cycle
`clk_74a` pulse converted to a toggle with the ID latched in the same cycle,
which is the right idiom (see [[apf-target-commands]]).

### The one test that would settle it

Count EVERY `dataslot_update` pulse in `clk_74a`, ignoring the slot ID, and
expose the counter. Raw count moves on a dead pick -> ours. Does not move ->
Analogue's. Costs an RTL change and a Quartus rebuild, then waiting on a fault
that takes many attempts to hit, which is why it has not been done.

Cheap corroboration if it ever comes up in the wild: whether other cores with
user-reloadable slots show the same thing. No compile needed.

### Mitigations that ARE shipped

Three, and after them the user could no longer reproduce it. None is PROVEN
to be the fix — the fault took many attempts to hit, so absence is weak
evidence — but all three earn their place on other grounds.

1. **Row 262 has one owner.** UI_BOOT_Y and UI_TRANSPORT_Y are the same line,
   so the transport repainted over LOADING PLAYLIST during the wait. A switch
   can legitimately take seconds, and a garbled row during it reads as a dead
   pick — so the user picks again. This alone removes a large part of the
   reported behaviour whatever the root cause is.
2. **`pl_check_req`** — on the Analogue menu CLOSING, ask slot 3 what it holds
   and raise the load ourselves if it is not what is loaded. One 0190 at a
   moment where playback is already interrupted; a metadata query, not a slot
   read, so it does not cost the MP3 slot its fragment cache. Helps if the
   notification was dropped; cannot help if APF never made the assignment.
3. **`PAUSE_LOAD`** (the user's idea) — pause the outgoing track for the
   duration of the switch. Takes the core's 0180 refill traffic off the bus
   while APF is reassigning slot 3, which is the contention the user
   suspected; and silence during the wait reads as WORKING, where music
   continuing reads as nothing happening.

If it ever returns it clears all three at once, leaving "APF never made the
assignment" as the only surviving explanation.

### 2026-08-13: it did return, and the mitigations were not running

Two bugs, either sufficient alone, meant the menu-close fallback had **never
once completed a load** since it was added: it set the dedupe timestamp at the
same moment it raised its own request, so the notification handler discarded
that request one iteration later; and the open/closed memory was paused's menu
bit, which `load_track()` clears wholesale.

The design fault underneath: noticing a pick had exactly ONE route, so every
bug in it was total. There are four now, failing independently -- 008A tagged
for slot 3, 008A tagged for slot 2 (the RTL can mis-attribute), the menu-close
edge, and a 3 s identity poll that depends on neither notification nor edge.

### CONFIRMED WORKING 2026-08-13

The user watched a switch fail to register, **waited instead of re-picking, and
the poll loaded it**. First direct evidence any of these mitigations does
anything -- and it is the one that depends on neither the notification nor the
menu edge, so it covers the case where everything upstream is lost.

The user-facing workaround changes with it: wait a few seconds, rather than
pick again. README and CHANGELOG updated to say so.

Still not a claim the fault is gone. The poll recovers a pick that APF DID
assign but never told us about; it cannot help if APF never made the
assignment, which remains the unexplained case.

**Measured, and it corrects a standing claim:** a 0190 getfile on slot 3 every
3 s while streaming produces NO audible tic. The warning against polling slot 3
applies to slot READS walking the cluster chain, not to a metadata query. The
RTL comment that stated it absolutely has been corrected.

Documented in the README as a known limitation, so a user who does hit it
knows to pick again rather than assuming the core is broken.

## Headerless VBR still estimates the total time — dropped from the README

Removed from Known limitations 2026-08-12 as too narrow to be worth a user's
attention, and recorded here so it is not simply forgotten.

Still true: a VBR file with no Xing/Info/VBRI header has no frame count and no
fixed byte rate, so the total is derived from a drifting estimate and reads
wrong. CBR without a header is exact — that was the 1.2x seek fix, which
computes the rate from the first frame.

Near-unreachable in practice: LAME and every mainstream VBR encoder write a
Xing header, so this needs a file from an unusual encoder or one that has had
its header stripped. `tools/xing_check.py` reports which files in a folder
carry one, if evidence is ever wanted.

Restore the README line if anyone reports a wrong duration — the symptom is
otherwise baffling, and the cause is a property of their file rather than
anything they can see.

## ID3v1-only tags never display from a playlist — BACKLOGGED, currently unreachable

A file carrying **only** an ID3v1 tag, loaded from a playlist, shows no title,
artist or album. Found 2026-08-21 while tagging the Aesop's Fables audiobook,
23 of whose 26 chapters were in exactly that state.

### Cause

`id3v1_read()` is called from `read_track_head()`, which runs BEFORE
`load_track()` establishes `slot_size`. Its first line is
`if (slot_size < 128u) return;`, so on a playlist load — where `pl_arm_load()`
zeroes `slot_size` and sets `force_size_probe`, because a file opened by name
with 0192 raises no reload edge — it gives up immediately. On a menu load
`R_SLOT_SZ` is valid and it works, which is why this looks intermittent.

### The trap, and the reason a naive fix fails

The obvious repair is to call it later, once a size exists. That does not work,
and the reason is worth writing down because it costs an hour to rediscover.

The only cheap size available on a playlist load is
`slot_size = audio_start + track_bytes`, derived from the Xing/Info header. That
is the end of the **audio**, not the end of the **file** — and the ID3v1 block
sits after it. So `slot_size - 128` lands inside the last audio frame, the
`TAG` check fails, and the result is identical to the bug by a longer route.

### The fix, when it is worth doing

Two parts, and the second is what makes the first useful:

1. Retry the v1 read from `load_track()`, after a size exists, rather than from
   `read_track_head()`, which runs before one does.
2. Scan a ~512-byte window near the estimated end for the `TAG` magic instead of
   demanding an exact offset. The Xing-derived size underestimates by precisely
   the tag length, so a window absorbs the error.

~40 lines and one helper. Touches only the load path, so it does not reopen the
fragment-cache hazard — far reads at load are already routine there (album art
does them). Estimate ~1 hour plus one hardware test.

Coverage: Xing/Info + v1-only is fixed by the window; menu-loaded files already
work; headerless + v1-only falls out for free, since those still probe during
playback and a retry when `slot_size` lands would catch them.

### Why it is parked

**Measured, not assumed: 0 of the 14 MP3s on the card are affected.** Thirteen
carry both a v2 title and a v1 block, so the v2 path resolves first and
`id3v1_read()` is never reached; the fourteenth is the deliberate
`_no_tag` test file, which has neither and would show nothing regardless.

The audiobook that surfaced this was fixed at the source instead, with real
ID3v2.3 tags written from the LibriVox metadata. Nothing on the card can now
trigger the defect, and changing the load path for a reason that is not present
is the exact mistake that cost three fixes in the stutter hunt above.

# Enhancements

## Progressive JPEG covers are silently skipped — user report 2026-08-13

picojpeg is baseline only: `case M_SOF2: return PJPG_UNSUPPORTED_MODE`. A
progressive JPEG is rejected whatever its size, which is exactly how it was
reported -- "even a 100x100 3 KB jpeg does not work", after the user had
correctly ruled size out. Desktop taggers show progressive files fine, so
mp3tag displaying the cover is not evidence against this.

**The README now says baseline**, which is the fix for the next person. It said
"JPEG album art only", and the user did everything right against that.

What the core accepts, for reference:

- baseline JPEG, 8-bit, greyscale or YCbCr; no progressive, CMYK or arithmetic
- ID3v2.3 / v2.4 `APIC`. **ID3v2.2 is not read at all** -- see its own entry
  below; it is NOT the one-line change this line first claimed
- MIME containing `jp[eg]`, case-insensitive; picture TYPE is not checked
- up to `ART_MAX_BYTES` (2 MB)
- under 736 px on either axis -> full decode, both must be <= 1024;
  736+ on both -> reduce path, up to 2560

**That size rule is a trap worth removing.** A 900x900 cover works and a
1200x600 one does not, because the reduce path is only chosen when BOTH axes
are >= 736 and full decode is capped at 1024. Nothing tells the user which
branch they are in. Allowing reduce on a per-axis basis, or raising
ART_FULL_MAX, would make the rule "up to 2560" flat.

## ID3v2.2 tags are not read at all — NOT planned, scoped 2026-08-13

Raised while helping a user whose cover would not show. Recording the real
shape of it, because this entry first described it as "a second frame-id
comparison", which is wrong and would have made it look free.

**v2.2 is a different container, not v2.3 with shorter names:**

| | v2.2 | v2.3 / v2.4 |
|---|---|---|
| frame id | 3 chars (`PIC`, `TT2`, `TP1`, `TAL`) | 4 chars (`APIC`, `TIT2`...) |
| frame header | 6 bytes | 10 bytes |
| size field | 3 bytes, plain | 4 bytes, plain (2.3) or syncsafe (2.4) |
| picture frame | 3-char format field, `JPG` / `PNG` | NUL-terminated MIME string |

So it needs a parallel walk in BOTH readers -- `art_find_apic()` and the text
frame reader, which currently share `p += 10u + fsize` and a table of
4-character names. Call it 60-100 lines, plus a v2.2 file to test against,
which we do not have.

**Who has these:** iTunes wrote v2.2 by default for years, so old ripped
libraries carry it -- and old material is exactly what gets converted into
audiobooks, which is how this came up.

**Not planned, and the trigger is evidence.** One user with a cover problem is
not yet a v2.2 report: their tag version has not been checked, and a
progressive JPEG explains the same symptom. If v2.2 files actually turn up,
this moves up; until then the failure is at least not silent -- an untagged or
unreadable file now shows its filename rather than nothing.

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

**Full analysis: [docs/FLAC.md](docs/FLAC.md)** (written 2026-08-13). The
benchmark that answers the gating question is built and waiting on the v1.3.0
branch behind `IO_BENCH`; it reports on the diag row as `IO<n>KB`. Read it
before building anything else, and read the heap warning in that document --
the FLAC decoder must share Helix's heap, not add to it, and getting that wrong
produces a core that cannot decode at all while the build still passes.

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

## Raise the 128-track playlist cap — requested 2026-08-13

### The budget, measured

`_heap_start` (end of BSS) is 0x2ACC0 and `_tag_start` — the first reserved DMA
buffer, which the linker ASSERTs the image must stay below — is 0x33000. So
**33,600 bytes are free**, and the link fails rather than silently overlapping
if that is exceeded.

**Do not use build.sh's "% of usable RAM" to judge this.** It compares the ROM
(text + data) against a flat 176 KB and ignores BSS entirely, so it currently
reports 74.6% while the real headroom is 33.6 KB of 256 KB. Playlist storage is
almost all BSS, which is exactly what that figure cannot see.

### What a track costs

4 bytes of index (`pl_off` + `pl_order`, `uint16` each), an eighth of a byte in
the `pl_dead` bitmap — and the `.m3u` TEXT, which dominates everything else.

| cap | text | total | vs today | fits in 33,600? |
|---|---|---|---|---|
| 128 | 16 KB | 16,912 B | — | current |
| 256 | 24 KB | 25,632 B | +8,720 B | yes, comfortably |
| 256 | 32 KB | 33,824 B | +16,912 B | yes, ~16 KB left |
| 512 | 64 KB | 67,648 B | +50,736 B | **no** |

So **256 is affordable and 512 is not**, without freeing something first.

**Raise PL_MAX and PL_TEXT_MAX TOGETHER.** Already learned once and written on
PL_TEXT_MAX: at 8 KB the buffer ran out around 74 tracks while the docs
promised 128, so the cap was a lie in the other direction. A real playlist here
averages ~110 bytes a line.

### The structural answer, if 512+ is ever wanted

Stop holding the text at all. `pl_text` exists so a name can be resolved
without touching the card, but names are only needed when a track is opened —
not to count the list or to shuffle it. Keeping a byte offset into the FILE
instead of into a RAM buffer makes the per-track cost 4-6 bytes flat and
decouples the cap from RAM almost entirely.

The cost is one extra slot read per track change, on a path that already does
several and is already the slow part — see the load-speed entry. Worth pricing
against that work rather than doing both separately.

## Audiobooks: `.m4b` — requested 2026-08-13

Wanted because this core already suits long-form listening: it has resume, and
the README points audiobook users at playlists. `.m4b` is the format that
material actually ships in.

**It is [[AAC]] plus chapters, not a separate job.** `.m4b` is an MP4 container
with the audiobook extension by convention — same atoms as `.m4a`, almost
always AAC-LC inside. So it inherits the whole AAC entry above: the Helix
fixed-point AAC decoder is the easy part, CPU headroom is a real question at
60 MHz, and the MP4 container is the actual cost, because atom parsing and
sample tables mean random access and random access is the expensive operation
here.

Do not treat this as a smaller ask than AAC. It is AAC with more on top.

### What `.m4b` adds beyond AAC

- **Chapters**, which are the point of the format. Two encodings in the wild
  and both appear: a Nero-style `chpl` atom in `moov/udta`, and QuickTime text
  tracks referenced by `chap`. Supporting one and not the other means half of
  real files show nothing.
- **A UI that does not exist.** Chapters need listing and seeking-to; the
  transport is built around tracks, and a playlist position is not a chapter
  position.
- **Bookmarks per book.** The saved point is currently ONE, and audiobook
  listeners keep several going. See the one-bookmark scope note under the
  playlist work — per-book positions need a stem plus a point EACH, which is
  another settings-register widen.
- **Durations of hours**, against songs of minutes. Worth checking the elapsed
  and total fields, the progress bar's arithmetic and the seek step scaling
  hold up at 10+ hours before assuming they do.

### Cheaper thing that helps the same user today

Nothing in the ask needs a new format: a long MP3 in a one-line `.m3u` already
resumes, which is what the README recommends. The gap `.m4b` closes is chapters
and having several books on the go — so if this is ever picked up and AAC is
too expensive, **per-book bookmarks alone would deliver most of the value** for
a fraction of the work, and against MP3 files that already play.

## Track changes take too long — user report 2026-08-12

"Skipping titles needs a bit long time." Real, and the load path is already
instrumented: `ld_head`, `ld_size`, `ld_art`, `ld_pre`, `ld_total`, all live,
all one flag away (`UI_SHOW_DIAG 1`).

### MEASURE FIRST — and there is a live disagreement to settle

This entry used to assert "artwork decode dominates". The comment on the size
probe in `load_track()` asserts the opposite: *"~20 blocking reads -- measured
at 480 ms ... which is most of the hiccup."* **Both are guesses in the same
codebase pointing at different culprits**, and the timers settle it in one
hardware session. Do not optimise before reading them.

The diag-row workflow proved itself on the playlist investigation: flip the
flag, do the thing, photograph the row. Cheap.

What each answer would mean:

- **`ld_size` dominates** — the size probe. Already incremental in the main
  loop, but a 0192-opened track raises no 008A, so `R_SLOT_SZ` is stale,
  `force_size_probe` is set, and the BLOCKING fallback runs on every playlist
  track change. That is the case to attack, and it is not the artwork.
- **`ld_art` dominates** — the levers below.
- **`ld_head` or `ld_pre` dominate** — neither, and this entry needs rewriting.

### Levers, if the artwork really is the cost

1. **`_tag_size` is 4 KB**, so an 847 KB cover costs ~207 target reads. Raising
   it is a one-line change, but a read larger than 4 KB is UNPROVEN on this
   hardware -- prove the read first, in isolation. Cheapest thing to try.
2. **A scaled IDCT inside picojpeg.** Half scale is roughly quarter cost. The
   reduce path already skips this for covers over 736 px, so the win is on
   SMALL covers, which are the ones taking the full decode.
3. **Decode the art AFTER playback starts.** This entry previously called it
   "the biggest perceived win for the least work". **The second half is wrong**
   -- corrected 2026-08-13 after actually reading the path.

   `art_decode()` is one blocking pull-parser and the core is single-threaded,
   so nothing decodes MP3 while it runs; the PCM FIFO holds ~46 ms against an
   artwork decode of hundreds. Calling it later just moves the underrun.

   Doing it properly means making the decode RESUMABLE: N MCUs per main-loop
   pass, yielding to refill between slices. In its favour, the MCU loop is ours
   (`art.inc`) and `pjpeg_decode_mcu()` is already one MCU at a time, so no
   fork of picojpeg. Against it: ~12 locals plus `pjpeg_image_info_t` become
   persistent state, the two-pass reduce/full init rewinds the stream, a track
   change mid-decode has to abort cleanly, and `art_need_bytes` can still block
   for a 4 KB read INSIDE a slice -- which has to be measured against the 46 ms
   FIFO before any of this is worth starting.

   Still the biggest perceived win. Not the least work — it lands in the audio
   continuity path that took longest to stabilise.

## Meters sit pegged on loud music — PEAK drives the bars (v1.4.0)

User report 2026-08-15: "for many songs many of the meters are peaked out, very
evident on the bar meters. Is the baseline set too high?" The instinct is right
and the cause is one level down: it is not the baseline, it is what DRIVES the
bar.

`peak_amp` is max|sample| over the block, and `amp = peak_amp * UI_WAVE_H /
32768` maps it linearly. On a modern master the peak of any 26 ms block is near
full scale almost continuously, so the bar lives in its top few pixels.

Measured on three real tracks, per 1152-sample block:

| track | bar from PEAK | bar from AVERAGE |
|---|---|---|
| Pink Floyd | max 37, p90 17 | max 12, p90 5 |
| Jerry Garcia | max 46, p90 41 | max 16, p90 12 |
| Circles Around the Sun | max 8, p90 6 | max 3, p90 1 |

Peak runs ~3x average, consistently. Those samples are quiet INTROS so nothing
pegs in them; the loud body of the same masters puts per-block peak at 0.9-1.0
FS, which is bar 65-72 of 72.

### The fix

Real meters split the two: VU averages, PPM peaks. Ours uses peak for the bar
AND the hold marker.

Drive the bar from the mean of |sample| -- one add per sample in a loop that
already runs, one divide per block -- and leave `wave_pk` on peak so the hold
marker still means what it says. Then apply a gain so a loud passage lands
around 75-85% of height with room left to move: average sits near 0.2-0.35 FS
on loud material, so roughly 2.5x.

Cheap in code, maybe 100 bytes. Not cheap in TUNING: this changes the look of
all ten meters at once, and the magic eye took about eight rounds to get right.
Preview it as ASCII against real audio before any firmware build -- that is how
the eye was tuned without burning hardware cycles.

### Also record

Peaks became an accumulated MAXIMUM across the display frame in v1.3.0
(082babf), where they were previously a plain assignment that dropped half the
chunks. That is correct -- no transient is lost -- but it does read very
slightly higher, so a small part of the pegging is new in v1.3.0 rather than
inherited.

## Absolute paths in a playlist — supported in code, NEVER tested

`pl_open_into()` treats an entry beginning with `/` as absolute and replaces
the template path outright, and the README documented that with an example
pointing outside the core's own folder (`/Music/Albums/...`). Removed at the
v1.3.0 release audit: the code path has never once been run, and it compounds a
second unknown -- whether APF will open anything outside
`/Assets/mp3player/common/` at all.

Both questions settle in one hardware session: put one absolute entry in a test
playlist, once inside the core's folder and once outside it. If the inside case
works the claim can come back for relative-to-root paths; if the outside case
works too, the "music can live anywhere on the card" line can come back with
it. Until then the README claims only what has been played: names relative to
the playlist's own folder, including subfolders under it.

## Memory and speed — measured 2026-08-16, for v1.4.0

Slack at the v1.3.0 merge was **1216 bytes**, the floor `link.ld`'s heap assert
allows. Anything added failed the link. This is where the room came from and
where the rest is.

### Where the RAM actually goes

`nm --size-sort -S` on the v1.4.0 build:

| | bytes | kind | |
|---|---|---|---|
| `arena` | 24576 | BSS | sized by Helix's MEASURED 23824 peak; 752 spare |
| `ui_draw_dynamic` | 19212 | text | the ten meters |
| `pl_text` | 16384 | BSS | the .m3u text, sized for 256 tracks |
| `main` | 13724 | text | |
| `load_track` | 11892 | text | |
| `art_acc` | 11040 | BSS | artwork scaling accumulator |
| `xmp3_huffTable` | 8484 | rodata | Helix |
| `pcm` | 4608 | BSS | Helix output, reused as the FLAC meter window |

### Done

**picojpeg at -Os: 3208 bytes.** It decodes art once per load, inside the
silent gap where the FIFO is already flushed, so nothing it does is on the
audio path. Slack 1216 -> 4424, which is what made the overlay possible.

The ceiling for this approach is known: **the whole build at -Os saves 25744
bytes.** Most of that is Helix and the meter drawing, both hot, so it is not
available -- but it bounds the argument.

### Ruled out, with the reason

**`art_acc` overlapping the `arena` -- 11 KB, and it does NOT work.** The
earlier entry proposed it on the assumption their lifetimes are disjoint. They
are not: `load_track` allocates the decoder at the top (line ~6085) and decodes
artwork at ~6299, so both are live together. It would need art decoded BEFORE
the decoder is allocated, which means reordering the function that carries the
"one FLAC attempt broke every load after it" history. Possible, not cheap, and
not to be attempted on a hunch.

### Still available, ranked

1. **Split the cold half of player.c into its own -Os translation unit.**
   `load_track` + `main` are 25.6 KB of text and neither is hot -- one runs per
   track change, the other once. At the -Os ratio measured elsewhere that is
   roughly 6-8 KB. The obstacle is mechanical, not conceptual: both reach dozens
   of file-scope statics, so splitting means exporting them.
2. **Drop a meter.** `ui_draw_dynamic` is the single largest text symbol at 19
   KB for ten meters. Cheap in effort, unpopular, last resort.
3. **`pl_text` 16 KB -> SDRAM.** Only if the CPU can address SDRAM directly,
   which is UNVERIFIED -- the framebuffer and art stash are reached through the
   drawing engine, not by load/store. Settle that question before planning
   around it.

### Speed: MEASURED 2026-08-19, and it was not what anyone guessed

`UI_SHOW_LOAD_TIMES=1` was finally run. Three loads, milliseconds:

| | H head | S size | A **art** | P prefill | T total |
|---|---|---|---|---|---|
| MP3 320 kbps | 526 | 0 | **1655** | 26 | 2209 |
| FLAC, album track 1 | 299 | 625 | **2801** | 6 | 3731 |
| FLAC, album track 2 | 296 | 628 | **2796** | 6 | 3726 |

**Album art is 75% of a load.** Everything else is noise beside it.

Two guesses in this codebase died here. The size probe -- "the single largest
load-time win available" -- is the S column, and it reads **0 ms on MP3**. It
had already been moved off the load path by then, and the user could not tell
any difference, correctly. It was never what made a load slow.

And the cost is not scaling. The test cover is 1200x1200, which already takes
picojpeg's 1/8 reduce path, so the 2801 ms is Huffman decoding 259 KB of
entropy data. A smaller panel would save nothing. Only NOT decoding helps.

DONE, from that: the art stash is now keyed on a fingerprint of the IMAGE
(length plus a hash of its first and last 512 bytes) rather than on the 0190
file identity. All twelve tracks of the test album embed a byte-identical
259276-byte JPEG, so eleven of those decodes were reproducing a picture the
stash already held. Costs two small reads against 2801 ms.

P is gone from the diagnostic row: 6..26 ms earned nothing, and the width it
took clipped T -- "T3731" printed as "T373", a total smaller than one of its
own parts.

### Still available

1. **Skip the PICTURE block in `flac_open`** -- 628 ms off every FLAC load, and
   the only item here with no UX consequence at all. `flac_open` walks the
   metadata through the ring to reach STREAMINFO and the comments, which means
   streaming past 259 KB of artwork it does not want. It needs a small addition
   to `fw/flac.h`: an optional skip callback the caller backs with a
   reposition, since the decoder has no concept of seeking. Cheapest remaining
   win.
2. **Incremental art decode** -- see below. Backlogged, not abandoned.
3. `H` at 299..526 ms is unexamined. Nobody has looked at what the head read
   actually does.

## Incremental album-art decode (BACKLOGGED 2026-08-19)

Decode the cover in slices from the idle path instead of blocking the load,
the way the file-size probe already does. Would remove 2801 ms from the load,
let art be skipped entirely while hidden, and fix the one case the image cache
cannot: the FIRST track of every album, and mixed playlists where every cover
differs.

**Backlogged after the trade was quantified, not for lack of appetite.**

The 2801 ms is CPU-bound. Spread across idle time it stretches by however much
idle exists, and this build has measured that elsewhere: **MP3 runs ~27% idle,
24-bit FLAC ~0%**. So the honest description is not "art appears a beat later"
-- it is:

- music starts ~2.8 s sooner, on every track
- art fills in **roughly 10 seconds** into an MP3
- on a dense FLAC it may not progress at all while playing

That is a genuine trade rather than a free win, and it is the user's call which
half they want. The image cache already took the common case (playing an album
straight through), which narrows what is left to justify the work.

### If it is picked up

`art_decode()` in `fw/art.inc` is already MCU-at-a-time -- `pjpeg_decode_mcu()`
inside a `my`/`mx` loop -- so the shape is a state machine over that loop with
the locals hoisted to statics. Drive it from `refill_pump()`'s "ring is at
least half ahead" early return, which is exactly where `size_probe_step()` now
runs and is proven not to starve audio.

Three things to get right, each already load-bearing in that function:

- `has_art` decides the LAYOUT (panel versus full-width waveform) and must be
  known at load time. The FINDER establishes it without decoding, so this is
  available -- do not infer it from decode completion.
- `ui_art_mount()` fills the stash with the panel colour, so it must run before
  the first slice, and `ui_art_round()` only after the last.
- The accumulator work between MCU rows (`art_flush_row`, `next_ay`) carries
  the H1V2 block-offset history and the row-overrun bound described in its own
  comments. Hoisting it is where a subtle corruption would come from; it is
  caught by `tools/art_scale_model.py`, which should be re-run against any
  restructuring.

Un-hiding mid-track needs a decision either way: today art is always decoded at
load, so toggling it on is instant. Any scheme that skips the decode has to
answer what appears at that moment.

## Playlist overlay — hold a button, browse the list, pick a track (v1.4.0)

Requested 2026-08-15. Hold a button, see the playlist, scroll it, choose a
track. Agreed as the headline enhancement for 1.4.0.

### Most of it already exists

This is the unusual case where the hard parts are done and the missing piece is
presentation:

| need | already there |
|---|---|
| the track NAMES, in RAM | `pl_text[16384]`, the .m3u text itself |
| where each name starts | `pl_off[PL_MAX]`, byte offset per track |
| shuffle-aware ordering | `pl_order[]` |
| play an arbitrary index | `pl_play_at(i)` — the skip path already calls it |
| hold-to-act input | `PL_HOLD_MS`, used by Left/Right scrub and hold-A |
| text and rect drawing | the engine, with `fb_text_boxed` clipping |

So a name is `&pl_text[pl_off[i]]` and playing it is `pl_play_at(i)`. What has
to be written is the overlay itself: a draw routine for N rows with a
highlight, a selection index with Up/Down, and a modal input state that
suspends the normal button meanings while it is open. Plus a full repaint on
dismiss, which `ui_gradient()` + `ui_draw_chrome()` already do.

### The binding constraint is SPACE, and it is binding NOW

Measured at v1.3.0: **link slack is 1024 bytes, exactly the floor the
`link.ld` heap assert allows.** The next addition of any size fails the link.
That is not a warning about the future; it is the state today.

Largest consumers, from `nm --size-sort`:

| | | |
|---|---|---|
| `arena` | 24576 | BSS, sized by Helix's measured 23824 peak — 752 spare |
| `ui_draw_dynamic.part.0` | 19212 | TEXT, the ten meters |
| `pl_text` | 16384 | BSS, the .m3u text |
| `main` | 12148 | TEXT |
| `load_track` | 11908 | TEXT |
| `art_acc` | 11040 | BSS, the art scaling accumulator |
| `xmp3_huffTable` | 8484 | RODATA |

**Do not assume SDRAM is the escape.** It holds the framebuffer and the art
stash, but those are reached through the DRAWING ENGINE — see the SDRAM->SDRAM
block move, whose source rides in the colour registers. Whether the CPU can
load and store SDRAM directly is UNVERIFIED, and `pl_text` has to be parsed by
the CPU. Prove that before planning around it.

Candidates that do not depend on that question, cheapest first:

1. **`art_acc` overlapping the arena.** 11 KB used only while artwork decodes,
   and artwork decodes at load. Whether it is disjoint from the decoder's
   lifetime needs checking against `load_track`'s ordering — if it is, this is
   the single biggest win available.
2. **Cold code at `-Os`.** `load_track` and the boot paths are not hot;
   `ui_draw_dynamic` and the decoders must stay `-O2`. Per-file flags, the way
   `flac.c` is already handled — NOT `__attribute__((optimize))`, which was
   tried on the FLAC decoder and nearly doubled the object.
3. **Trim a meter.** Ten is generous, and they are the largest single text
   item. Unpopular, and a last resort.

### Order of work

Reclaim space FIRST, and land it as its own build with no behaviour change, so
a regression there is unambiguous. Then build the overlay against a known
budget. Attempting them together means a link failure mid-feature with two
suspects, which is how this branch lost most of a day already.

## Hold-A for 1.2x is too easy to trigger by accident

A user reported a track playing at "double speed". The likely cause is not a
decode fault at all -- it is hold-A engaging 1.2x when they meant to pause.

**PL_HOLD_MS is 400 ms**, shared with the Left/Right scrub. That is a sensible
threshold for a deliberate scrub and a short one for a button whose tap action
is PAUSE: pressing pause with any deliberation crosses it. The toast says
SPEED 1.2x, but a user who was not looking at the screen just hears the music
speed up for no reason.

Options, cheapest first: a longer threshold for A alone (say 800 ms, since
nothing about speed needs to be quick); require Select+A; or drop the gesture
and put speed in Core Settings, which costs a slot but cannot be hit by
accident. Confirm the cause before changing anything -- the user was asked and
has not answered yet.

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

## More meters — idea bank (user, 2026-08-12)

"I'd occasionally like to slip a new one in every once in a while." So this is
a standing list, not a task. Nine exist: bars, waterfall, L/R levels, phase
scope, oscilloscope, VU needles, scrolling waveform, mirrored bars, peak dots.

**APPEND to the `VIZ_*` enum, never reorder it.** `viz_mode` persists as an
INDEX, so inserting a meter in the middle silently repoints every user's saved
preference at a different one. Same rule as the interact.json ids. Adding one
is otherwise self-contained: an enum entry, a draw block in `ui_draw_dynamic`,
and a name in the toast chain.

**What a new meter can read for free.** The per-frame loop at the `peak_l`/
`peak_r` computation already visits EVERY sample of every decoded frame, so
anything derived per-sample costs only the arithmetic, not the traversal.
Available today: peak L, peak R, the `wave[]` history, the scope sample spread,
and elapsed/total.

### Before building any meter with EMPTY areas — learned the hard way

Every meter fills the box with `bed`, the gradient sampled ONCE at the box's
top row. The screen behind it is a per-row gradient that falls from 16.1/31 at
the top of the box to 9.9/31 at the bottom. So the box is a flat slab sitting
on a ramp, and every meter built so far got away with it because its content
covers the box.

The magic eye was the first with large empty areas and it showed immediately:
a visible rectangle, stopping at the box edge, obvious on a dock and worst on
the accent colours with the steepest ramp. **Any meter that leaves background
showing must paint the real ramp itself** — `ui_grad_at(y)` per row, 72 rects,
cached with the rest of its face.

Two follow-on traps from the same episode:

- **Background must be painted PER ROW, not in strips.** The eye paints its
  glow in 4-row strips to keep the rect count down; doing the same to the
  UNLIT remainder gave each strip its top row's colour, which reads as
  horizontal stepping against the dithered surround. Lit pixels can be
  quantised — the colour mixed in dominates. Background cannot.
- **When a change regresses something already fixed, diff against the
  known-good commit.** Two rounds of re-reasoning here both missed a clamp
  that had been deleted during an experiment and never restored; the diff
  found it in one look.

### Buildable from what exists

- **L/R isometric cube stacks** -- user's idea. Discrete blocks in fake-3D with
  peak-hold caps; the isometric projection is what keeps it distinct from the
  mirrored bars.
- **Vinyl platter and tonearm** -- platter spins, arm creeps inward with track
  progress, highlight pulses with level. Uses elapsed/total, which no current
  meter visualises.
- **Tape reels** -- same data. The supply reel speeding up as it empties is the
  detail that sells it.
- **Radar sweep** -- a line sweeps a circle painting level as radius, with a
  decaying trail. Polar version of the scrolling waveform.

### Needs one enabler: a bass proxy

A **one-pole lowpass in the existing per-sample loop** -- a shift and an add.
Unlocks:

- **Beating speaker cone** -- user's idea, and it NEEDS this. Driven by overall
  level it wobbles on everything and reads wrong; driven by bass energy it
  thumps on kicks.
- **Ferrofluid spikes**, and **pressure rings** emitted on each kick.

### The one that unlocks the most: an RTL filter bank

[[Spectrum display]] below says the decoder exposes no frequency bins. True,
and beside the point -- what is needed is a filter bank, and **the EQ is
already biquads in hardware**. An analysis bank in RTL costs LOGIC, not BRAM
(the resource at 97%), and zero CPU. Retires the known limitation and unlocks
true spectrum bars, a real spectrogram waterfall, and a graphic-EQ display.

In firmware instead: possible on a decimated signal, but decode leaves roughly
14 MHz of the 60 at 1x (derived from the measured 54.8 MHz at 1.2x, not
measured directly) and nothing at 1.2x.

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

## The persisted-settings register file — 5 of 16 slots free

**Was "FULL", and is no longer.** rev 21 widened `set_idx` from 3 bits to 4 in
`mp3_soc.v`, taking the file from 8 slots to 16. Eleven are spoken for: volume,
accent, repeat, shuffle, resume point, meter, EQ, resume on/off, and three
words carrying twelve characters of the playlist stem.

**Five are free**, so the next remembered setting costs nothing but an
`interact.json` entry. The one after five more needs another widen — and the
register file is `ramstyle = "logic"`, kept in fabric because BRAM is at 97%,
so each slot is real LUTs rather than free memory.

Anything stored here is read back through `interact.json`, so see the id rules
at the top of `fw/settings.inc` before adding one: ids are never renumbered, a
slider's `max` has to cover the new range, and array order sets the menu order.

# Fixed

Kept for the reasoning, not the status. Nothing here is outstanding.

## Resume where you left off — SHIPPED v1.1.0, extended in v1.2.0

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

**Shipped.** v1.1.0 restored the track and position; v1.2.0 added the
playlist NAME, so it survives using a list other than playlist.m3u. Scope is
one bookmark — see the note under the v1.2.0 defect.

## Variable playback speed — SHIPPED, hold A for 1.2x

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

**Shipped.** Hold A toggles 1.2x, off every launch, documented in the README
along with the distortion it can cause. Still open, separately: the 400 ms
hold threshold is shared with the seek and may be too easy to hit by
accident — see its own entry.


## No internals on screen, and the filename when a file has no tag — 2026-08-12

Two places put debugging output in front of users, and both shipped.

**The title row.** A file with no readable ID3 tag displayed
`NOTAG FFFB9064 R04` -- a status word, the file's first four bytes, and the
reload status. That string was built to tell three failure modes apart during
the reload hunt and it earned its place then. On a shipped player it captioned
a file that was playing perfectly well with a hex dump.

Now shows the FILENAME, which is almost always the song name: last path
component, extension trimmed only when the dot looks like one (`Blur - 13.mp3`
must keep its number). Falls back to `UNKNOWN TRACK` when APF reports no name
either. This also retires the `UNICODE TAG` caption -- the same mistake in
words. A tag encoding the parser declines is our limitation to state in the
README, not a label for someone's music, and those files have filenames too.

**The LOAD FAILED screen.** Printed `HEAD FFFB9064` under the heading: a hex
dump on the one screen a user sees when something has already gone wrong. Now
says `THE FILE COULD NOT BE READ`.

Left alone deliberately: `! FILE SIZE WRONG - CHECK SD CARD` is plain and
tells the user what to do about it. The Select+A / Select+B struct dumps are
behind `#if DEBUG_DIAG`, which is 0, so a release build cannot reach them.

**Standing rule from the user: no diagnostic is ever shown to users.** Debug
output goes behind a compile-time flag from the start, not "temporarily" into
a live screen.

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
