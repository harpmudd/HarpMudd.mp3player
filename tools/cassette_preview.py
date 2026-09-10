#!/usr/bin/env python3
"""Draws the cassette meter on the desk, before any firmware exists.

The magic eye took about eight hardware rounds. This is the response to that.

KEEP THIS IN STEP WITH fw/player.c. It drifted once -- it went on rendering
progress-driven packs, a bright centre gap and light tan tape long after all
three had been removed from the firmware -- and a preview that lies is worse
than no preview, because it is trusted. Every offset and colour below is copied
from the VIZ_TAPE block; if you change one there, change it here.

ANATOMY, from the reference photos -- the first attempt got this wrong and it
is why the first attempt looked like a dark slab with a green bar:

  * A cassette is mostly BLACK SHELL plus a LARGE LIGHT LABEL. The label is the
    dominant element, not the window.
  * Colour lives in the label's STRIPES. They are only ever visible in the two
    ~25px flanks either side of the window, because the bezel covers the middle.
  * The window is a NARROW HORIZONTAL BEZEL cut into the label, not a wide dark
    band. It holds two small toothed hubs with the tape between them.
  * Front-on you do NOT see two big concentric tape discs. You see the hubs and
    a dark ribbon between them.
  * There is a raised panel across the bottom with the drive holes in it.

WHAT IS NOT HERE, deliberately:

  * Progress. The reels are a FIXED wind. The variable version never read as
    progress (the two packs' radii always summed to a constant, so the gap only
    shifted) and its unclamped ratio crashed the player -- see the ROADMAP.
  * The reel coast. The hubs spin down over ~0.4 s on pause and back up over
    ~0.2 s; a still frame cannot show it. Pass --phase to see rotation.

    python tools/cassette_preview.py [--scale 4] [--widths 130,150]
"""
import argparse, math, pathlib, re
from PIL import Image, ImageDraw

W, H = 360, 96      # UI_INNER_W x (UI_WAVE_H + UI_WAVE_TOP) -- the meter box
                    # reaches 24 rows above UI_WAVE_Y, which is what lets a
                    # cassette be 1.56:1 instead of squashed.
ACCENT = (0x6F, 0xE0, 0xB0)

HUB_R = 9           # TAPE_HUB_R
PACK  = 5           # TAPE_PACK -- fixed wind, bands at r = 14, 12, 10

# Straight from the VIZ_TAPE block's colour constants.
C_SHELL  = (0x3E, 0x44, 0x4C)
C_EDGE   = (0x58, 0x60, 0x69)
C_LABEL  = (0xF2, 0xEE, 0xE4)
C_BEZEL  = (0x17, 0x1A, 0x1D)
C_HUB    = (0xCE, 0xD3, 0xD8)
C_SLOT   = (0x2A, 0x2E, 0x33)
C_TAPE   = (0x57, 0x44, 0x33)   # wound pack, outer band
C_TAPE2  = (0x3C, 0x2F, 0x24)   # wound pack, alternating band
C_RIBBON = (0x2C, 0x25, 0x21)   # the exposed tape: one layer seen edge-on
C_PANEL  = (0x3A, 0x3E, 0x44)
C_SCREW  = (0x56, 0x5C, 0x64)


def mix(a, b, num, den):
    """ui_mix(): num/den of the way from a to b."""
    return tuple(int(a[i] + (b[i] - a[i]) * num / den) for i in range(3))


def grad_at(y):
    t = y / (H - 1)
    v = (16.1 + (9.9 - 16.1) * t) / 31.0
    return mix((0, 0, 0), mix((10, 26, 22), ACCENT, 10, 100), int(v * 190), 100)


def tape_hw(r, dy):
    """Half-width of a circle of radius r at row offset dy -- tape_hw()."""
    d = abs(dy)
    if d >= r:
        return 0
    w = r
    while w and w * w + d * d > r * r:
        w -= 1
    return w


def disc(d, cx, cy, r, c):
    """tape_disc(): filled, one rect per row."""
    for i in range(0, r + 1):
        w = tape_hw(r, i)
        d.rectangle([cx - w, cy - i, cx + w, cy - i], fill=c)
        if i:
            d.rectangle([cx - w, cy + i, cx + w, cy + i], fill=c)


def load_hub_masks(src="fw/player.c"):
    """Read tape_hub[][] out of the FIRMWARE rather than re-deriving it.

    The preview used to generate its own hub procedurally, and that is exactly
    how this tool drifted: an approximation looks close enough to trust and
    then stops matching. Parsing the real table means the hubs here cannot be
    wrong unless the firmware itself is."""
    txt = pathlib.Path(src).read_text(encoding="utf-8", errors="replace")
    i = txt.index("tape_hub[TAPE_HUB_PH][TAPE_HUB_N]")
    body = txt[txt.index("{", i) + 1: txt.index("};", i)]
    rows = [[int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", ln)]
            for ln in body.splitlines() if "0x" in ln]
    if not rows:
        raise SystemExit("could not parse tape_hub[][] from " + src)
    return rows


HUB_MASKS = load_hub_masks()
HUB_N = len(HUB_MASKS[0])            # TAPE_HUB_N -- 19, i.e. 2*HUB_R + 1


def hub(d, cx, cy, r, phase):
    """Mid-tone wheel with slots CUT INTO it, from the firmware's own mask.

    Not bright teeth radiating from a dark centre -- that reads as a snowflake
    at this size, which is what the first version looked like."""
    disc(d, cx, cy, r, C_HUB)
    m = HUB_MASKS[int(phase * len(HUB_MASKS)) % len(HUB_MASKS)]
    for iy in range(HUB_N):
        bits = m[iy]
        y = cy - r + iy
        for ix in range(HUB_N):
            if bits & (1 << ix):
                d.point((cx - r + ix, y), fill=C_SLOT)


def frame(shell_w, phase, bass, level, name="AESOP'S FABLES"):
    """96 tall, matching TAPE_SHELL_H. bass and level are 0..1."""
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        d.line([(0, y), (W - 1, y)], fill=grad_at(y))

    sx  = (W - shell_w) // 2
    y0  = 0
    cx0 = sx + shell_w // 2
    bw  = (shell_w * 62) // 100
    hdx = (bw * 28) // 100
    hcy = y0 + 48
    lx, lw = sx + 3, shell_w - 6          # 3px inset: a real label nearly
    lr = lx + lw - 1                      # spans the shell

    # ---- face ------------------------------------------------------------
    d.rounded_rectangle([sx, y0, sx + shell_w - 1, y0 + 95], radius=3,
                        fill=C_SHELL)
    # All FOUR sides. With only top and bottom the shell faded into the
    # background at the left and right.
    rim = mix(C_EDGE, ACCENT, int(level * 7), 24)      # capped at 1/3 accent
    d.rectangle([sx + 3, y0, sx + shell_w - 4, y0], fill=rim)
    d.rectangle([sx + 3, y0 + 95, sx + shell_w - 4, y0 + 95], fill=rim)
    d.rectangle([sx, y0 + 3, sx, y0 + 92], fill=rim)
    d.rectangle([sx + shell_w - 1, y0 + 3, sx + shell_w - 1, y0 + 92], fill=rim)

    d.rounded_rectangle([lx, y0 + 6, lr, y0 + 55], radius=3, fill=C_LABEL)

    # The playlist name, minus the .m3u. Nobody writes the extension on a tape.
    d.text((lx + 3, y0 + 16), name, fill=(0x6E, 0x74, 0x7C))

    # Static accent stripes, then the bezel OVER them. They used to react per
    # band and read as glitchy -- the cause was structural, not tuning: the
    # bezel was redrawn every frame across the same rows.
    for i, sh in enumerate((2, 3, 2)):
        yy = y0 + 40 + i * 6
        d.rectangle([lx, yy, lr, yy + 3], fill=mix(C_LABEL, ACCENT, sh, 4))

    d.rounded_rectangle([cx0 - bw // 2, y0 + 30, cx0 + bw // 2, y0 + 65],
                        radius=9, fill=C_BEZEL)

    # Wound tape: concentric 2px bands, so the wind is COUNTABLE. A flat disc
    # of one tone cannot show that it is wound at all.
    for cx in (cx0 - hdx, cx0 + hdx):
        for k, rr in enumerate(range(HUB_R + PACK, HUB_R, -2)):
            disc(d, cx, hcy, rr, C_TAPE2 if k & 1 else C_TAPE)

    # ---- per-frame -------------------------------------------------------
    # The exposed ribbon, contoured against both reels row by row, glowing with
    # bass. +1 on the LEFT only: a rect spans xl..xr-1, so without it the first
    # pixel lands on the left reel's outermost one and the right stays clear.
    tc = mix(C_RIBBON, ACCENT, int(bass * 7), 20)
    rl = HUB_R + PACK
    for dy in range(-(rl - 1), rl):
        xl = (cx0 - hdx) + tape_hw(rl, dy) + 1
        xr = (cx0 + hdx) - tape_hw(rl, dy)
        if xr > xl:
            d.rectangle([xl, hcy + dy, xr - 1, hcy + dy], fill=tc)

    hub(d, cx0 - hdx, hcy, HUB_R, phase)
    hub(d, cx0 + hdx, hcy, HUB_R, phase)

    # ---- panel, holes, screws --------------------------------------------
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
    ap.add_argument('--widths', default='150')
    ap.add_argument('--out', default='cassette_preview.png')
    a = ap.parse_args()
    widths = [int(x) for x in a.widths.split(',')]

    # columns: quiet / mid-beat / loud, with the hubs at three rotations
    cols = [(0.00, 0.0, 0.05), (0.33, 0.6, 0.55), (0.66, 1.0, 0.95)]
    pad = 6
    sheet = Image.new("RGB", (len(cols) * (W + pad) + pad,
                              len(widths) * (H + pad) + pad), (18, 18, 18))
    for r, sw in enumerate(widths):
        for c, (ph, bass, lvl) in enumerate(cols):
            sheet.paste(frame(sw, ph, bass, lvl),
                        (pad + c * (W + pad), pad + r * (H + pad)))
    s = a.scale
    sheet = sheet.resize((sheet.width * s, sheet.height * s), Image.NEAREST)
    sheet.save(a.out)
    print(f"{a.out}: rows = shell width {widths}, cols = phase/bass/level")


main()
