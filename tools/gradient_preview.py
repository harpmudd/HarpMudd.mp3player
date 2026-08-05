#!/usr/bin/env python3
"""Compare background gradient treatments at the framebuffer's real precision.

The current ramp draws 40 bands between 0x2124 and black, and those 40 bands
collapse to THIRTEEN distinct RGB565 colours -- each holding for ~28 framebuffer
rows, which the Pocket's 4x integer scale turns into ~111 panel rows. That is the
banding. Drawing more bands changes nothing, because the colour space has no
values in between: at this brightness green has 10 levels and red/blue have 5.

The fix for a ramp with too few levels is DITHER, and the same trick is what
makes an accent tint expressible at all. At luma ~35 a tinted top and a neutral
top quantise to nearly the same RGB565 triple, so without dither the tint is
invisible; with it, alternating rows between adjacent levels lands the eye on
intermediate colours that RGB565 cannot name.

Dither is VERTICAL ONLY here, deliberately. The engine draws a rect per command,
so one colour per row is 360 commands -- affordable for a background drawn on
repaints rather than per frame. Varying the pattern along x as well would mean
runs within a row, tens of thousands of commands, which this core cannot spend.

    python tools/gradient_preview.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
H, BANDS = 360, 40
GRAD_TOP, GRAD_BOT = 0x2124, 0x0000

PALETTE = [
    ("amber",   0xFC65), ("lime",    0xBF08), ("mint",    0xAF13),
    ("seafoam", 0x7E55), ("sky",     0x5D5C), ("indigo",  0x7C5A),
    ("lilac",   0xAC5E), ("blush",   0xF534), ("coral",   0xF32D),
    ("crimson", 0xB9E9), ("gold",    0xEE07), ("cream",   0xEF1A),
]

# Ordered dither thresholds over 4 rows. A void-and-cluster pattern would be
# marginally better but 4 entries is enough for a one-level step and keeps the
# firmware side to a 4-entry table.
BAYER = (0.125, 0.625, 0.375, 0.875)


def ch(c):
    return (c >> 11, (c >> 5) & 0x3F, c & 0x1F)


def pack(r, g, b):
    return (min(max(r, 0), 31) << 11) | (min(max(g, 0), 63) << 5) | min(max(b, 0), 31)


def rgb888(c):
    r, g, b = ch(c)
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def luma(c):
    r, g, b = rgb888(c)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def tinted_top(accent, target_luma, mix_to_grey=0.0):
    """The accent, scaled to the SAME luma the neutral ramp already uses, so the
    text contrast that was tuned against this background does not move. Optional
    pull toward grey for a subtler tint."""
    r, g, b = rgb888(accent)
    l = 0.2126 * r + 0.7152 * g + 0.0722 * b
    if l <= 0:
        return GRAD_TOP
    k = target_luma / l
    r, g, b = r * k, g * k, b * k
    if mix_to_grey > 0.0:
        r = r + (target_luma - r) * mix_to_grey
        g = g + (target_luma - g) * mix_to_grey
        b = b + (target_luma - b) * mix_to_grey
    return pack(int(round(r / 255 * 31)), int(round(g / 255 * 63)),
                int(round(b / 255 * 31)))


def rows_banded(top, bot, bands=BANDS):
    """What ui_gradient() does today."""
    tr, tg, tb = ch(top)
    br, bg, bb = ch(bot)
    out = []
    for y in range(H):
        i = min(y * bands // H, bands - 1)
        out.append(pack(tr + (br - tr) * i // bands,
                        tg + (bg - tg) * i // bands,
                        tb + (bb - tb) * i // bands))
    return out


def rows_dithered(top, bot):
    """One colour per row, each channel dithered between adjacent levels."""
    tr, tg, tb = ch(top)
    br, bg, bb = ch(bot)
    out = []
    for y in range(H):
        t = y / (H - 1)
        thr = BAYER[y & 3]
        px = []
        for a, b_ in ((tr, br), (tg, bg), (tb, bb)):
            ideal = a + (b_ - a) * t
            base = int(ideal)
            px.append(base + (1 if (ideal - base) > thr else 0))
        out.append(pack(*px))
    return out


def strip(rows, w):
    im = Image.new("RGB", (w, len(rows)))
    px = im.load()
    for y, c in enumerate(rows):
        col = rgb888(c)
        for x in range(w):
            px[x, y] = col
    return im


def main():
    tl = luma(GRAD_TOP)
    print("current top 0x%04X  luma %.1f" % (GRAD_TOP, tl))
    print("distinct colours -- banded: %d   dithered: %d\n"
          % (len(set(rows_banded(GRAD_TOP, GRAD_BOT))),
             len(set(rows_dithered(GRAD_TOP, GRAD_BOT)))))

    print("%-9s %-16s %-16s" % ("accent", "tint @ luma 35", "tint @ luma 55"))
    print("-" * 46)
    for name, c in PALETTE:
        a = tinted_top(c, tl)
        b = tinted_top(c, 55.0)
        print("%-9s 0x%04X r%d g%-2d b%-2d  0x%04X r%d g%-2d b%-2d %s"
              % (name, a, *ch(a), b, *ch(b),
                 "<- same as neutral" if a == GRAD_TOP else ""))

    panels = [
        ("A  now: 40 bands", rows_banded(GRAD_TOP, GRAD_BOT)),
        ("B  360 bands, no dither", rows_banded(GRAD_TOP, GRAD_BOT, H)),
        ("C  dithered, neutral", rows_dithered(GRAD_TOP, GRAD_BOT)),
        ("D  dither+seafoam L35", rows_dithered(tinted_top(0x7E55, tl), GRAD_BOT)),
        ("E  seafoam L45 half-sat",
         rows_dithered(tinted_top(0x7E55, 45.0, 0.5), GRAD_BOT)),
        ("F  seafoam L55 full", rows_dithered(tinted_top(0x7E55, 55.0), GRAD_BOT)),
        ("G  coral L45 half-sat",
         rows_dithered(tinted_top(0xF32D, 45.0, 0.5), GRAD_BOT)),
        ("H  sky   L45 half-sat",
         rows_dithered(tinted_top(0x5D5C, 45.0, 0.5), GRAD_BOT)),
    ]

    # Two sheets. The overview is 1:1 for shape; the detail is 4x VERTICAL,
    # which is what the Pocket's integer scale actually does to these rows --
    # and therefore what decides whether dither texture is acceptable. Judging
    # dither at 1:1 would flatter it dishonestly.
    for tag, zoom, y0, y1 in (("gradient_preview", 1, 0, H),
                              ("gradient_preview_4x", 4, 40, 130)):
        hh = (y1 - y0) * zoom
        w, pad, lab = 150, 10, 18
        sheet = Image.new("RGB", (len(panels) * (w + pad) + pad, hh + lab + 2 * pad),
                          (20, 20, 24))
        d = ImageDraw.Draw(sheet)
        ui = ImageFont.load_default()
        for i, (name, rows) in enumerate(panels):
            x = pad + i * (w + pad)
            d.text((x, pad), name, fill=(235, 235, 240), font=ui)
            s = strip(rows[y0:y1], w)
            if zoom != 1:
                s = s.resize((w, hh), Image.NEAREST)
            sheet.paste(s, (x, pad + lab))
        out = ROOT / "docs" / ("%s.png" % tag)
        sheet.save(out)
        print("wrote %s  (%s)" % (out, "1:1" if zoom == 1
                                  else "rows %d-%d at %dx vertical = panel scale"
                                       % (y0, y1, zoom)))


if __name__ == "__main__":
    main()
