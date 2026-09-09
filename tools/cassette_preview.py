#!/usr/bin/env python3
"""Draws the cassette meter on the desk, before any firmware exists.

The magic eye took about eight hardware rounds to tune. This is the response to
that: the cassette is the most detailed meter yet, and every look decision --
proportions, hub teeth, how the reels change with progress -- is settled here at
zero cost per iteration.

Geometry mirrors what the firmware will use: the meter box is UI_INNER_W x
UI_WAVE_H = 360x72, and the shell is 240 wide, centred, leaving ~60px each side
that MUST be painted with the real per-row gradient (the magic-eye lesson).

    python tools/cassette_preview.py [--scale 4] [--out preview.png]
"""
import argparse, math
from PIL import Image, ImageDraw

W, H = 360, 72                     # UI_INNER_W x UI_WAVE_H
SHELL_W = 240
SHELL_X = (W - SHELL_W) // 2       # 60
SHELL_Y0, SHELL_Y1 = 1, 70
# Vertical budget, tight and deliberate: the bottom openings need their own
# band. Drawing them over the window's lower edge made them read as a dashed
# line across the tape rather than as holes in the shell.
HUB_DX = 58                        # hub offset from shell centre
REEL_CY = 44
HUB_R = 6
PACK_MIN, PACK_MAX = 8, 16
LABEL = (SHELL_X + 13, 4, SHELL_X + SHELL_W - 13, 25)
WIN = (SHELL_X + 13, 28, SHELL_X + SHELL_W - 13, 61)   # one wide window
OPEN_Y0, OPEN_Y1 = 63, 68                              # capstans / head

ACCENT = (0x6F, 0xE0, 0xB0)        # the mint accent from the screenshots


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def grad_at(y):
    """The per-row background ramp: 16.1/31 at the box top to 9.9/31 at bottom."""
    t = y / (H - 1)
    v = (16.1 + (9.9 - 16.1) * t) / 31.0
    return mix((0, 0, 0), mix((10, 26, 22), ACCENT, 0.10), v * 1.9)


def reel(d, cx, pack_r, phase, shade):
    pack_r = int(round(pack_r))
    """Wound tape pack, dark hub, six bright teeth on the hub rim.

    The teeth sit only on the hub's OUTER edge, which is where a real cassette
    hub has them. Filling the whole disc with alternating wedges reads as a
    snowflake, not a hub -- that was the first attempt."""
    d.ellipse([cx - pack_r, REEL_CY - pack_r, cx + pack_r, REEL_CY + pack_r],
              fill=shade['pack'])
    for rr in range(pack_r - 2, HUB_R, -3):          # wound-tape banding
        d.ellipse([cx - rr, REEL_CY - rr, cx + rr, REEL_CY + rr],
                  outline=shade['pack2'])
    d.ellipse([cx - HUB_R, REEL_CY - HUB_R, cx + HUB_R, REEL_CY + HUB_R],
              fill=shade['hubface'])
    for y in range(REEL_CY - HUB_R, REEL_CY + HUB_R + 1):
        for x in range(cx - HUB_R, cx + HUB_R + 1):
            dx, dy = x - cx, y - REEL_CY
            r = math.hypot(dx, dy)
            if r > HUB_R:
                continue
            if r <= HUB_R * 0.34:                     # centre bore
                d.point((x, y), fill=shade['hub'])
                continue
            if r < HUB_R * 0.58:
                continue
            a = (math.atan2(dy, dx) / (2 * math.pi) + phase) % 1.0
            if (a * 6) % 1.0 < 0.38:                  # six slots, cut dark
                d.point((x, y), fill=shade['hub'])


def frame(progress, phase, flash):
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)

    for y in range(H):                       # the ramp, per ROW not in strips
        d.line([(0, y), (W - 1, y)], fill=grad_at(y))

    shade = {
        # Neutral shell, accent LABEL. A grey object on the accent-tinted
        # background ramp reads as a physical thing; an accent-tinted shell
        # read as a green graphic.
        'shell':   (0x24, 0x28, 0x2C),
        'edge':    (0x5E, 0x66, 0x6E),
        'lip':     (0x14, 0x16, 0x19),
        'label':   mix(mix((0, 0, 0), ACCENT, 0.42), ACCENT, flash * 0.58),
        'win':     (0x07, 0x08, 0x09),
        'pack':    (0x3A, 0x35, 0x30),
        'pack2':   (0x25, 0x22, 0x1F),
        'hub':     (0x1A, 0x1D, 0x20),
        'hubface': (0xC6, 0xCB, 0xD0),
        'tape':    (0x6A, 0x64, 0x5C),
        'screw':   (0x3C, 0x42, 0x48),
    }

    d.rounded_rectangle([SHELL_X, SHELL_Y0, SHELL_X + SHELL_W - 1, SHELL_Y1],
                        radius=5, fill=shade['shell'], outline=shade['edge'])
    d.rounded_rectangle(list(LABEL), radius=3, fill=shade['label'])

    cx0 = SHELL_X + SHELL_W // 2

    # five screws, as a real shell has
    for sx, sy in ((SHELL_X + 6, 6), (SHELL_X + SHELL_W - 7, 6),
                   (SHELL_X + 6, SHELL_Y1 - 4), (SHELL_X + SHELL_W - 7, SHELL_Y1 - 4)):
        d.ellipse([sx - 2, sy - 2, sx + 2, sy + 2], fill=shade['screw'])
        d.point((sx, sy), fill=shade['lip'])

    d.rounded_rectangle(list(WIN), radius=4, fill=shade['win'],
                        outline=shade['edge'])

    # supply empties, take-up fills
    sup = PACK_MAX - (PACK_MAX - PACK_MIN) * progress
    take = PACK_MIN + (PACK_MAX - PACK_MIN) * progress
    # angular speed ~ 1/radius: the supply reel speeds up as it empties
    reel(d, cx0 - HUB_DX, sup,  phase * (PACK_MAX / sup),  shade)
    reel(d, cx0 + HUB_DX, take, -phase * (PACK_MAX / take), shade)
    # the exposed tape running across the front -- a strong cassette cue
    ty = WIN[3] - 4
    d.line([(cx0 - HUB_DX, ty), (cx0 + HUB_DX, ty)], fill=shade['tape'], width=2)

    # the openings along the bottom edge: capstans, pinch rollers, head
    for ox, ow in ((-48, 8), (-22, 14), (8, 14), (36, 8)):
        x0 = cx0 + ox
        d.rounded_rectangle([x0, OPEN_Y0, x0 + ow, OPEN_Y1],
                            radius=2, fill=shade['lip'], outline=shade['screw'])
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--out', default='cassette_preview.png')
    a = ap.parse_args()

    rows = [("start  0%", 0.02), ("middle 50%", 0.50), ("end   95%", 0.95)]
    phases = [0.00, 0.06, 0.12]
    flashes = [0.0, 1.0, 0.35]

    pad = 6
    sheet = Image.new("RGB", (len(phases) * (W + pad) + pad,
                              len(rows) * (H + pad) + pad), (18, 18, 18))
    for r, (_, prog) in enumerate(rows):
        for c, ph in enumerate(phases):
            sheet.paste(frame(prog, ph, flashes[c]),
                        (pad + c * (W + pad), pad + r * (H + pad)))
    s = a.scale
    sheet = sheet.resize((sheet.width * s, sheet.height * s), Image.NEAREST)
    sheet.save(a.out)
    print(f"{a.out}: {sheet.width}x{sheet.height}")
    print("rows = track progress (2% / 50% / 95%), cols = rotation phase + bass flash")


main()
