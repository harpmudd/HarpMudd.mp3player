#!/usr/bin/env python3
"""Compare candidate BOOT METER animations, at the speed boot actually runs now.

The point is not to show each option looking its best over five seconds. Boot
finishes in a few hundred milliseconds, so the only question that matters is
what the user sees in that window -- and the honest way to answer it is to
sample each candidate at real elapsed times and lay them side by side.

Each ROW is one candidate. Each COLUMN is a moment: 0, 100, 200, 300, 450 and
700 ms after the splash appears. Read a row left to right and you are watching
that option boot.

Background: the original scrolls one column per frame from an EMPTY history, so
it needs UI_WAVE_N frames (36 at 18 fps = 2.0 s) just to populate the width. It
used to look animated because you were watching it FILL. Boot is now faster
than the fill, so it showed half a meter. Priming the history fixed the
half-drawn look and removed the motion with it -- 7 of 36 columns of scroll
reads as static. These candidates try to get both.

    python tools/boot_meter_preview.py [--scale 3]
"""
import argparse
from PIL import Image, ImageDraw

# --- straight from fw/player.c -------------------------------------------
UI_MARGIN, UI_WAVE_N, UI_WAVE_H, UI_WAVE_GAP = 20, 36, 72, 2
FB_W = 400
UI_INNER_W = FB_W - 2 * UI_MARGIN
UI_TRACK = (0x18, 0x1C, 0x18)          # 0x18E3 RGB565, roughly
ACCENT = (0x6F, 0xE0, 0xB0)

WV_FPS, WV_BODY, WV_PULL, WV_DRIFT = 18, 53, 18, 5
WV_HIT, WV_DECAY, WV_FLOOR = 7, 2, 2

SHOTS_MS = (0, 100, 200, 300, 450, 700)
CELL_H = UI_WAVE_H + 8


class Rng:
    """The firmware's xorshift, so the contour is the same shape."""
    def __init__(self, seed=0x1234567):
        self.s = seed | 1

    def __call__(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s


class Wave:
    """ui_wave_gen(): body + transient, one new column per call."""
    def __init__(self, primed, seed=0x1234567):
        self.rng = Rng(seed)
        self.level = (UI_WAVE_H * WV_BODY) // 100
        self.tr = 0
        self.h = [0] * UI_WAVE_N
        if primed:
            for _ in range(UI_WAVE_N):
                self.gen()

    def gen(self, n=1):
        for _ in range(n):
            self.h = self.h[1:] + [0]
            body = (UI_WAVE_H * WV_BODY) // 100
            lv = self.level + (self.rng() % (2 * WV_DRIFT + 1)) - WV_DRIFT \
                 - (self.level - body) // WV_PULL
            self.level = max(WV_FLOOR, min(UI_WAVE_H, lv))
            self.tr = (self.tr * WV_DECAY) // 4
            if self.rng() % WV_HIT == 0:
                head = UI_WAVE_H - self.level
                if head:
                    hit = self.rng() % (head + 1)
                    self.tr = max(self.tr, hit)
            self.h[-1] = min(UI_WAVE_H, self.level + self.tr)


def mix(a, b, num, den):
    return tuple(int(a[i] + (b[i] - a[i]) * num / den) for i in range(3))


def draw(h, reveal=UI_WAVE_N):
    """One meter frame. `reveal` is how many columns from the RIGHT are shown,
    which is the direction content actually arrives: ui_wave_gen() shifts left
    and inserts at the right edge. Masking from the left -- my first version --
    hid exactly the columns that had content and drew the empty ones."""
    im = Image.new("RGB", (UI_INNER_W, CELL_H), (7, 16, 14))
    d = ImageDraw.Draw(im)
    for i in range(UI_WAVE_N):
        x = (i * UI_INNER_W) // UI_WAVE_N
        xn = ((i + 1) * UI_INNER_W) // UI_WAVE_N
        lit = max(1, xn - x - UI_WAVE_GAP)
        if i < UI_WAVE_N - reveal:
            continue
        hh = h[i]
        c = mix(UI_TRACK, ACCENT, i + 1, UI_WAVE_N)
        if hh:
            d.rectangle([x, 4 + UI_WAVE_H - hh, x + lit - 1, 4 + UI_WAVE_H - 1],
                        fill=c)
    return im


# --- the candidates -------------------------------------------------------
def cand_current(ms):
    """WHAT SHIPS NOW: primed, one column per frame at 18 fps."""
    w = Wave(primed=True)
    w.gen(int(ms / 1000 * WV_FPS))
    return draw(w.h)

def cand_before(ms):
    """WHAT SHIPPED BEFORE: empty, one column per frame. Half a meter."""
    w = Wave(primed=False)
    w.gen(int(ms / 1000 * WV_FPS))
    return draw(w.h)          # the empty history IS the reveal

def cand_fastsweep(ms):
    """A: un-primed, but the sweep completes in ~300 ms then scrolls.
    Three columns per frame at 36 fps = 36 columns in 0.33 s."""
    fps, per = 36, 3
    w = Wave(primed=False)
    w.gen(int(ms / 1000 * fps) * per)
    return draw(w.h)          # same reveal, three times the rate

def cand_fastscroll(ms):
    """B: primed, but scrolling hard -- 3 columns per frame at 36 fps.
    Always complete, and the whole width turns over in a third of a second."""
    fps, per = 36, 3
    w = Wave(primed=True)
    w.gen(int(ms / 1000 * fps) * per)
    return draw(w.h)

def cand_grow(ms):
    """C: primed contour, revealed by a fast wipe from the LEFT over 250 ms,
    scrolling underneath. Entrance plus motion, and complete after 250 ms."""
    fps, per = 36, 2
    w = Wave(primed=True)
    w.gen(int(ms / 1000 * fps) * per)
    frac = min(1.0, ms / 250.0)
    return draw(w.h, reveal=int(UI_WAVE_N * frac))



# --- D: OSCILLOSCOPE ------------------------------------------------------
# A trace is FULL WIDTH at every instant -- there is no history to fill and
# nothing to catch half-drawn. It animates by changing shape, so it is
# complete and moving from the very first frame, which is the property the
# bars could never give at both ends.
#
# The waveform is two sines at different rates, beating against each other,
# amplitude-modulated by the SAME body+transient envelope the bars already
# use. Reusing that envelope is what keeps it looking like music rather than
# like a signal generator: it breathes, and transients punch through.
WV_SIN = [0,10,20,29,38,47,56,63,71,77,83,88,92,96,98,100,100,100,98,96,92,88,83,77,71,63,56,47,38,29,20,10,0,-10,-20,-29,-38,-47,-56,-63,-71,-77,-83,-88,-92,-96,-98,-100,-100,-100,-98,-96,-92,-88,-83,-77,-71,-63,-56,-47,-38,-29,-20,-10]

class Scope(Wave):
    def __init__(self, seed=0x1234567):
        super().__init__(primed=False, seed=seed)
        self.p1 = 0
        self.p2 = 0
        self.v = [0] * WAVE_COLS

    def step(self):
        self.gen()                       # advances level + transient
        self.p1 = (self.p1 + 5) & 63
        self.p2 = (self.p2 + 11) & 63
        env = min(100, (self.level + self.tr) * 100 // UI_WAVE_H)
        for c in range(WAVE_COLS):
            a = WV_SIN[(c * 3 + self.p1) & 63]
            b = WV_SIN[(c * 7 + self.p2) & 63]
            self.v[c] = ((a * 3 + b * 2) // 5) * env // 100


WAVE_COLS = 64
SCOPE_UNIT = 100

def draw_scope(v):
    im = Image.new("RGB", (UI_INNER_W, CELL_H), (7, 16, 14))
    d = ImageDraw.Draw(im)
    ey = UI_WAVE_H // 2 - 1
    cy = 4 + UI_WAVE_H // 2
    d.rectangle([0, cy, UI_INNER_W - 1, cy], fill=UI_TRACK)
    prev = 0
    for c in range(WAVE_COLS):
        x = (c * UI_INNER_W) // WAVE_COLS
        xn = ((c + 1) * UI_INNER_W) // WAVE_COLS
        w = max(1, xn - x)
        val = max(-ey, min(ey, v[c] * ey // SCOPE_UNIT))
        a = val if c == 0 else prev
        lo, hi = min(a, val), max(a, val)
        prev = val
        top = cy - hi
        h = (hi - lo) + 2
        col = mix(UI_TRACK, ACCENT, c + 1, WAVE_COLS)
        d.rectangle([x, top, x + w - 1, top + h - 1], fill=col)
    return im

def cand_scope(ms, fps=30):
    sc = Scope()
    for _ in range(int(ms / 1000 * fps)):
        sc.step()
    return draw_scope(sc.v)


CANDS = [
    ("BEFORE  empty fill, 18fps  (half a meter)", cand_before),
    ("NOW     primed, 18fps      (looks static)", cand_current),
    ("A       fast sweep, 3col/frame @36fps", cand_fastsweep),
    ("B       primed + fast scroll @36fps", cand_fastscroll),
    ("C       primed contour revealed over 250ms", cand_grow),
    ("D       OSCILLOSCOPE @30fps  (full width always)", cand_scope),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scale', type=int, default=3)
    ap.add_argument('--out', default='boot_meter_preview.png')
    a = ap.parse_args()

    pad, lblw = 5, 0
    W = lblw + len(SHOTS_MS) * (UI_INNER_W + pad) + pad
    H = len(CANDS) * (CELL_H + pad) + pad + 14
    sheet = Image.new("RGB", (W, H), (20, 20, 20))
    d = ImageDraw.Draw(sheet)

    for c, ms in enumerate(SHOTS_MS):
        d.text((lblw + pad + c * (UI_INNER_W + pad), 2), f"{ms} ms",
               fill=(200, 200, 200))

    for r, (name, fn) in enumerate(CANDS):
        y = 14 + pad + r * (CELL_H + pad)
        for c, ms in enumerate(SHOTS_MS):
            sheet.paste(fn(ms), (lblw + pad + c * (UI_INNER_W + pad), y))
        d.text((lblw + pad + 2, y + 2), name, fill=(255, 220, 120))

    s = a.scale
    sheet = sheet.resize((sheet.width * s, sheet.height * s), Image.NEAREST)
    sheet.save(a.out)
    print(f"{a.out}: rows = candidate, cols = {SHOTS_MS} ms into boot")


main()
