#!/usr/bin/env python3
"""Model fw/art.inc's box-downscale exactly, and check the three things that
could be quietly wrong on hardware rather than loudly wrong in a build.

1. EMIT ORDER AND COMPLETENESS -- every destination row emitted exactly once,
   in order, and only after its last source row has arrived. A destination row
   can straddle two MCU rows; emitting it early would ship a row averaged from
   half its input, which looks like a faint horizontal seam rather than like a
   bug.
2. ACCUMULATOR SLOT REUSE -- slots are recycled dy % ART_ACC_ROWS. If row
   dy+ART_ACC_ROWS starts accumulating before row dy is flushed and cleared,
   two rows silently add into each other.
3. OVERFLOW -- cells are uint16 and sum rowcnt*colcnt pixels of max 255.

Run against the real cover sizes measured off the card, not invented ones.
"""
import sys

ART_IMG = 92
ART_ACC_ROWS = 20
ART_FULL_MAX = 1024
UINT16_MAX = 65535


def last_sy(ay, sh, ah):
    return ((ay + 1) * sh + ah - 1) // ah - 1


def plan(iw, ih):
    """Mirror art_decode's mode choice and geometry."""
    reduce = ((iw + 7) // 8) >= ART_IMG and ((ih + 7) // 8) >= ART_IMG
    if not reduce and (iw > ART_FULL_MAX or ih > ART_FULL_MAX):
        reduce = True
    # MCU geometry: assume the common 4:2:0 case, the largest MCU (16x16), which
    # is the worst case for both straddling and slot reuse.
    mcu_w = 2 if reduce else 16
    mcu_h = 2 if reduce else 16
    rw = (iw + 7) // 8 if reduce else iw
    rh = (ih + 7) // 8 if reduce else ih
    mcus_row = (rw + mcu_w - 1) // mcu_w
    mcus_col = (rh + mcu_h - 1) // mcu_h
    sw = min(mcus_row * mcu_w, rw)
    sh = min(mcus_col * mcu_h, rh)
    return reduce, mcu_w, mcu_h, mcus_row, mcus_col, sw, sh


def check(iw, ih):
    reduce, mcu_w, mcu_h, mcus_row, mcus_col, sw, sh = plan(iw, ih)
    if not sw or not sh:
        return "skip", "degenerate"

    # The accumulator is capped at the source size on each axis; magnification
    # happens on emit. This is what bounds slot pressure.
    aw = min(ART_IMG, sw)
    ah = min(ART_IMG, sh)

    rowcnt = [0] * ah
    colcnt = [0] * aw
    for sy in range(sh):
        rowcnt[sy * ah // sh] += 1
    for sx in range(sw):
        colcnt[sx * aw // sw] += 1
    if min(rowcnt) == 0 or min(colcnt) == 0:
        return "FAIL", "empty accumulator cell -- divide by zero in art_flush_row"

    worst = max(rowcnt) * max(colcnt) * 255
    if worst > UINT16_MAX:
        return "FAIL", "accumulator overflows: %d > %d" % (worst, UINT16_MAX)

    # Walk the decode in source order, tracking which ay owns each slot.
    slot_owner = {}
    emitted = []
    next_ay = 0
    for my in range(mcus_col):
        for sy in range(my * mcu_h, min((my + 1) * mcu_h, sh)):
            ay = sy * ah // sh
            slot = ay % ART_ACC_ROWS
            if slot in slot_owner and slot_owner[slot] != ay:
                return "FAIL", ("slot %d reused by ay=%d while ay=%d still held "
                                "it (sy=%d)" % (slot, ay, slot_owner[slot], sy))
            slot_owner[slot] = ay
        sy_done = min((my + 1) * mcu_h, sh)
        while next_ay < ah and last_sy(next_ay, sh, ah) < sy_done:
            # Emitting means complete: no later source row may still map here.
            if any(s * ah // sh == next_ay for s in range(sy_done, sh)):
                return "FAIL", "ay=%d emitted early (more source rows follow)" % next_ay
            d0 = (next_ay * ART_IMG + ah - 1) // ah
            d1 = min(((next_ay + 1) * ART_IMG + ah - 1) // ah, ART_IMG)
            emitted += list(range(d0, d1))
            slot_owner.pop(next_ay % ART_ACC_ROWS, None)
            next_ay += 1
    while next_ay < ah:
        d0 = (next_ay * ART_IMG + ah - 1) // ah
        d1 = min(((next_ay + 1) * ART_IMG + ah - 1) // ah, ART_IMG)
        emitted += list(range(d0, d1))
        next_ay += 1

    if emitted != list(range(ART_IMG)):
        return "FAIL", ("destination rows emitted %s, expected 0..%d exactly once"
                        % (emitted[:8], ART_IMG - 1))

    mode = "reduce" if reduce else "FULL"
    ratio = sw / ART_IMG
    return "ok", ("%-6s src %dx%d -> acc %dx%d -> %d  (%.2fx %s)  max cell %d"
                  % (mode, sw, sh, aw, ah, ART_IMG, ratio,
                     "down" if ratio >= 1 else "UP", worst))


CASES = [
    # Measured off the user's own library.
    (300, 297), (455, 455), (1280, 1280),
    # Common cover sizes in the wild.
    (200, 200), (250, 250), (350, 350), (500, 500), (600, 600),
    (640, 640), (735, 735), (736, 736), (800, 800), (1000, 1000),
    (1024, 1024), (1400, 1400), (1500, 1500), (2000, 2000), (3000, 3000),
    # Degenerate and adversarial.
    (92, 92), (91, 91), (64, 64), (8, 8), (16, 9),
    (1024, 100), (100, 1024), (2000, 100), (4000, 90),
]

bad = 0
for iw, ih in CASES:
    status, msg = check(iw, ih)
    if status == "FAIL":
        bad += 1
    print("%-6s %5dx%-5d %s" % (status, iw, ih, msg))
print()
print("FAILURES: %d" % bad)
sys.exit(1 if bad else 0)
