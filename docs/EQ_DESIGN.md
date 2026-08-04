# Preset EQ — design outline

How a preset equaliser would fit this core. Written before any code so the
decisions that are awkward to reverse — where it sits, what rate it runs at, what
it is allowed to consume — are settled on paper.

Nothing here is built. See [ROADMAP.md](../ROADMAP.md) for why this ranks ahead
of new formats.

## The one-paragraph version

A time-multiplexed biquad engine in the RTL, sitting between `pcm_fifo` and the
`audio_l`/`audio_r` outputs of `mp3_soc`, running at a fixed ~48 kHz strobe in
the existing `clk_sys` domain. Five bands per channel, one multiplier, no block
RAM, no new clock domain crossing, and no CPU cost at all. Firmware picks a
preset by writing one register.

## Where it goes

[`src/fpga/core/mp3_soc.v:390-403`](../src/fpga/core/mp3_soc.v#L390-L403) — the
`pcm_fifo` instance currently drives the module's `audio_l`/`audio_r` outputs
directly:

```
    pcm_fifo → audio_l / audio_r → (core_top) → sound_i2s
```

The EQ splices in there:

```
    pcm_fifo → eq_biquad → audio_l / audio_r → (core_top) → sound_i2s
```

This is deliberately the smallest possible incision. `core_top.v` does not
change — it is frozen per the workspace convention and there is no reason to
touch it. `sound_i2s` does not change. The only edited file is `mp3_soc.v`, and
only to rename the FIFO's outputs and add one instance.

**No new CDC.** `pcm_fifo` and the whole SoC already live in `clk_sys`, and
`sound_i2s` already crosses into `clk_74a` internally through its own
`sync_fifo`. The EQ sits entirely on the `clk_sys` side of that existing
boundary, so it adds no crossing. This matters: a clock-domain bug in an audio
path is exactly the kind of intermittent fault that costs a week.

## What rate it runs at, and why not the obvious one

`pcm_fifo` pops at the *file's* sample rate — firmware writes `R_PCM_RATE` with a
fractional increment derived from `MP3FrameInfo.samprate` — and holds `out_l`/
`out_r` between pops. `sound_i2s` samples that held value asynchronously. So
there is already a zero-order-hold resample from 32/44.1/48 kHz up to the DAC's
fixed 48 kHz.

That leaves two places to filter.

**At the FIFO pop rate (the source rate).** Filters the true signal before the
hold. But biquad coefficients are a function of sample rate, so every preset
would need a coefficient set per supported rate — three times the storage, plus
firmware logic to select on `samprate`, plus a reload glitch whenever a track
changes rate.

**At a fixed 48 kHz after the hold.** One coefficient set per preset, no
dependence on the file, nothing to reload on a track change. The cost is that
the filter sees ZOH imaging artifacts — but those are already present in what
reaches the DAC today, and a gentle shelving/peaking EQ well below Nyquist does
not meaningfully interact with them.

**Take the fixed 48 kHz option.** The simplification is large and the penalty is
theoretical.

Generating the strobe is trivial and needs no fractional accumulator:
`clk_sys` is 60 MHz and 60,000,000 / 48,000 = **1250 exactly**, so a mod-1250
counter produces it. This free-runs against the DAC's real rate, which is derived
from `clk_74a` — the two drift by whatever the two PLLs differ by. That drift
shifts the EQ's corner frequencies by the same fraction, i.e. a small fraction of
a percent. Irrelevant.

## Resource budget — the constraint that actually bites

From the current fit report:

| Resource | Used | Note |
|---|---|---|
| DSP blocks | 7 / 66 (11%) | ample |
| ALMs | 3,437 / 18,480 (19%) | ample |
| **M10K RAM blocks** | **300 / 308 (97%)** | **effectively exhausted** |

The framebuffer owns the block RAM. **The EQ must not use M10K.** Coefficients
and filter state go in ALM-based storage — MLAB / LUT-ROM — which is the resource
there is plenty of. This is the single most important implementation constraint
and the easiest one to violate by accident, because inferring a small RAM in
Verilog usually lands in M10K unless told otherwise (`ramstyle = "MLAB, no_rw_check"`).

Sizing at **five bands per channel** (ten biquads total):

- **State:** 10 biquads x 4 words (x1, x2, y1, y2) x 32 bits = 1,280 bits.
- **Coefficients:** 5 bands x 5 coefficients x 18 bits = 450 bits per preset;
  eight presets = 3,600 bits of ROM.

Both are small enough to sit in logic without noticing.

## Why five bands and not ten

Ten bands doubles state, coefficients and cycles for a control surface nobody
uses on a preset EQ. Five is the classic preset shape — low shelf, low-mid peak,
mid peak, high-mid peak, high shelf — and is what the named presets (Rock, Jazz,
Vocal, Bass, Flat) are actually describing. If per-band user control is ever
wanted, the engine below scales to ten by changing one parameter; the cycle
budget has room for it many times over.

## The engine

One multiply-accumulate datapath, time-multiplexed across all ten biquads.

Per 48 kHz sample there are **1250 clocks**. A Direct Form I biquad is five
multiplies; ten biquads is 50 MACs. Even at one MAC per cycle plus pipeline
drain and per-band overhead, that is **~60-80 cycles — about 6% of the budget**.
There is no need for parallelism, no need for a second multiplier, and no timing
pressure. One DSP block, up from 7 to 8 of 66.

Sequence per sample tick:

1. Latch `out_l`/`out_r` from the FIFO.
2. For each of the 5 bands, for each channel: read state and coefficients,
   evaluate the biquad, write state back, pass the output to the next band.
3. Apply the preset preamp, saturate to 16 bits, register into `audio_l`/`audio_r`.

Cascading the bands means each band's output feeds the next, so only the final
result is saturated — intermediate stages keep full accumulator width.

## Fixed point

Everything is integer; there is no FPU anywhere in this design and none is
wanted.

- **Coefficients: Q2.16 signed (18 bits).** `a1` reaches magnitude 2 for
  low-frequency high-Q sections, so two integer bits are required. Q1.17 would
  overflow on exactly the bands a bass preset needs.
- **State: 32 bits.** Storing state at 16 bits is the classic fixed-point IIR
  mistake — quantisation feeds back and produces limit cycles, audible as a low
  rumble on silence. The extra width costs 640 bits of MLAB and removes the
  entire failure mode.
- **Accumulator: 40 bits**, ample headroom for five cascaded stages.
- **Rounding: round-to-nearest**, not truncate. Truncation in an IIR feedback
  path biases toward a DC offset.

**Preamp is not optional.** A preset that boosts any band can exceed full scale
on material already mastered near it, and the result is hard clipping — much
worse than the EQ is good. Each preset carries a negative preamp gain sized to
its maximum boost, applied before the output saturation. Compute it when the
coefficients are generated, store it alongside them.

## Bypass must be bit-exact

The "Flat" preset must be a **true bypass** — a multiplexer that routes
`pcm_fifo`'s output to `audio_l`/`audio_r` untouched — and not a biquad loaded
with unity coefficients. A unity biquad is not an identity function in fixed
point: it still rounds. Users who leave EQ off should get exactly the audio they
get today, provably, and that is also the thing that makes an A/B test
meaningful when tuning the other presets.

## What the user actually experiences

**Y cycles presets.** `Y` is completely unused today — it is not even `#define`d,
and bit 7 sits empty between `KEY_X` and `KEY_L1`. `input.json` declares no keymap
restrictions, so the button already arrives in `cont1_key`; enabling it costs one
line of firmware and no JSON change. `Select`+`Y` cycles backwards, exactly
mirroring `X` / `Select`+`X` for the meters, so the gesture is already learned.

Eight presets, wrapping:

| # | Label | Character |
|---|---|---|
| 0 | `FLAT` | true bypass — bit-identical to today |
| 1 | `BASS` | low shelf lift, gentle upper-mid dip |
| 2 | `ROCK` | smile curve — lows and highs up, mids back |
| 3 | `POP` | presence lift around 2-4 kHz |
| 4 | `JAZZ` | warm lows, relaxed upper-mid |
| 5 | `CLASSICAL` | near-flat with a slight high-shelf air lift |
| 6 | `VOCAL` | mid forward, lows trimmed |
| 7 | `TREBLE` | high shelf lift |

Each press raises the same transient label the meters and colours use —
`EQ: ROCK` in the info bar above the progress meter, ~1 s at full brightness then
a ten-step fade. Playback is not interrupted: the filter is in the RTL and the
audio never stops flowing, so a preset change is seamless in a way a track change
can never be.

### The meter shows the CURVE while you flip (user's idea, 2026-08-04)

A name alone tells you which preset is selected; it does not tell you what it
does. The meter area is 36 bars over 72 px and is the one large graphic region on
screen — so on a preset change it becomes the preset's **actual response curve**
for about a second, then melts back into the live meter.

**Drawn from the verified coefficients, not by hand.** `gen_eq_coeffs.py
--curves` emits `eq_curve[8][36]`: a signed pixel offset from the meter's centre
line per bar, boost above and cut below, generated from the same quantised
integers the RTL filters with. The picture therefore cannot drift from the
filter, which a hand-drawn icon set inevitably would. 288 bytes.

It is the response *without* the preamp: the preamp is a level adjustment, and
what the user wants to see is the tonal shape, not how it was normalised.

Rendered at the true 36x72 geometry the shapes are unmistakable at a glance:

```text
   ROCK                                 TREBLE
   |##                                  |                                   #
   |#####                               |                                  ##
   |#######                           ##|                                 ###
   |#########                        ###|                           #    ####
   |###############          ###########|                         ###########
   |====================================|####################================
   |                   ###              |
```

**Reuses the bar renderer exactly as it is.** Same 36 rects the meter already
draws every frame, at heights that happen to come from a table instead of from
`pcm[]`. No new primitive, no new cost, and it lands in the one budget that must
not be disturbed only for the second or so it is up — during which the decoder is
running normally anyway.

**The return is a morph, not a dissolve.** Interpolate each bar's height from the
curve to the live meter's value over ~10 frames, so the curve visibly melts into
the music rather than blinking out. Same cost as drawing the meter.

`FLAT` is a flat line across the centre, which is exactly the right picture for
a true bypass.

This supersedes "nothing else on screen moves". The persistent `EQ` tag in the
mode row below still earns its place: the curve is transient, and a user who
walks away and comes back needs to see the state without pressing anything.

**A persistent indicator, not just a toast.** The label fades, so a user who
walks away and comes back needs to be able to see the EQ state without pressing
anything. An `EQ` tag joins the repeat/shuffle icons in the mode row, dimmed when
`FLAT` and drawn in the accent colour otherwise — the same dimmed-not-hidden
treatment those icons already use, which means it reads as "off" rather than
"missing".

**It works while paused or stopped.** The toast appears and the preset takes
effect; there is simply nothing to hear until playback resumes. No special case
needed.

### Two things the user would notice that the engine alone does not solve

**1. Changing preset will click unless it is handled.** Swapping biquad
coefficients while the filter state is non-zero is a step discontinuity, and a
step discontinuity is exactly the click this project spent ten rounds chasing in
the FIFO. Do not ship it and hope.

The cheap fix is a **~4 ms duck**: ramp the output gain to zero over ~2 ms, swap
the coefficients and clear the state at the bottom, ramp back over ~2 ms. It
reuses the preamp multiplier that is already in the datapath, costs no extra
hardware, and 4 ms is below the threshold where it reads as anything but
instant. A crossfade between old and new filter outputs is the higher-fidelity
option but needs both filters resident, which doubles the state for a difference
nobody could hear on a preset change.

Whichever is chosen, it belongs in the simulation plan: assert that the output is
continuous across a preset change, with no sample-to-sample step exceeding a
threshold.

**2. The meters will not react to the EQ.** They are computed in firmware from
`pcm[]` — the decoder's output buffer, before it is pushed to the FIFO and
therefore before the RTL filter. A bass preset would be plainly audible and
completely invisible on the bars, the VU needles and the waveform.

Three honest options:

- **Accept and document it.** The meters show what the *file* contains; the EQ
  shapes what the speaker does. Defensible, and free.
- **Apply the EQ curve to the meter levels in firmware.** A per-band gain applied
  to a level estimate is cheap and approximately right, but it is a second
  implementation of the filter that can drift from the real one.
- **Feed the meters from post-EQ audio.** Correct, and the most work: the RTL
  would need to expose a peak/level register per channel for firmware to read
  back, and the meters would move from a per-frame batch to a sampled value.

The first is the right starting point. The third is the right answer if the EQ
becomes something users lean on, and the RTL should reserve a register address
for it so that change does not become a rework.

### The honest current-state caveat

`SETTINGS_WRITE` is 0, so **the EQ choice would reset to `FLAT` on every
relaunch**, exactly as volume and accent colour do now. The settings byte should
be added regardless — the record is versioned and the field is free — but the
README must not describe EQ as remembered until the write blocker is resolved.

## Firmware interface

One new register, decoded beside `R_PCM_RATE` in `mp3_soc.v`:

```
    R_EQ    write: preset index (0 = flat/bypass)
```

Firmware side mirrors the accent colour exactly, which was just built and is a
good template:

- a preset-name table with a `_Static_assert` tying its length to the preset count
- a control binding to cycle presets
- `ui_toast_set("EQ: ", 0xFFFFFFFFu, eq_name[eq_idx])` for the on-screen label
- `settings_mark_dirty()` and a byte in the settings record

**Caveat:** `SETTINGS_WRITE` is currently 0, so an EQ choice will not survive a
relaunch until that blocker is resolved. Add the settings byte anyway — the field
is free and the record is versioned — but do not describe it as persistent in the
README until writing is safe again.

The binding is `Y`, which is free — see above. No other control moves.

## Verification, before any hardware

This is a numeric design, which is the category this project has repeatedly got
wrong by assuming instead of measuring. The plan is therefore fixed in advance:

1. **Generate coefficients in python, print the table, paste it in.** Never
   hand-write them. A hand-written sine table for the VU needle produced a 45
   degree sweep instead of 100 and cost a hardware round; filter coefficients are
   far less forgiving than that was.
2. **Reference model first.** Write the biquad cascade in python at the same
   fixed-point widths, and plot the magnitude response of every preset. Confirm
   the curves are the intended shapes and that no preset's peak gain exceeds its
   preamp allowance.
3. **`iverilog` testbench against that model.** `iverilog` is installed
   (`C:\iverilog\bin`, not on PATH). Drive an impulse and compare the RTL output
   sample-for-sample against the python cascade — exact equality, since both are
   integer. Then a swept sine, measuring output magnitude per frequency, and
   compare against the designed response.
4. **Assert bit-exact bypass**: with preset 0 selected, output must equal input
   for a long random stimulus, with no exceptions.
5. **Silence test**: feed zeros for a million samples after a loud passage and
   assert the output reaches and stays at exactly zero. This is the limit-cycle
   check, and it is the one that catches insufficient state width.

Only then compile.

## Risks

- **This needs a Quartus recompile**, the first in a long firmware-only run.
  Budget a timing-closure round. The design is small and slow, so the risk is
  low, but it is not zero.
- **Block RAM at 97%** leaves no room for an accidental M10K inference. Check the
  fit report's RAM block count after the first compile and treat any increase as
  a bug.
- **Preset taste is subjective** and cannot be verified in simulation. Expect a
  hardware listening round after the numeric work passes, and expect to adjust
  the curves rather than the engine.
