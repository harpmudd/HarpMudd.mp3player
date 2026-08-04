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

## Settings write damaged the user's files — RELEASE BLOCKER, under observation

**`SETTINGS_WRITE` is 1 again** as of 2026-08-04, on the user's instruction and
before the blocker is understood. Saving works: settings survive a relaunch, and
one full session of writes left every `.mp3` on the card byte-for-byte intact.

**This is not resolved.** Three times previously, every `.mp3` sharing the card
came back extended and one file's audio was destroyed. Nothing found since
explains why, the mechanism that looked certain was tested and missed (below),
and all three events followed *extended* use rather than a single session. So
the current state is "writing is on and being watched", not "fixed".

**Until it has survived a run of real sessions, take a snapshot around each
one** — `python tools/card_snapshot.py before | after | diff`. Roughly a minute,
and it is the only thing that turns a recurrence into evidence instead of lost
music. Keep the originals backed up. `tools/settings_edit.py` remains the way to
set values without trusting the write path at all.

To turn it back off: set `SETTINGS_WRITE` to `0` in
[fw/settings.inc](fw/settings.inc) and rebuild — the write body compiles out and
the core then contains no `0184` at all.

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

### Mechanism found: the damaged size IS word 1 of the settings record

Read as decimal for three events, and that is what hid it:

```text
21,037,825 = 0x01410301
21,365,505 = 0x01460301
20,382,465 = 0x01370301
```

The low 16 bits are identical all three times. Now decode them as the record's
word 1, which `settings_write_now()` packs big-endian as
`{SET_VERSION, volume, palette, repeat}`:

| size | version | volume | palette | repeat |
| --- | --- | --- | --- | --- |
| `0x01410301` | 1 | 65 | 3 | 1 (ALL) |
| `0x01460301` | 1 | 70 | 3 | 1 (ALL) |
| `0x01370301` | 1 | 55 | 3 | 1 (ALL) |

Every field is in range and every one is right. Version is constant because it
is a constant; palette and repeat are constant because they had not been
changed; **volume** is the one that moves — and 65 is the firmware default, and
volume is both the most-changed setting and the thing that triggers a write.

Confirmed independently against the card: the `settings.bin` on it right now
holds volume 55, palette 3, repeat ALL — bytes 04..07 are `01 37 03 01` =
`0x01370301`, byte-for-byte the third damaged size.

So APF is not writing 32 bytes of payload at offset 0. Something in the write
path reads the word at *bridge address + 4* and uses it as a **size**. Compare
`0192`, where the bridge address points at a parameter *struct* APF parses
("filename and flag/size") rather than at raw bytes; `0184`'s source pointer
appears to be treated the same way, so our word 1 lands in a size field.

This accounts for everything the earlier theories could not: why all four `.mp3`
files share one size (each is stamped in turn while it occupies the MP3 slot),
why `settings.bin` stays exactly 32 bytes (it is not the file being sized), why
the low half never moved, and why a mitigation about *where* `settings.bin` lives
changed nothing.

### The prediction was made and it FAILED — 2026-08-04

Writing was re-enabled and a real session run with a before/after card snapshot.
Settings ended at volume 55, palette 9, repeat ALL, so word 1 was `0x01370901`
and the mechanism predicted an `.mp3` extended to **20,384,001** bytes.

- `settings.bin` was rewritten correctly, in place, at exactly 32 bytes.
- **All five `.mp3` files came back byte-for-byte identical.**
- Nothing grew, nothing was rewritten. The prediction missed completely.

So the word-1 theory is **unconfirmed**, and the striking part — three damaged
sizes that each decode to a valid, plausible settings record, plus the card's
own `settings.bin` matching the third exactly — remains unexplained. Either it
is a much larger coincidence than it looks, or the fault needs a condition this
session did not reproduce. **Do not build a fix on it.**

It is still worth keeping as a **detector**: if damage recurs, decode the new
size as `{version, volume, palette, repeat}` at that moment. A second hit
confirms it outright; a miss kills it. Falsifiable on the next event rather than
on speculation.

**The clean run does not clear `0184` either.** One session of writes leaving
the card intact is necessary, not sufficient — all three damage events followed
extended use, and the deferred-write and subdirectory mitigations were already
in place for the third.

### Ruled out by inspection, so the measurement need not re-check it

- **Slot ID.** `data.json` declares Settings as id 4; `SET_SLOT_ID` is 4.
- **Slot writeability.** Analogue's `parameters` bitmap is bit 0 user-reloadable,
  1 core-specific, 2 nonvolatile filename, **3 read-only**, 4 instance json,
  5 init nonvolatile, 6 reset-on-load, 7 restart, 8 full reload, 9 persist
  filename. Ours is `0x1` — bit 3 clear, and "must not be read-only" is the only
  slot requirement 0184 documents.
- **The parameters.** 0184 takes slot id `[15:0]`, slot offset, bridge *source*
  address and length: the same four words as 0180, which works. The 48-bit offset
  added in openFPGA 2.1 belongs to `0185`, a different command we never issue, so
  there are no spare bits carrying a stray high half.
- **The RTL.** `core_bridge_cmd`'s 0184 arm is structurally identical to its 0180
  arm and latches `target_20/24/28/2C` at dequeue; `target_dataslot_id` is 16 bits
  zero-extended.

So the configuration is right and the command is issued correctly. It needs the
measurement.

### The test — built, not yet run, and now a prediction rather than a fishing trip

`bash fw/build.sh probe` produces `mp3player.probe.rom`, in which **Select+Start
performs exactly one 0184 and then latches**, painting the result code on screen.
`tools/card_snapshot.py before | after | diff` records every file on the card —
whole-file hashes, sizes in hex — and names the verdict.

Because the mechanism above names a specific number, the run is now falsifiable:
**write down word 1 of the record before pressing anything, and predict the
damaged size.** With the card's current settings (volume 55, palette 3, repeat
ALL) that is `0x01370301` = 20,382,465 bytes. A file landing on exactly that
proves it; a file landing anywhere else refutes it and the mechanism goes back in
the bin. Better still, set the volume to something distinctive first — one press
of Up makes it 60 and the prediction becomes `0x013C0301` = 20,710,145.

Full procedure, including why it must be run on an idle core with nothing loaded:
**[tools/settings_probe.md](tools/settings_probe.md)**. It is destructive by
design; expendable copies only.

Details and the full reasoning live at the `SETTINGS_WRITE` definition in
[fw/settings.inc](fw/settings.inc).

# Enhancements

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

## Preset EQ — feasible, but belongs in the RTL

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

# Fixed

Kept for the reasoning, not the status. Nothing here is outstanding.

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

**Still worth exercising on hardware:** a mistyped *middle* entry (auto-advance
steps over it), a mistyped *last* entry with repeat OFF (playlist ends rather
than restarting at track 1), and an `.m3u` where every entry is wrong (idle
screen reading "No playable tracks in playlist", not the splash).

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
behaviour (whether `0192` can open into an empty `deferload` slot) and this
project has been bitten before by building on unconfirmed APF assumptions.

If it ever becomes worth revisiting, the scratch-slot route is the one to prove
first — it is the only one with no boot cost.
