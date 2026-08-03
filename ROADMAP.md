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
