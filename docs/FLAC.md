# FLAC on this core — the complete picture

The most-requested feature. This is what it would actually take, what it would
actually give you, and the one number that decides it.

---

## The short version

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

## The three budgets

### 1. CPU — comfortable

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

### 3. Throughput — THE UNKNOWN

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
