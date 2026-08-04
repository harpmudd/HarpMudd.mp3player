> **RETIRED 2026-08-04.** This procedure tested the `0184` write, which no
> longer exists in the core. The fault it was built to find was confirmed —
> `0184`'s bridge address is treated as a parameter struct whose second word APF
> reads as a SIZE, and our record's word 1 landed there and was applied to
> whatever file sat in the MP3 slot. Settings now use a `nonvolatile` data slot
> and APF performs the save itself, so there is no write command to probe.
>
> Kept for the reasoning and for `tools/card_snapshot.py`, which is still the
> right way to check a card either side of a risky change.

# The 0184 settings-write measurement

The one test standing between this core and a release. Everything else is
hardware-confirmed; `SETTINGS_WRITE` is 0 because three times the settings write
extended every `.mp3` sharing the card.

**This is now a prediction, not a fishing trip.** The damaged sizes turned out to
be *word 1 of the settings record* — `{version, volume, palette, repeat}` packed
big-endian — so the test can name the number it expects before the button is
pressed:

| volume | predicted damaged size | |
| --- | --- | --- |
| 55 | `0x01370301` | 20,382,465 |
| 60 | `0x013C0301` | 20,710,145 |
| 65 | `0x01410301` | 21,037,825 &nbsp;← matches damage event 1 |
| 70 | `0x01460301` | 21,365,505 &nbsp;← matches damage event 2 |

So the question is no longer the vague "does a write damage a file". It is:

> **Does one 0184 extend a file to exactly the value in word 1 of the record?**

A file landing on precisely that number proves the mechanism. A file landing
anywhere else refutes it, and the mechanism goes back in the bin — which is the
point of predicting first.

## Read this first

**The test is destructive by design.** If 0184 is what damages files, this
damages them again. Put only expendable copies on the card. The originals are at
`C:\Users\mikek\Desktop\songs` and are known good.

Two or three small `.mp3` files are enough. Fewer, smaller files means a faster
hash and a trivial restore, and the signature does not depend on how many are
there — all four grew last time.

## Procedure

**1. Build the probe.**

```bash
bash fw/build.sh probe
```

This writes `dist/Assets/mp3player/common/mp3player.probe.rom`. It is a separate
filename on purpose: a diagnostic build must never be mistaken for a shippable
one. The flag comes from the build, not from an edit to `fw/settings.inc` — the
checked-in source always builds a core with no 0184 in it.

**2. Put the probe on the card.** Copy it over `mp3player.rom` in
`/Assets/mp3player/common/`, keeping the original somewhere to put back after.

**3. Snapshot the card.** With the card mounted over Analogue's USB mode — the
drive letter moves between D: and E:, so let the tool find it rather than
assuming:

```bash
python tools/card_snapshot.py before
```

It hashes every file whole, and prints sizes in hex. Both matter. An earlier
draft hashed only the first and last megabyte and missed a planted `SPM3` at
offset 5,505,024 — the exact place the third event found ours. And the three
damage sizes look unrelated in decimal but are `0x01410301`, `0x01460301`,
`0x01370301`: **the same low 16 bits three times.** Never read a size on this
card in decimal.

**4. Write the prediction down first.** Boot the core and note the volume it
comes up at (it is whatever `settings.bin` holds — 55 unless you change it).
Word 1 is `0x01`, volume, palette, repeat as four bytes; the table at the top of
this file has the common values. **Write the predicted size down before pressing
anything.** A prediction recorded after the fact is not a prediction.

Nudging the volume one step first is worth it: it moves the expected size off any
of the three historical values, so a match cannot be confused with old damage.

**5. Run it, once.** Do not load anything — no track, no playlist. Let it sit on
the idle screen so nothing is streaming and no refill is in flight.

Press **Select+Start**.

The screen paints the result: `APF reported SUCCESS`, `APF reported an ERROR`
with its code, or `no answer from APF`. It also shows a press count. **If that
count is above 1, the run is void** — start again from step 3, because a second
write destroys the "exactly one 0184" property the whole test rests on. The
write itself is latched and only happens once, but a stray press still means you
cannot be sure what the core did.

**6. Snapshot again and compare.** Remount the card:

```bash
python tools/card_snapshot.py after
python tools/card_snapshot.py diff
```

**Take the baseline ONCE and never re-run `before`.** Every `after` re-diffs
against the same baseline, so one baseline covers as many sessions as you like.
Re-running `before` destroys the only reference you have and the next diff
compares two snapshots minutes apart with no core use between them --
a confident all-clear that means nothing. That happened, and was caught only
because the file mtimes did not line up. `before` now refuses to overwrite an
existing baseline, and `diff` prints the window it is comparing and warns when
it is under five minutes.

## Reading the answer

**A file grew to EXACTLY the predicted size.** The mechanism is proven: 0184
treats the bridge address as a parameter struct and reads word 1 as a size. That
turns the blocker from "0184 is haunted" into a specific, addressable fault —
either stage the payload where the write path does not parse it, or stop using
0184. Do not enable `SETTINGS_WRITE` in the same session; write up the mechanism
first, then design against it.

**A file grew, but not to the predicted size.** The mechanism is refuted. Record
the actual size in hex, and decode it against the record's other words before
assuming anything — the prediction was worth making precisely because it can
fail. 0184 is still unusable either way.

**A file was rewritten at the same size, or damaged some other way.** Also fatal
for 0184, and a different mechanism again. Note exactly what changed.

In any of those three, 0184 stays off: settings lose the ability to save from the
UI, and `settings.bin` remains what it already is — a hand-edited preferences
file that `settings_load()` honours.

**Nothing changed, and APF reported success.** One isolated write is safe, so
the damage needed something else present — the debounced pump firing repeatedly,
a write concurrent with a refill, or the write landing during a track change.
That is a different and much more tractable investigation than the current one,
and it is worth re-running this test a second time before believing it.

**Nothing changed, and APF reported an error.** The write never happened, so the
run measured nothing. Note the code (`1` = slot not defined, `2` = error or out
of range) and fix that first.

## After the test, always

Put the release `mp3player.rom` back on the card. `bash fw/build.sh` rebuilds it,
and the probe build never overwrites it.
