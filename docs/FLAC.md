# FLAC on this core

FLAC shipped in v1.3.0. This file is two things, in order: **what the decoder
does and where its limits come from**, and then the working record of how those
limits were found — kept because several confident predictions in it turned out
to be wrong, and the corrections are the useful part.

---

## What it does

A streaming decoder written for this core (`fw/flac.c`). libFLAC and dr_flac
both allocate working buffers per BLOCK — blocksize x channels x 4 bytes, which
is 36,864 for the 4608-sample blocks these files use, against a 24,576-byte
arena. They do not fit and cannot be made to.

This one buffers ONE channel and streams the other into the same array: `ch0[i]`
is read at exactly the instant `ch1[i]` is produced, and channel 1's LPC history
is the entries just overwritten. Working set is blocksize x 4 bytes, half what
the obvious structure needs.

## The limits, and where each comes from

| | limit | why |
|---|---|---|
| Sample rate | **48 kHz** | CPU. Measured, see below. |
| Bit depth | 8, 16, 20, 24 | no conversion path for 32-bit |
| Channels | mono, stereo | the decoder is two-channel by construction |
| Block size | 6144 samples | the arena is 24,576 bytes at 4 bytes a sample |
| Container | native FLAC | no Ogg demuxer |

**The rate ceiling is measured, not chosen.** Decode cost as a percentage of
realtime, with I/O excluded:

| file | format | cost |
|---|---|---|
| Pink Floyd — The Show Must Go On | 16/44.1 | **74%** |
| Psychedelic Furs — The Boy That Invented Rock & Roll | 24/44.1 | **80%** |
| Jerry Garcia — Alabama Getaway | 24/88.2 | **150%** |
| Circles Around the Sun — Third Sunrise Over Gliese | 24/96 | **180%** |

Cost tracks sample rate almost exactly. 48 kHz lands near 87% for 24-bit, which
fits; 88.2 kHz would need the decoder to be half again faster, and the largest
single optimisation available — a 64-bit bit reservoir — bought 1.3x on one of
its two passes. It is not a setting that can be raised.

Anything outside the table is refused with the reason on screen rather than
played badly, because a file decoding at 150% of realtime sounds broken in a
way a listener cannot distinguish from a damaged file or a broken core.

## The meter cadence, which is not a bug

FLAC meters do not move as evenly as MP3's, and they will not be made to.

subframe() decodes ALL of channel 0 emitting nothing -- a stereo pair cannot be
reconstructed until channel 1 arrives, and buffering one channel rather than
two is the only reason this decoder fits the arena. So ~40 ms of every 104 ms
frame produces no meter data at all. MP3's frames are 1152 samples and its
longest gap is 23 ms.

Three fixes were made around this and all of them were about not WASTING the
information that exists -- an even UI refresh, peaks accumulated as a maximum
instead of overwritten, and a deadline that was dead for 18 seconds at a time.
None of them can create information that has not been decoded yet. What is
left is the format's frame size meeting a memory-constrained decoder, and the
only real cures are a larger arena or a faster CPU.

## Two bugs worth remembering

**The frame header CRC-8 is not optional.** During sequential playback frames
abut, so the sync scan never runs — seeking is the only path that lands
mid-stream and searches. Scanning for the 14-bit sync pattern alone hits a
FALSE sync inside audio data within a handful of frames on every test file.
Requiring byte alignment and verifying the CRC is what made seeking usable.

**Seeking must use the SEEKTABLE.** Between two seek points of one test file
the rate is 164 KB/s, and between the next two it is 213 KB/s. A single average
across a 30% swing lands somewhere different every press. Interpolating between
the two points bracketing the target is what fixed it; files with no seek table
interpolate across the whole file, anchored at the first audio frame.

---

# The working record

Everything below is the investigation as it happened, including the parts that
were wrong. The estimates in the next section were superseded by the
measurements above — they are kept because how they failed is instructive.

---

## The short version (SUPERSEDED — see the measurements above)

**Decoding FLAC is the easy part.** It is Rice coding plus an LPC filter — all
integer, no MDCT, no synthesis filterbank. It is *cheaper* than MP3, and the
CPU has room.

**Getting the data off the card is the hard part.** FLAC is lossless, so a CD
track runs 700–1000 kbps against MP3's 128–320. That is ~112 KB/s sustained
where **40 KB/s is the most this core has ever had to hold**.

Nobody has measured what the card can actually sustain. That single number
decides the feature, and a build that measures it is on the branch now.

**And the payoff is smaller than it sounds** — see *What you would actually
hear*, because it changes how much risk is worth taking.

---

## The three budgets (SUPERSEDED — CPU was the binding one, not I/O)

### 1. CPU — comfortable (WRONG: it is the binding constraint)

| | |
|---|---|
| available | 60 MHz |
| MP3 decode at 1× | ~45.7 MHz |
| headroom | **~14 MHz** |

Derived from the measured 54.8 MHz that 1.2× playback needs. FLAC decode is
materially lighter than MP3, so it fits — this is not the constraint.

### 2. RAM — tight, but it is a swap not an addition

Only one decoder is ever resident: a track is MP3 **or** FLAC, never both. So
the FLAC decoder replaces Helix rather than joining it.

| | |
|---|---|
| Helix, measured | 23,816 bytes |
| FLAC block buffer (4096 samples, stereo, 16-bit) | ~16 KB |
| FLAC decoder state | a few KB |

That fits in the same envelope. **But it must genuinely share** — see the
warning below, because this exact space just bit us.

### 3. Throughput — THE UNKNOWN (answered: 736 KB/s, never the problem)

| | |
|---|---|
| proven today | 40 KB/s (MP3 at 320 kbps) |
| FLAC needs | ~112 KB/s |
| card can sustain | **unmeasured** |

Everything hinges here.

---

## The buffering trap

This is the part that makes throughput more dangerous than a simple
"is it fast enough" question.

The compressed ring is a fixed **32 KB**. That is a fixed number of *bytes*,
so as bitrate rises it holds less *time*:

| format | bitrate | ring holds |
|---|---|---|
| MP3 320 kbps | 40 KB/s | **0.82 s** |
| FLAC ~900 kbps | 112 KB/s | **0.29 s** |

**Protection against a stall falls 2.8× at exactly the moment demand rises
2.8×.** And stalls are real: APF drops its fragment cache on every track
change, and random reads are what made the old size probe cost 480 ms.

Restoring 0.8 s of protection at FLAC rates needs a ~90 KB ring. That room
does not exist.

---

## What you would actually hear

**The output is fixed at 16-bit / 48 kHz.** `sound_i2s` hands the APF audio
interface 15 magnitude bits plus sign, at 48 kHz. That is the Pocket's
interface — not a choice this core makes, and not something a better source
format can lift.

So a lossless file is downconverted to the same 16/48 output as a 320 kbps
MP3, through a handheld headphone amplifier. **The honest expected difference
is small**, and on this hardware many listeners would not hear one at all.

That does not make it pointless — "my library is FLAC and I want to play it
without converting" is a completely legitimate reason, and probably the real
reason it is requested. But it does mean **a dropout is a bad trade for it**.

---

## What else changes

FLAC is not just a different decoder. It is a different *file*:

| | MP3 today | FLAC |
|---|---|---|
| metadata | ID3v2 frames | Vorbis comments |
| album art | APIC frame | METADATA_BLOCK_PICTURE (base64 in a comment) |
| duration | frame count / Xing | STREAMINFO — actually easier |
| seeking | byte offset × bitrate | SEEKTABLE, or frame-header search |

The tag and art readers are MP3-shaped throughout. FLAC needs its own path for
both — that is real work beyond the decoder, and it is the part most likely to
be underestimated.

---

## MEASURED 2026-08-14 — throughput passes, comfortably

**736 KB/s sequential.** FLAC needs ~112 KB/s, so that is a **6.5× margin** —
far past the 200 KB/s I set as the comfortable threshold.

**Throughput is not the constraint.** The thing that could have ruled FLAC out
outright does not.

Two corollaries worth having:

- The ring holding only 0.29 s at FLAC rates matters much less when the card
  refills it 6.5× faster than it drains.
- Even a 3149 kbps hi-res file (394 KB/s) is inside the measured ceiling, which
  I did not expect.

## The REAL constraint is RAM, and it is worse than this document first said

The original estimate here — "~16 KB block buffer" — assumed 16-bit storage.
A FLAC decoder does not work in 16-bit. Residuals and LPC intermediates are
32-bit, so the working buffers are **blocksize × channels × 4 bytes**:

| | blocksize | working buffers |
|---|---|---|
| CD rip, 16/44.1 | 4096 | **32,768 B** |
| the sample on the card, 24/96 | 4608 | **36,864 B** |

Against a decoder arena of **26,624 bytes**, all of which Helix currently needs
23,824 of. **A conventional FLAC decoder does not fit**, and no amount of
shuffling the playlist buffer changes that — the gap is ~10 KB, not 2.

### The way through: decode as a stream, not a block

FLAC does not actually require the whole block resident. Residuals arrive in
Rice partitions, and LPC prediction needs only the last `order` samples of
history — 32 at most. So a decoder can work a partition at a time, apply LPC
with a rolling history, and push PCM to the FIFO as it is produced.

Working set then falls to a few hundred bytes plus the FIFO staging buffer,
and blocksize stops mattering at all.

The cost is that no off-the-shelf decoder does this. libFLAC and dr_flac both
allocate per-block buffers, so this means a purpose-written decoder: roughly
800–1200 lines for the 16-bit path. That is the real scope of the feature, and
it is why "FLAC is cheaper than MP3 to decode" was never the useful fact.

## About the sample file

`Circles Around the Sun - Third Sunrise Over Gliese.flac` is **24-bit / 96 kHz,
blocksize 4608, 3149 kbps average, 6.4 minutes, 144 MB**, with a SEEKTABLE, a
Vorbis comment written by Mutagen, and a 59 KB embedded PICTURE.

It is the hardest case, and this document had already scoped it out: the output
is 16/48 regardless, so 24/96 buys nothing audible while costing **2.2× the
decode work** of a CD rip and needing 36,864 bytes of working buffers.

**A 16-bit / 44.1 kHz CD rip is the target to develop against.** The hi-res
file is still useful later — as the thing that must be *rejected cleanly*
rather than played badly.

## The original gating measurement (now answered)

**Can the card sustain ~112 KB/s sequential?**

A burst benchmark is on the branch (`IO_BENCH`). It reads 32 sequential 4 KB
chunks back-to-back with no decoding in between, during the silent part of a
track load, into scratch memory that cannot disturb audio. It reports on the
diagnostic row as `IO<n>KB`.

It has to be a burst. During playback the refill rate is limited by **demand**,
so timing normal playback measures the file's bitrate and reports it as
capacity. And it has to be sequential — the old size probe's 480 ms over ~20
reads was random offsets making APF re-walk the cluster chain, which is not
representative of streaming.

### Reading the result

| measured | verdict |
|---|---|
| **< 130 KB/s** | **No.** No margin over the 112 KB/s demand. Any stall is a dropout. |
| **130–200 KB/s** | **Marginal.** Possible with a bigger ring, which means finding RAM. Prototype and listen hard. |
| **> 200 KB/s** | **Yes.** Comfortable margin; the remaining work is ordinary. |

---

## Warning from tonight, which applies directly

Raising the playlist cap to 256 tracks nearly shipped a core that could not
decode **at all**.

The space between the end of BSS and the reserved DMA buffers is not free
space — **it is the heap**, and Helix mallocs 23,816 bytes of it at every track
load. Sizing a static buffer against the whole gap left a 15 KB heap;
`MP3InitDecoder()` returns 0 below ~24 KB. The build passed. The linker was
satisfied. The rom was written.

Two comments in the tree said Helix needed "~34 KB". Both were wrong. The real
figure came from measuring the structs.

**For FLAC this is the central risk, not a footnote.** Any FLAC work must
budget against the heap, not the address gap, and the two decoders must share
that space rather than both hold it. A second linker ASSERT now enforces the
heap minimum, so this specific mistake fails the build instead of shipping.

---

## Recommended order

1. **Measure.** Flip on the build that is already waiting. Two minutes.
2. **Decide** against the table above. If it is under 130 KB/s, stop — and the
   honest answer to the request is that the hardware will not hold it.
3. If it clears: **prototype decode only.** No tags, no art, no seeking. Get
   audio out of one known-good 16/44.1 file and listen for dropouts across a
   whole track.
4. Only then: Vorbis comments, METADATA_BLOCK_PICTURE, seeking, the UI.

**Scope limit: 16-bit / 44.1 kHz.** 24/96 is not worth attempting — it doubles
the throughput problem to reach an output that is 16/48 regardless.

---

## CORRECTION 2026-08-14 — the CPU estimate above was wrong

First hardware playback, four files from the test card:

| file | format | bitrate | result |
|---|---|---|---|
| Pink Floyd — The Show Must Go On | 16/44.1 | 843 kbps | **plays clean** |
| Psychedelic Furs — The Boy That Invented Rock & Roll | 24/44.1 | 1635 kbps | hiccups |
| Jerry Garcia — Alabama Getaway | 24/88.2 | 2501 kbps | hiccups |
| Circles Around the Sun — Third Sunrise Over Gliese | 24/96 | 3149 kbps | hiccups |

The Furs file is the one that matters. It runs at **the same sample rate** as
the file that plays perfectly, and it still hiccups — so the difference is bit
depth alone, and "§1 CPU — comfortable" was reasoning from the wrong variable.

**Cost tracks BITRATE, not sample rate.** The reason is the bit reader, not the
arithmetic: 24-bit residuals are larger, so each sample carries more coded bits
(9.6 bits/sample for the Pink Floyd file against 18.5 for the Furs). The LPC
and Rice work per sample is essentially unchanged by depth; the bits pulled
through the reader roughly double. A cost model built on samples/second could
never have seen that.

The corollary is that the tier table in §1 should be read as a **bitrate**
table. Anything near 850 kbps is fine; the failures start somewhere between
that and 1635, and the exact cutoff has to be measured, not derived — the same
mistake twice would be careless.

### Attribution before optimisation

"The decoder cannot keep up" and "the ring is starved" both present as
hiccups and need opposite fixes, so guessing between them was not worth a
hardware round trip. The diagnostic row now reports, per second:

- **D** — percent of the second spent idle, waiting on a full FIFO
- **O** — percent spent blocked in `flac_pull` waiting for bytes
- **U** — underrun edges since boot, i.e. the audible fault itself

Read D and O together. High D with underruns means the decoder is fast enough
and the ring is the problem — worth noting because the ring was just cut from
32 KB to 24 KB to make room for the decoder, which at 204 KB/s holds only about
0.12 s. Both near zero means the decoder genuinely cannot keep up.

### What was done anyway, because it was cheap and safe

All on the per-sample path, all verified against the reference decoder:

1. **64-bit bit reservoir**, refilled four bytes at a time instead of one.
2. **`unary()` by `__builtin_clzll`** rather than one iteration per zero bit.
3. **Hot functions at `-O2`**, the rest of `flac.c` still `-Os`. The whole file
   at `-O2` does not link — the heap assert in `link.ld` catches it — but the
   five functions on the per-sample path do fit.
4. **Word-at-a-time copy out of the ring**, which is UNCACHED, so every byte
   there was a separate bus transaction.
5. **`wasted == 0` fast path** in the FIXED and LPC loops, removing one
   variable shift per tap on the dominant inner loop.

Two of these were nearly bugs and are worth recording: capping the reservoir at
63 bits matters because `1 << bitcnt` is undefined at 64, and the first version
of the leading-zero count scanned bit by bit, which would have made `unary()`
*slower* than what it replaced.

---

## MEASURED 2026-08-14 — where the cycles actually go

The correction above was still reasoning from bitrate rather than from cycles.
These are cycles, taken on hardware with the decoder byte-identical to the
build that plays a CD rip cleanly.

`L` is decode cost as a percent of realtime and is **not capped at 100** — the
point of it. `R` is the Rice/bit-reader pass as a share of channel 0 against
the reconstruction pass. `P` is the LPC order. `D` is idle, `O` is blocked on
the card.

| file | format | L | R | P | D | O | **L+O** |
|---|---|---|---|---|---|---|---|
| MP3 (control) | — | — | — | — | 11 | 0 | — |
| Pink Floyd | 16/44.1 | 76 | 64 | 6 | 24 | 0 | **76** |
| Psychedelic Furs | 24/44.1 | **95** | 70 | 11 | 0 | 23 | **118** |
| Jerry Garcia | 24/88.2 | 157 | 76 | 5 | 0 | 35 | **192** |
| Circles Around the Sun | 24/96 | 191 | 67 | 11 | 0 | 39 | **230** |

The MP3 row is the control and the reason the rest can be trusted. MP3 at 1×
was independently measured at ~45.7 MHz of 60, predicting about 24% idle; D
reads 11–13. An earlier round of readings showed D0 on *every* file including
one that played perfectly, which is what a broken instrument looks like — that
build had a regression, and its numbers were discarded rather than explained.

### Three things this says that guessing did not

**The Furs decodes in 95% of realtime.** It fits. What breaks it is the 23% of
I/O stacked on top. For that file the decoder was never the wall, and every
estimate up to this point — including the bitrate correction above — had aimed
at the wrong thing.

**O is not the card being slow.** Demand against the measured 736 KB/s: the
Furs 204 KB/s = 28%, Garcia 313 = 42%, Circles 393 = 53%, against measured O of
23, 35 and 39. It is transfer time the CPU *blocks* on. Pink Floyd shows O0
because D24 of idle lets the async pump run inside the full-FIFO wait — so
**O is a consequence of D reaching 0, and it compounds**: once decode saturates,
refills stop being started early and the only ones left are blocking.

**R of 64–76% puts the bit reader on top**, and its share rises with bitrate.
That is the same conclusion the bitrate correlation reached, now with a
number behind it rather than an inference.

### What follows

Two fixes, aimed at the two measured bottlenecks, each independently visible on
the row — L for decode, O for I/O:

1. `refill_pump()` from `flac_pull()`, so reads are started while decoding
   rather than only inside an idle wait that no longer exists.
2. The 64-bit reservoir and `clz`-based `unary()`, reinstated as plain C with
   none of the parts that caused the regression.

Predicted, so it can be wrong on the record: Furs to about L70 and playable,
Garcia to ~112 and Circles to ~143, both still over. If that holds, the honest
shape for this release is FLAC up to 48 kHz — which covers essentially every CD
rip — and a clear on-screen refusal for hi-res files rather than letting them
play badly.

---

## MEASURED 2026-08-14 (second pass) — after the bit reader

With the frame-header bug fixed and the bit reader working on valid input:

| file | format | L before | **L after** | R | P | D | O | L+O |
|---|---|---|---|---|---|---|---|---|
| Pink Floyd | 16/44.1 | 76 | **74** | 60 | 6 | 27 | 0 | 74 |
| Psychedelic Furs | 24/44.1 | 95 | **80** | 67 | 11 | 0 | 27 | **107** |

The bit reader helps in proportion to coded bits per sample, exactly as the
bitrate model predicts: Pink Floyd at 9.6 bits/sample gains 2 points, the Furs
at 18.5 gains 15. It is a 1.3x on the Rice pass, not the 1.6x predicted — the
prediction was optimistic and the measurement stands.

**The Furs now decodes in 80% of realtime and is sunk entirely by 27% of I/O.**
Seven percent over the line, with the whole overage on the I/O side.

### A correction worth keeping

The corruption in 67efaa8 — LPC orders of 21 and 18, R at 0, files leaving the
FLAC path — was diagnosed as a race between the async and blocking read paths.
That race does not exist: target_read_start_slot() already calls
refill_drain(), so there is exactly one command in flight ever. The real cause
was the frame-header bug (f0f6bac), which shipped in the same build.

Two costs came from getting that wrong. The "fix" for the imaginary race turned
every failed read into a ~35-second blocking retry chain, and the genuinely
working I/O change was reverted alongside it on suspicion. Bundling two changes
meant neither could be judged, and the wrong one was blamed.
