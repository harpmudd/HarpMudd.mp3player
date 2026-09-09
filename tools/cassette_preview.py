#!/usr/bin/env python3
"""Draws the cassette meter on the desk, before any firmware exists.

The magic eye took about eight hardware rounds. This is the response to that.

ANATOMY, from the reference photos -- the first attempt got this wrong and it
is why the first attempt looked like a dark slab with a green bar:

  * A cassette is mostly BLACK SHELL plus a LARGE LIGHT LABEL. The label is the
    dominant element, not the window.
  * Colour lives in the label's STRIPES, not in the shell. That is where the
    accent goes, and it is what flashes to the beat.
  * The window is a NARROW HORIZONTAL BEZEL cut into the label, not a wide dark
    band. It holds two small toothed hubs with the tape between them.
  * Front-on you do NOT see two big concentric tape discs. You see the hubs and
    a dark mass of tape between them.
  * There is a raised panel across the bottom with the drive holes in it.

    python tools/cassette_preview.py [--scale 4] [--widths 130,170]
"""
import argparse, math
from PIL import Image, ImageDraw

W, H = 360, 96      # UI_INNER_W x (UI_WAVE_H + UI_WAVE_TOP) -- the meter
                    # box now reaches 24 rows above UI_WAVE_Y, which is what
                    # lets a cassette be 1.56:1 instead of squashed.
ACCENT = (0x6F, 0xE0, 0xB0)


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def grad_at(y):
    t = y / (H - 1)
    v = (16.1 + (9.9 - 16.1) * t) / 31.0
    return mix((0, 0, 0), mix((10, 26, 22), ACCENT, 0.10), v * 1.9)


# Lightened -- the first palette sat too dark against a dark background.
C_SHELL = (0x2E, 0x32, 0x38)
C_EDGE  = (0x6A, 0x72, 0x7C)
C_LABEL = (0xF2, 0xEE, 0xE4)
C_BEZEL = (0x17, 0x1A, 0x1D)
C_HUB   = (0xCE, 0xD3, 0xD8)
C_SLOT  = (0x2A, 0x2E, 0x33)
C_TAPE  = (0x4E, 0x3E, 0x33)
C_PANEL = (0x3A, 0x3E, 0x44)
C_SCREW = (0x56, 0x5C, 0x64)


def hub(d, cx, cy, r, phase):
    """Light hub with six slots cut into its rim -- the reference look."""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=C_HUB)
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            dx, dy = x - cx, y - cy
            rr = math.hypot(dx, dy)
            if rr > r:
                continue
            if rr <= r * 0.34:
                d.point((x, y), fill=C_SLOT)
                continue
            if rr < r * 0.56:
                continue
            a = (math.atan2(dy, dx) / (2 * math.pi) + phase) % 1.0
            if (a * 6) % 1.0 < 0.38:
                d.point((x, y), fill=C_SLOT)


def frame(shell_w, progress, phase, flash):
    """96 tall, matching TAPE_SHELL_H in fw/player.c. Offsets are the same
    numbers the firmware uses -- keep them in step."""
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        d.line([(0, y), (W - 1, y)], fill=grad_at(y))

    sx = (W - shell_w) // 2
    y0 = 0
    cx0 = sx + shell_w // 2
    bw = int(shell_w * 0.62)
    hdx = int(bw * 0.28)
    hcy = y0 + 48
    lx0, lx1 = sx + 5, sx + shell_w - 6

    d.rounded_rectangle([sx, y0, sx + shell_w - 1, y0 + 95], radius=7,
                        fill=C_SHELL, outline=C_EDGE)
    d.rounded_rectangle([lx0, y0 + 6, lx1, y0 + 55], radius=3, fill=C_LABEL)

    # static accent stripes; the TAPE is what reacts now
    for i, sh in enumerate((0.5, 0.75, 0.5)):
        yy = y0 + 40 + i * 6
        d.rectangle([lx0, yy, lx1, yy + 3], fill=mix(C_LABEL, ACCENT, sh))

    d.rounded_rectangle([cx0 - bw // 2, y0 + 30, cx0 + bw // 2, y0 + 65],
                        radius=9, fill=C_BEZEL)

    tw = max(2, (hdx - 5) * 2)
    d.rectangle([cx0 - tw // 2, y0 + 39, cx0 + tw // 2, y0 + 55],
                fill=mix(C_TAPE, ACCENT, flash * 0.5))
    pr = int(progress * 255)
    g = 5 + 14 * (255 - pr if pr > 128 else pr) // 128
    d.rectangle([cx0 - g // 2, y0 + 39, cx0 + g // 2, y0 + 55], fill=(0x26, 0x20, 0x1B))

    # pack rings: supply (LEFT) pays out, take-up (RIGHT) winds on
    pr = int(progress * 255)
    packs = 1 + (7 * (255 - pr)) // 255
    packt = 1 + (7 * pr) // 255
    for cx, pk in ((cx0 - hdx, packs), (cx0 + hdx, packt)):
        d.ellipse([cx - 9 - pk, hcy - 9 - pk, cx + 9 + pk, hcy + 9 + pk], fill=C_TAPE)
    hub(d, cx0 - hdx, hcy, 9, phase)
    hub(d, cx0 + hdx, hcy, 9, phase)

    d.rounded_rectangle([sx + 16, y0 + 72, sx + shell_w - 17, y0 + 91],
                        radius=3, fill=C_PANEL)
    for ox, w_ in ((-38, 6), (-15, 7), (15, 7), (38, 6)):
        x = cx0 + ox * shell_w // 150
        d.rectangle([x, y0 + 77, x + w_, y0 + 84], fill=C_BEZEL)

    for px, py in ((sx + 4, y0 + 5), (sx + shell_w - 9, y0 + 5),
                   (sx + 4, y0 + 86), (sx + shell_w - 9, y0 + 86)):
        d.rectangle([px, py, px + 4, py + 4], fill=C_SCREW)
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--widths', default='120,150,180')
    ap.add_argument('--out', default='cassette_preview.png')
    a = ap.parse_args()
    widths = [int(x) for x in a.widths.split(',')]

    cols = [(0.02, 0.00, 0.0), (0.50, 0.06, 0.6), (0.98, 0.12, 0.3)]
    pad = 6
    sheet = Image.new("RGB", (len(cols) * (W + pad) + pad,
                              len(widths) * (H + pad) + pad), (18, 18, 18))
    for r, sw in enumerate(widths):
        for c, (prog, ph, fl) in enumerate(cols):
            sheet.paste(frame(sw, prog, ph, fl),
                        (pad + c * (W + pad), pad + r * (H + pad)))
    s = a.scale
    sheet = sheet.resize((sheet.width * s, sheet.height * s), Image.NEAREST)
    sheet.save(a.out)
    print(f"{a.out}: rows = shell width {widths}, cols = progress/phase/flash")


main()
