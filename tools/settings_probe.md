# The 0184 settings-write measurement

The one test standing between this core and a release. Everything else is
hardware-confirmed; `SETTINGS_WRITE` is 0 because three times the settings write
extended every `.mp3` sharing the card, and two mitigations reasoned from that
signature both failed. This is not a third mitigation. It answers one question
and nothing else:

> **Does a single 0184, with nothing else running, damage a file?**

If yes, 0184 is unusable by this core and settings must never write the card.
If no, then 0184 alone is not the fault and the search moves to what else ran
during the three events. Either answer closes the blocker's current form.

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

**4. Run it, once.** Unmount, boot the core, and — this is the part that makes
the measurement clean — **do not load anything**. No track, no playlist. Let it
sit on the idle screen so nothing is streaming and no refill is in flight.

Press **Select+Start**.

The screen paints the result: `APF reported SUCCESS`, `APF reported an ERROR`
with its code, or `no answer from APF`. It also shows a press count. **If that
count is above 1, the run is void** — start again from step 3, because a second
write destroys the "exactly one 0184" property the whole test rests on. The
write itself is latched and only happens once, but a stray press still means you
cannot be sure what the core did.

**5. Snapshot again and compare.** Remount the card:

```bash
python tools/card_snapshot.py after
python tools/card_snapshot.py diff
```

## Reading the answer

**Any `.mp3` grew, or was rewritten.** 0184 is unusable by this core. Settings
lose the ability to save from the UI; `settings.bin` stays what it already is, a
hand-edited preferences file that `settings_load()` honours. Update
`fw/settings.inc` and the roadmap to say so, and the blocker is closed as
*answered*, not as fixed.

Record the new sizes **in hex**. If the low 16 bits are `0x0301` a fourth time,
that constant is the strongest lead anyone has had on this: a length ending in a
fixed half is a 32-bit word assembled from two 16-bit halves, not anything
computed from a 32-byte record, and it is a far narrower thing to chase than
"the offset is wrong".

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
