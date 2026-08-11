# QA test plan — branch `speed-1.2x`

Firmware **126,844 bytes**. Every debug switch off, so no red rows should
appear anywhere.

Ordered by risk, not by feature. The first three sections cover everything that
changed on this branch; sections 4–8 are regression checks on things that broke
at least once while building it, which is why they earn a place here at all.

**Filing a failure:** note *which file* was playing and *what you pressed*.
Both have decided a diagnosis on this project more than once. §10 says how to
get real numbers if a symptom is vague.

---

## 0. Setup

The card needs a `playlist.m3u` with several entries, and at least one `.mp3`
that is **not** in the playlist — the standalone path behaves differently on
purpose.

```
python tools/xing_check.py D:/Assets/mp3player/common
```

Run it once and keep the output. It reports which files have a Xing/Info frame
count and which are CBR. **Files with no header are the interesting ones** —
they exercise a different rate path, and they were the only three that ever
failed at seeking. A test set with none of them proves less than it looks.

On the current card those are Stockholm Syndrome (256), Stone Temple Pilots
(128) and Widespread Panic (160).

---

## 1. Resume playback — the headline feature

Resume covers **playlist playback only**. That is deliberate; a standalone file
records nothing.

| # | Steps | Expected |
|---|---|---|
| 1.1 | Enable **Resume playback** in Core Settings. Play a playlist track ~60 s in. Quit to the menu. Relaunch. | Boot row reads **RESUMING TRACK**. The same track resumes near 60 s. |
| 1.2 | As 1.1, but with **Shuffle on**. | The same *track* returns, not merely the same position in the order. |
| 1.3 | As 1.1, but quit after ~2 s. | Starts from the beginning. Under 2 s is deliberately ignored. |
| 1.4 | Turn **Resume playback** off. Play 60 s in, quit, relaunch. | Starts at the first track from 0:00. Boot row reads **LOADING TRACK**. |
| 1.5 | Resume a track, then let it play to the end. | Advances normally. Resume does not re-fire. |
| 1.6 | Fresh install (delete `/Settings/HarpMudd.Mp3Player/`). | Resume is **off** by default; other settings return to defaults. |

**1.7 — audiobook shape.** A long file as a one-line `.m3u`. Play 10+ minutes
in, quit, relaunch. Should return to the right place. This is the use the
feature exists for and the only test that exercises a large seconds value.

---

## 2. Load MP3 and the standalone path

| # | Steps | Expected |
|---|---|---|
| 2.1 | With a playlist present, **Load MP3** and pick a file **not** in it. | *That file* plays. Not a playlist track. |
| 2.2 | Play the standalone file a minute, quit, relaunch. | Nothing was recorded — your **playlist** position is intact and resumes. |
| 2.3 | Launch, then **Load MP3 within ~10 s**, before resume settles. | The picked file plays **from the start**, not dropped into mid-track. |
| 2.4 | **Load Playlist** and pick a different `.m3u` while a track plays. | The new list loads and starts playing. |

2.1 and 2.3 are both fixes from this branch and fail in different ways, so run
them separately.

---

## 3. Variable speed (1.2×)

Off by default and not remembered; it resets every launch.

| # | Steps | Expected |
|---|---|---|
| 3.1 | **Hold A** during playback. | Toast **SPEED 1.2x**. Audio speeds up, pitch rises. |
| 3.2 | **Hold A** again. | Toast **SPEED NORMAL**. Back to normal. |
| 3.3 | **Tap A**. | Plays / pauses as always. Must not change speed. |
| 3.4 | Quit and relaunch while at 1.2×. | Back to normal speed. |
| 3.5 | At 1.2×, hold **Left/Right** to seek. | Position tracks correctly. **Test the headerless files from §0 specifically.** |
| 3.6 | Same seek test at **1.0×** on those files. | Also correct. This path was latently broken before, not just at speed. |

**Known limitation — do not file:** occasional distortion when engaging 1.2×,
more likely in dense passages. 1.2× needs up to 54.8 MHz of the 60 available,
so the decoder can miss. It is the budget, not a bug.

---

## 4. Seeking and transport

| # | Steps | Expected |
|---|---|---|
| 4.1 | Hold Left/Right for 10+ s. | Accelerates. Clock stays sane; never walks backwards. |
| 4.2 | **Select + Left/Right**. | Moves exactly one second. |
| 4.3 | Seek while **paused**, and while **stopped**. | Both allowed; position updates. |
| 4.4 | Seek to the very end. | Advances or stops per repeat mode. No hang. |
| 4.5 | **B** mid-track. | Restarts instantly, keeps playing. |
| 4.6 | **Start**. | Stops and returns to 0:00. Pressing again does nothing. |

Run 4.1 on a headerless file too — it is the exact shape that failed before.

---

## 5. Settings persistence

The most-broken area of this branch. Needs **three or four relaunches**, not
one: the worst bug here reset values on *every* launch and a single test would
have caught it, but a subtler one would not.

| # | Steps | Expected |
|---|---|---|
| 5.1 | Set volume, accent, meter and EQ to distinctive non-defaults. Relaunch 3–4 times. | All four hold every time. |
| 5.2 | Change accent with **L/R**, then open Core Settings. | The menu agrees with the buttons. |
| 5.3 | Change a value in Core Settings, then quit without playing. | It sticks. |
| 5.4 | Toggle **Resume playback** in Core Settings. | Takes effect; survives relaunch. |

**5.5 — the update path.** With settings saved, copy a *new* build over the
card and relaunch. Volume, colour, repeat, shuffle, meter and EQ must survive.
Album art and screen blank are expected to reset — they no longer persist.

---

## 6. Screen blanking

Now button-only and not remembered.

| # | Steps | Expected |
|---|---|---|
| 6.1 | **Select + Down** repeatedly. | Cycles off → 1 → 5 → 10 → 30 min, with a toast each time. |
| 6.2 | Set 1 min, leave it alone. | Screen blacks out after a minute. Audio keeps playing. |
| 6.3 | While blank, let a track change. | Screen stays dark. |
| 6.4 | Press any button while blank. | Wakes, and that press does nothing else — it must not pause or skip. |
| 6.5 | **Pause**, then wait out the timeout. | Blanks while paused too. |
| 6.6 | Relaunch. | Back to off. Expected. |

---

## 7. Controls regression

Every one of these has been broken at some point.

| # | Steps | Expected |
|---|---|---|
| 7.1 | **Up/Down**. | Volume in 5% steps. |
| 7.2 | **Select + Up**. | Nothing. Must **not** change volume or toggle the art panel. |
| 7.3 | **Select** alone. | Toggles the art panel. Not remembered across launches. |
| 7.4 | **X** and **Y**. | Cycle meter and EQ, each with a named toast. |
| 7.5 | **Select + L** / **Select + R**. | Repeat and shuffle. |
| 7.6 | Analogue **Controls** screen. | Every button is named. |

---

## 8. Playlists

| # | Steps | Expected |
|---|---|---|
| 8.1 | Tap Left/Right through the whole list. | Wraps both ways. Track number matches the entry. |
| 8.2 | Turn shuffle on and skip through. | **The track number jumps around** — it names the entry, not the play order. |
| 8.3 | Shuffle a full pass. | Every track once before repeating; each pass reshuffled. |
| 8.4 | Add a misspelled filename to the `.m3u`. | It is skipped, the count reports what will actually play. |
| 8.5 | Repeat off / all / one. | Stops at the end / loops / repeats the track. |

---

## 9. Boot and first run

| # | Steps | Expected |
|---|---|---|
| 9.1 | Rename `playlist.m3u` away and launch with an empty slot. | Getting-started screen with the three numbered steps. |
| 9.2 | Launch with a playlist present. | **LOADING PLAYLIST** with animated dots, then a track starts playing. |
| 9.3 | Load MP3 from the getting-started screen. | Indicator appears **immediately** on picking, not after a pause. Plays first time — not needing two attempts. |
| 9.4 | Pocket core list. | Shows as **MP3 Player**. |

---

## 10. If something fails

**Read the card first.** `/Settings/HarpMudd.Mp3Player/Interact/_core/interact_persist.json`
is plain JSON and authoritative. It settled four failures on this project
before any build, including contradicting a theory of mine about settings being
reset. Check it before anything else.

**Then instrument.** Set `UI_SHOW_SPEED_DIAG` to `1` in `fw/player.c` and
rebuild. Two red rows appear:

```
RES S104 D1 O1 A1 V0 P1 L1 C3
1.0x T241 M16003 R16003 K2048
```

`D` is the resume branch code (documented at `resume_dbg`), `V` counts saves,
`L` is what `settings_load()` returned, `T` is `track_secs` — non-zero means
the exact rate path.

**Do not fix by reasoning alone.** Four wrong fixes went out that way on
2026-08-10. The rounds that worked all started from a number.
