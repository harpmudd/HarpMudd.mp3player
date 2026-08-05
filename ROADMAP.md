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

**None open.** The settings-persistence blocker is resolved — see Fixed.

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
