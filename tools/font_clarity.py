#!/usr/bin/env python3
"""Renders font candidates exactly as the Pocket shows them, for comparison.

The point is to judge clarity WITHOUT a hardware round-trip. Everything the
hardware does to a glyph is reproduced here, in order:

  1. FreeType rasterises Inter into a CELL_W x CELL_H cell (gen_font_rom.py).
  2. 8-bit coverage is ROUNDED to 4 bits -- not shifted; ">> 4" deleted the
     faintest edge coverage and saturated the top, which is what made type
     look harsh next to its own anti-aliasing.
  3. mp3_fb.sv maps those 16 coverage values through cov_weight(), a fitted
     gamma LUT, to a 5-bit weight. Coverage is NOT the weight: a linear blend
     over RGB565 emits ~22% light at half coverage, not 50%.
  4. The blend is integer: fg*w + bg*(16-w), per RGB565 channel.
  5. The Pocket scales 400x360 to its 1600x1440 panel at an exact 4x, NEAREST
     NEIGHBOUR. So every UI pixel becomes a hard 4x4 block. This is the part
     that matters most and the part a desktop preview normally hides: AA here
     does not soften an edge, it places a visibly grey square.

Which is why "more anti-aliasing" is not the lever. The lever is fewer, better
placed grey blocks -- hinting that snaps stems to the grid, an atlas rendered
at the size it is shown at rather than scaled, and a gamma curve chosen for a
blocky display.

    python tools/font_clarity.py            # writes docs/font_clarity.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(ROOT)
TTF = os.path.join(ROOT, 'third_party', 'font', 'Inter-SemiBold.ttf')
OUT = os.path.join(ROOT, 'docs', 'font_clarity.png')

CELL_W, CELL_H = 16, 16
BASELINE = 12
SCALE = 4                       # the Pocket's integer upscale

# mp3_fb.sv cov_weight(): 4-bit coverage -> 5-bit blend weight.
COV_WEIGHT = [0, 4, 6, 7, 8, 10, 10, 11, 12, 13, 13, 14, 15, 15, 16, 16]

# A steeper alternative: less time spent in the mid greys, which on a 4x
# blocky display is where the fuzz lives. Same endpoints.
COV_STEEP = [0, 2, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 15, 16, 16, 16]

UI_BG = 0x0862
UI_WHITE = 0xFFFF
SAMPLE = 'Sultans of Swing'


def rgb565_to_rgb(v):
    return (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)


def blend565(fg, bg, w):
    """Exactly mp3_fb.sv: per-channel fg*w + bg*(16-w), in RGB565 units."""
    inv = 16 - w
    r = (((fg >> 11) & 31) * w + ((bg >> 11) & 31) * inv) >> 4
    g = (((fg >> 5) & 63) * w + ((bg >> 5) & 63) * inv) >> 4
    b = ((fg & 31) * w + (bg & 31) * inv) >> 4
    return (r << 11) | (g << 5) | b


def render_text(px_size, opsz, wght, text, hint, cell_h=CELL_H, baseline=BASELINE):
    """Rasterise one line into an 8-bit coverage bitmap, left to right."""
    font = ImageFont.truetype(TTF, px_size, layout_engine=ImageFont.Layout.BASIC)
    try:
        font.set_variation_by_axes([float(opsz), float(wght)])
    except Exception:
        pass
    w = int(px_size * len(text) * 0.75) + 16
    img = Image.new('L', (w, cell_h), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = '1' if hint else 'L'      # '1' = no AA; 'L' = greyscale AA
    d.text((1, baseline), text, font=font, fill=255, anchor='ls')
    return img


def to_panel(cov_img, lut, fg=UI_WHITE, bg=UI_BG, scale=SCALE):
    """4-bit quantise -> gamma LUT -> RGB565 blend -> 4x nearest neighbour."""
    w, h = cov_img.size
    out = Image.new('RGB', (w, h))
    src = cov_img.load()
    dst = out.load()
    for y in range(h):
        for x in range(w):
            cov = (src[x, y] * 15 + 127) // 255       # the ROUNDED quantiser
            dst[x, y] = rgb565_to_rgb(blend565(fg, bg, lut[cov]))
    return out.resize((w * scale, h * scale), Image.NEAREST)


def scaled_from_atlas(px_size, opsz, wght, text, factor):
    """What the engine does: Bresenham fractional scaling of the 16px cell.

    NEAREST, not bilinear -- mp3_fb.sv advances the source position num/den per
    output pixel and takes whatever pixel it lands on. At 1.5x that duplicates
    some source columns and not others, so stems come out uneven. Modelling it
    as bilinear would show a soft blur, which is the wrong artefact entirely.
    """
    small = render_text(px_size, opsz, wght, text, hint=False)
    w, h = small.size
    return small.resize((int(w * factor), int(h * factor)), Image.NEAREST)


def main():
    if not os.path.exists(TTF):
        cands = []
        for dp, _, fs in os.walk(os.path.join(ROOT, 'third_party')):
            for f in fs:
                if f.lower().endswith(('.ttf', '.otf')):
                    cands.append(os.path.join(dp, f))
        print('Inter not at the expected path. Found:')
        for c in cands:
            print('   ', c)
        return 1

    rows = []

    # --- body text, 1x: the four generator-side candidates -----------------
    rows.append(('A  baseline: 15px opsz14 wght600, AA, current LUT',
                 to_panel(render_text(15, 14, 600, SAMPLE, hint=False), COV_WEIGHT)))
    rows.append(('B  16px instead of 15 -- fills the 16-row cell',
                 to_panel(render_text(16, 14, 600, SAMPLE, hint=False), COV_WEIGHT)))
    rows.append(('C  14px -- more white space between stems',
                 to_panel(render_text(14, 14, 600, SAMPLE, hint=False), COV_WEIGHT)))
    rows.append(('D  steeper gamma LUT (less mid-grey)',
                 to_panel(render_text(15, 14, 600, SAMPLE, hint=False), COV_STEEP)))
    rows.append(('E  no AA at all (hinted, 1-bit) -- the crispness bound',
                 to_panel(render_text(15, 14, 600, SAMPLE, hint=True), COV_WEIGHT)))

    # --- the title line: scaled atlas vs an atlas rendered at that size ----
    rows.append(('F  TITLE as today: 15px raster scaled 1.5x',
                 to_panel(scaled_from_atlas(15, 14, 600, SAMPLE, 1.5), COV_WEIGHT)))
    rows.append(('G  TITLE rendered DIRECTLY at 23px (needs a bigger source cell)',
                 to_panel(render_text(23, 14, 600, SAMPLE, hint=False,
                                      cell_h=24, baseline=18), COV_WEIGHT)))
    # True panel size: a TS_2X glyph is 32 UI px tall, so 32*4 = 128 panel px,
    # against F's 24*4 = 96. Showing it at scale=2 made it look denser than it
    # is and the comparison was not like for like.
    rows.append(('H  TITLE at integer 2x from the SAME 16px atlas -- no resampling,'
                 ' but 33% taller than F',
                 to_panel(scaled_from_atlas(15, 14, 600, SAMPLE, 2.0), COV_WEIGHT)))

    pad, label_h = 10, 16
    width = max(im.width for _, im in rows) + pad * 2
    height = sum(im.height + label_h + pad for _, im in rows) + pad
    sheet = Image.new('RGB', (width, height), rgb565_to_rgb(UI_BG))
    d = ImageDraw.Draw(sheet)
    y = pad
    for label, im in rows:
        d.text((pad, y), label, fill=(150, 170, 200))
        y += label_h
        sheet.paste(im, (pad, y))
        y += im.height + pad

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT)
    print('wrote %s  (%d x %d)' % (OUT, sheet.width, sheet.height))
    print('')
    print('Every row is shown at the Pocket 4x nearest-neighbour scale, so what')
    print('you see is what the panel shows -- grey BLOCKS, not soft edges.')
    print('F vs G is the one to look at first: same text, same size on screen,')
    print('one scaled from the 15px atlas and one rendered at its real size.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
