# Stage 0 — Feasibility measurement results

Cycle-accurate simulation (Icarus Verilog + real PicoRV32 RTL), not estimates.
Test vector: `MEANDR90.mp3` from minimp3's CC0 `vectors/performance/` set —
**320 kbps @ 48 kHz, the maximum MPEG-1 bitrate**, i.e. deliberate worst case.
One frame = 1152 samples = **24 ms** of audio.

Reproduce: `bash build_fw.sh helix && vvp tb_fastmul.vvp +firmware=fw_helix/fw_helix.hex`

## 1. Does the codec work on RISC-V? — YES (Helix)

```
frame 1  err -2  consumed 960   13,389 instr   <- ERR_MP3_MAINDATA_UNDERFLOW
frame 2  err  0  consumed 960  660,682 instr   <- clean decode
frame 3  err  0  consumed 960  668,035 instr   <- clean decode
```

Frame 1's `-2` is **expected and correct**: MP3's bit reservoir lets a frame
reference main_data bytes that precede it, so the first frame of any stream has
nothing to reference. It bails cheaply (13K instructions). Frames 2+ are the
steady state and return `err 0`, each consuming exactly one 960-byte frame.

> Gotcha for later: any benchmark that counts frame 1 as representative will
> under-report decode cost by ~50x. Instrumentation must print the error code.

## 2. How much work is a frame? (CPU-independent)

**~664,359 instructions per frame** (mean of frames 2, 3).

Instructions-retired is the load-bearing metric: it is a property of the codec,
not of the CPU or the testbench memory model. Confirmed empirically — changing
the multiplier config changed cycles by 2.4x while `instret` stayed *identical*
(660,682 both runs).

    required MIPS   = 664,359 / 0.024 s      = 27.7 MIPS
    required clock  = required MIPS x CPI_of_core

Cross-check: 27.7 MIPS matches Helix's documented "20-30 MHz ARM" requirement,
which independently validates the measurement.

## 3. PicoRV32 is the wrong CPU — measured, both configs

| PicoRV32 config | cycles/frame | measured CPI | clock needed for real-time |
|---|---|---|---|
| `ENABLE_MUL` (serial multiplier) | 8,429,474 | **12.69** | **351 MHz** |
| `ENABLE_FAST_MUL` (DSP blocks)   | 3,552,232 | **5.35**  | **148 MHz** |

The serial multiplier costs 2.4x because Helix is multiply-bound — `MULSHIFT32`
(→ `mulh`) saturates the polyphase filterbank, IMDCT and dequantiser. Any soft
core used here **must** have a single-cycle hardware multiplier.

Even at its best, PicoRV32 needs ~148 MHz. That is not achievable on the
Pocket's Cyclone V alongside video/APF/audio logic, and leaves no margin.
PicoRV32 is a non-pipelined multi-cycle design; the ~4 CPI I assumed when
choosing it was optimistic for this workload.

**This was the real project risk — the CPU, not the codec.**

## 4. Helix vs minimp3 — Helix wins decisively

Same harness, same vector, same CPU config (serial mul):

| codec | frame 2 |
|---|---|
| Helix (fixed-point) | 8,429,474 cycles — completes |
| minimp3 (floating-point) | **did not complete in 24.9M cycles** |

>3x slower and never finished. Cause is exactly the risk flagged up front:
minimp3 is float, and RV32IM has no FPU, so every operation goes through
softfloat emulation. Fixed-point Helix is the correct choice for a soft core.

Licence note: Helix is RPSL-1.0 (OSI/FSF-approved; requires source disclosure
and retention of its notice, forbids relicensing). Fine here — every HarpMudd
repo is already source-inclusive and several already carry third-party subtrees
under their own notices.

## 5. Memory footprint — measured, fits entirely in BRAM

From the real RV32IM build (test vector's 9,601 bytes excluded):

| region | bytes |
|---|---|
| `.text` | 40,968 |
| `.rodata` + `.srodata` (tables) | 12,908 |
| **code + tables (ROM)** | **~53.9 KB** |
| `.data` + `.bss` static RAM | ~7.0 KB |
| heap — decoder instance, measured | 34,320 |
| **total RAM** | **~41 KB** |
| **TOTAL** | **~94 KB** |

Analogue Pocket FPGA: Cyclone V 5CEBA4F23C8N — 49K LE, 3,464,192 bits BRAM
= **423 KB**. Decoder needs ~22% of it.

**Architectural consequence: the decoder needs no SDRAM at all.** Firmware,
tables, working RAM and the PCM ring buffer all fit in BRAM. Only the compressed
bitstream streams in from SD — 320 kbps = 40 KB/s, trivial for APF data-slot
reads. This removes SDRAM latency from the decode loop entirely, which is also
what makes a low CPI achievable.

## 6. CPU pivot to VexRiscv — MEASURED, not estimated

VexRiscv "Full" (`pythondata-cpu-vexriscv`, pre-generated Verilog — no Scala
toolchain needed): RV32IM, 5-stage fully bypassed, 4 kB I$ + 4 kB D$,
**single-cycle multiply**, Wishbone iBus/dBus. Run in `tb_vexriscv.v` with the
**identical `fw_helix.hex` binary** used for PicoRV32.

```
frame 1  err -2     18,646 cycles   13,389 instr
frame 2  err  0  1,081,112 cycles  660,682 instr
frame 3  err  0  1,113,491 cycles  668,035 instr
```

`instret` is **byte-identical to PicoRV32's** (660,682 / 668,035), which proves
the two CPUs executed exactly the same work — the entire cycle difference is
microarchitecture, not a measurement artifact.

| CPU | mem model | cycles/frame | CPI | clock needed |
|---|---|---|---|---|
| PicoRV32, serial mul | 1 wait | 8,429,474 | 12.69 | 351 MHz ❌ |
| PicoRV32, fast mul | 1 wait | 3,552,232 | 5.35 | 148 MHz ❌ |
| PicoRV32, fast mul | **0 wait** | 2,739,474 | 4.12 | 114 MHz ❌ |
| **VexRiscv Full** | **0 wait** | **1,097,302** | **1.65** | **45.7 MHz** ✅ |

The third row exists to keep the comparison honest: the first PicoRV32 runs used
a 1-wait-state memory while VexRiscv used 0-wait, so part of the gap was the
testbench rather than the CPU. Re-running PicoRV32 at 0 wait states recovers
~23% (5.35 -> 4.12 CPI) — but **on an identical memory model VexRiscv is still
2.5x faster**, and PicoRV32 still needs an unusable 114 MHz. The conclusion
survives the correction.

VexRiscv also beat the conservative ~63 MHz projection made by scaling Dhrystone
ratios. Dhrystone understates it here because single-cycle multiply matters far
more to a DSP kernel than to Dhrystone. **Lesson: scale by a representative
workload, or just measure.**

### Both bitrates measured on VexRiscv

| content | instr/frame | frame period | CPI | clock needed | margin vs 141 MHz |
|---|---|---|---|---|---|
| 128 kbps / 44.1 kHz (8 steady frames) | 524,488 | 26.1 ms | 1.69 | **33.9 MHz** | 4.2x |
| 320 kbps / 48 kHz (worst case) | 664,359 | 24.0 ms | 1.65 | **45.7 MHz** | 3.1x |

CPI is essentially constant across bitrates (1.65 / 1.69) — performance is not
content-dependent in any surprising way, so the worst-case figure is a genuine
ceiling rather than a lucky sample.

Caveat on the 128 kbps number: the only vector available at that bitrate is a
synthetic 1 kHz sine (`l3-sin1k0db.bit`), whose Huffman entropy is LOWER than
real music. **Treat 33.9 MHz as a floor**; real music at 128-192 kbps will sit
between the two rows. Both are far under fmax, so the conclusion is unaffected.

Note the first 2 frames of that stream return `-2` (not just the first) — at
418-byte frames it takes more than one frame to accumulate the 511-byte maximum
`main_data_begin`. **Any benchmark must decode well past the reservoir fill
before reading steady-state numbers**; 3 frames was not enough here.

45 MHz is unremarkable for this workspace — below the 72 MHz the SDRAM
controller already runs at.

**Verdict: Option 2 is FEASIBLE — Helix + VexRiscv, everything in BRAM.**

## 7. Where the remaining risk actually lives

Stage 0 moved the risk out of the DSP/CPU domain. What is still unproven:

1. **APF runtime file streaming (Stage 2) — the biggest unknown.**
   `target_dataslot_read` (0180) and `target_dataslot_openfile` (0192) are fully
   implemented in `_mister_pocket_lib/core/core_bridge_cmd.v`, but **no core in
   this workspace has ever driven them** — every existing port ties them to
   constant 0. This rests on Analogue's documentation, not on local precedent,
   and the "browse your library" experience depends entirely on it.
   **Recommend proving this on hardware before building a player on top of it** —
   it is testable standalone, with no decoder attached.
2. **Real-time behaviour under I/O load.** The benchmark decodes back-to-back
   with zero interruption. The real player interleaves SD reads, UI and audio
   output. Analogue's docs warn that cluster-chain traversal is slow and that the
   seek cache is dropped when switching data slots — the classic recipe for
   buffer underrun. Needs a decode-ahead buffer; sizing is unproven.
3. **Timing closure on the full design.** 141 MHz is VexRiscv *standalone*; ours
   shares the die with APF, video and audio. 45 MHz should be comfortable, but
   it is not yet demonstrated.
4. **Structurally unlike every other core here** — no MiSTer reference, and the
   `port_recon`/`pack_rom`/`.mra` pipeline does not apply. A soft CPU plus a
   separately-built firmware binary needs its own build/release path.
