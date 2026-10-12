#!/usr/bin/env python3
"""Candidate TITLE sizes, rendered exactly as the Pocket shows them.

The defect: the title line is a 15px glyph in a 16x16 cell, scaled 1.5x by the
draw engine. 1.5x is the only scale that RESAMPLES -- the engine advances the
source position num/den per output pixel and takes whatever it lands on, so
some source columns are duplicated and others are not, and stems come out
uneven. The fix is to render the title at the size it is actually shown at.

This picks that size without writing any RTL. Every row goes through the real
pipeline: 8-bit coverage -> ROUNDED 4-bit quantise -> cov_weight() -> integer
RGB565 blend -> 4x nearest neighbour.

    python tools/title_sizes.py          # writes docs/title_sizes.png

cov_weight below is the STEEP curve that shipped in v1.5.1, read off
mp3_fb.sv. The older, shallower curve is still in tools/font_clarity.py and
using it here would judge every candidate under a gamma the core no longer
has.
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = os.path.join(ROOT, 'third_party', 'font', 'Inter-SemiBold.ttf')
OUT = os.path.join(ROOT, 'docs', 'title_sizes.png')

SCALE = 4                     # the Pocket's integer upscale to the panel
UI_BG = 0x0862
UI_FG = 0xFFFF
SAMPLE = 'Sultans of Swing'

# mp3_fb.sv cov_weight(), as shipped in v1.5.1.
COV = [0, 2, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 15, 16, 16, 16]


def rgb565_to_rgb(v):
    return (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)


def blend565(fg, bg, w):
    inv = 16 - w
    r = (((fg >> 11) & 31) * w + ((bg >> 11) & 31) * inv) >> 4
    g = (((fg >> 5) & 63) * w + ((bg >> 5) & 63) * inv) >> 4
    b = ((fg & 31) * w + (bg & 31) * inv) >> 4
    return (r << 11) | (g << 5) | b


def raster(px, text, cell_h, baseline):
    """Inter rendered into a cell_h-tall coverage bitmap."""
    font = ImageFont.truetype(TTF, px, layout_engine=ImageFont.Layout.BASIC)
    try:
        font.set_variation_by_axes([14.0, 600.0])
    except Exception:
        pass                      # Inter-SemiBold here is a STATIC face
    img = Image.new('L', (int(px * len(text) * 0.78) + 20, cell_h), 0)
    ImageDraw.Draw(img).text((1, baseline), text, font=font, fill=255,
                             anchor='ls')
    return img


def to_panel(cov_img):
    """4-bit quantise -> cov_weight -> RGB565 blend -> 4x nearest."""
    w, h = cov_img.size
    out = Image.new('RGB', (w, h))
    src, dst = cov_img.load(), out.load()
    for y in range(h):
        for x in range(w):
            c = (src[x, y] * 15 + 127) // 255      # ROUNDED, not shifted
            dst[x, y] = rgb565_to_rgb(blend565(UI_FG, UI_BG, COV[c]))
    return out.resize((w * SCALE, h * SCALE), Image.NEAREST)


def bresenham_1p5(cov_img):
    """What the engine does today: NEAREST resample of the 16px cell by 1.5.

    Not bilinear. Modelling it as a smooth filter shows a soft blur, which is
    the wrong artefact entirely -- the real one is uneven stem widths.
    """
    w, h = cov_img.size
    return cov_img.resize((int(w * 1.5), int(h * 1.5)), Image.NEAREST)


def main():
    if not os.path.exists(TTF):
        print('Inter not found at', TTF)
        return 1

    rows = [
        ('A  TODAY: 15px in a 16px cell, engine-scaled 1.5x  -> 24px',
         to_panel(bresenham_1p5(raster(15, SAMPLE, 16, 12)))),
        ('B  direct 21px in a 24px cell  (most side bearing)',
         to_panel(raster(21, SAMPLE, 24, 18))),
        ('C  direct 22px in a 24px cell',
         to_panel(raster(22, SAMPLE, 24, 18))),
        ('D  direct 23px in a 24px cell',
         to_panel(raster(23, SAMPLE, 24, 18))),
        ('E  direct 24px in a 24px cell  (fills the cell)',
         to_panel(raster(24, SAMPLE, 24, 19))),
    ]

    pad, lab = 12, 18
    width = max(im.width for _, im in rows) + pad * 2
    height = sum(im.height + lab + pad for _, im in rows) + pad
    sheet = Image.new('RGB', (width, height), rgb565_to_rgb(UI_BG))
    d = ImageDraw.Draw(sheet)
    y = pad
    for label, im in rows:
        d.text((pad, y), label, fill=(150, 170, 200))
        y += lab
        sheet.paste(im, (pad, y))
        y += im.height + pad

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT)
    print('wrote %s  (%d x %d)' % (OUT, sheet.width, sheet.height))
    print()
    print('Row A is the current defect. Compare its stem widths against B-E:')
    print('A duplicates some columns and not others, so vertical strokes')
    print('alternate thick and thin. B-E are rendered at their real size and')
    print('have no resampling at all.')
    print()
    print('All rows are at the panel 4x, so what you see is what it looks')
    print('like -- grey BLOCKS, not soft edges.')
    return 0


raise SystemExit(main())
