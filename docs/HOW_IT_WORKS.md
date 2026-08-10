# How it works

The parts of this core that were interesting to build. None of it is needed to
use the player — see the [README](../README.md) for that.

## Software decoding on a CPU that did not exist

There is no MP3 decoder chip in the Pocket, so the FPGA is loaded with a RISC-V
CPU running at 60 MHz and the decoder runs on it as software. Simulation put the
real-time floor around 46 MHz before any hardware was built, which is where the
headroom comes from.

Two candidates were measured before either was committed to. minimp3 was over
three times slower and never completed a frame, because it is floating point and
RV32IM has no FPU, so every operation went through software emulation. PicoRV32
needed between 114 and 351 MHz depending on configuration — not achievable here.
VexRiscv at 1.65 cycles per instruction was, with margin. Full numbers in
[STAGE0_RESULTS.md](../STAGE0_RESULTS.md).

## Keeping audio and video out of each other's way

Decoded audio goes into a hardware queue that drains at the file's own sample
rate, so the CPU can spend ~20 ms on a frame without the sound breaking up.

The display works the same way: the CPU sends drawing commands to a framebuffer
engine rather than writing pixels itself. That matters more than it sounds —
drawing from the CPU was measured causing audible jitter, because every pixel
written came out of the same budget that keeps the audio queue fed.

## Two framework bugs worth knowing about

Reading files uses Analogue framework commands no core had driven before, for
random reads and for opening a file by name.

The first problem was a handshake: the "command finished" signal stays asserted
until the *next* command starts, so a naive reader sees the previous command's
completion and every read after the first returns nothing. Only the very first
command after reset behaves — which is exactly the boot-works, reload-doesn't
signature that misled three rounds of debugging.

The second was a memory-map collision, and it is undocumented anywhere — it was
found by dumping that memory on hardware. The framework keeps its record of
every data slot's size in the same small area a core uses to talk to it, right
at the start. A core that puts its own scratch there loses that record on the
first track change. This core's scratch lives above it, and it carries a
diagnostic that checks the table is still intact rather than assuming it —
compiled out of a release build, one flag away.

## The equalizer is hardware

Five cascaded biquads per channel share one time-multiplexed multiplier, using
116 of the 1,250 clocks available between output samples — under 10%. The
decoder never knows it is there, so changing preset is seamless: no gap, no
reload, no interruption.

Its coefficients are generated and checked against a bit-exact model before
anything is compiled, and the hardware is then verified sample-for-sample
against that model. Presets are loudness-matched rather than peak-matched, so
switching between them changes tone without changing apparent volume. Design
notes in [EQ_DESIGN.md](EQ_DESIGN.md).

## Playlists

Switching tracks leans on the same file path: the core asks the framework to
describe the file already in the slot, then hands that description back with one
path component changed. Nothing about the layout is assumed.
