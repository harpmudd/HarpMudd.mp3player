#!/usr/bin/env python3
"""Title at 24px vs what ships today, through the REAL glyph pipeline.

Not a mock. This builds a per-glyph atlas exactly as tools/gen_font_rom.py and
tools/gen_font_ext.py build theirs -- left-aligned in the cell, a per-glyph
advance from the ink width, the accent SQUASH for glyphs that overflow above
the baseline -- and then composes a string from that atlas the way the draw
engine does. Rendering a string straight out of PIL skips the squash and shows
clipping the core does not actually produce.

Measured extents that set the geometry below (Inter-SemiBold, >8 ink):

                    above base   below   overflows
    15px (today)        11          3     | Aring aring
    24px                18          5     | Aring aring

Letters and digits are clean at BOTH sizes. The overflow set is identical and
the squash ratio is identical (75%), so 24px costs nothing in fit that 15px is
not already paying -- it just has 18 rows above the baseline to say it in
instead of 12.

    python tools/title_fit.py        # writes docs/title_fit.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = os.path.join(ROOT, 'third_party', 'font', 'Inter-SemiBold.ttf')
OUT = os.path.join(ROOT, 'docs', 'title_fit.png')

SCALE = 4
UI_BG, UI_FG = 0x0862, 0xFFFF
COV = [0, 2, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 15, 16, 16, 16]

SAMPLES = [
    'Sultans of Swing',
    'Björk - Jóga',
    'Typography (jaggy)',
]


def rgb(v):
    return (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)


def blend(fg, bg, w):
    i = 16 - w
    return ((((fg >> 11 & 31) * w + (bg >> 11 & 31) * i) >> 4) << 11) | \
           ((((fg >> 5 & 63) * w + (bg >> 5 & 63) * i) >> 4) << 5) | \
           (((fg & 31) * w + (bg & 31) * i) >> 4)


def glyph(font, ch, cw, chh, base):
    """One atlas cell: squashed above the baseline if it overflows.

    Same three-way choice gen_font_ext.py documents -- shifting the glyph down
    moves the baseline visibly, squeezing only the accent makes it vanish, so
    the ink above the baseline is compressed as a whole.
    """
    OFF, H = 12, chh + 24
    tall = Image.new('L', (cw, H), 0)
    ImageDraw.Draw(tall).text((0, base + OFF), ch, font=font, fill=255,
                              anchor='ls')
    tp = tall.load()
    rows = [y for y in range(H) if any(tp[x, y] > 8 for x in range(cw))]
    if not rows:
        return None, max(3, int(font.getlength(ch)))
    top = min(rows)
    if top < OFF:
        last = OFF + base - 1
        above = tall.crop((0, top, cw, last + 1)).resize((cw, base),
                                                         Image.LANCZOS)
        cell = Image.new('L', (cw, chh), 0)
        cell.paste(above, (0, 0))
        cell.paste(tall.crop((0, last + 1, cw, OFF + chh)), (0, base))
    else:
        cell = tall.crop((0, OFF, cw, OFF + chh))
    cp = cell.load()
    right = 0
    for x in range(cw):
        if any(cp[x, y] > 8 for y in range(chh)):
            right = x + 1
    return cell, min(right + 2 if right else max(3, int(font.getlength(ch))), cw)


def compose(px, text, cw, chh, base):
    """Atlas cells laid out by advance, as the engine paints them."""
    font = ImageFont.truetype(TTF, px, layout_engine=ImageFont.Layout.BASIC)
    cells = [glyph(font, ch, cw, chh, base) for ch in text]
    total = sum(a for _, a in cells) + 4
    img = Image.new('L', (total, chh), 0)
    x = 2
    for cell, adv in cells:
        if cell is not None:
            img.paste(cell, (x, 0))
        x += adv
    return img


def panel(cov_img):
    w, h = cov_img.size
    out = Image.new('RGB', (w, h))
    s, d = cov_img.load(), out.load()
    for y in range(h):
        for x in range(w):
            d[x, y] = rgb(blend(UI_FG, UI_BG, COV[(s[x, y] * 15 + 127) // 255]))
    return out.resize((w * SCALE, h * SCALE), Image.NEAREST)


def main():
    rows = []
    for text in SAMPLES:
        # TODAY: 15px atlas in a 16 cell, then the engine's 1.5x NEAREST
        # resample -- the step that makes stems alternate thick and thin.
        now = compose(15, text, 16, 16, 12)
        now = now.resize((int(now.width * 1.5), 24), Image.NEAREST)
        rows.append(('TODAY  15px cell16, engine-scaled 1.5x    "%s"' % text,
                     panel(now)))
        rows.append(('E      24px cell24 baseline 18, direct',
                     panel(compose(24, text, 24, 24, 18))))

    pad, lab = 12, 18
    width = max(i.width for _, i in rows) + pad * 2
    height = sum(i.height + lab + pad for _, i in rows) + pad
    sheet = Image.new('RGB', (width, height), rgb(UI_BG))
    d = ImageDraw.Draw(sheet)
    y = pad
    for label, im in rows:
        d.text((pad, y), label, fill=(150, 170, 200))
        y += lab
        sheet.paste(im, (pad, y))
        y += im.height + pad
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT)
    print('wrote %s  (%dx%d)' % (OUT, sheet.width, sheet.height))
    return 0


raise SystemExit(main())
