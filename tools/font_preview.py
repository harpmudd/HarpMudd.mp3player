#!/usr/bin/env python3
"""Render text through a BIT-EXACT model of the CHAR compose path, so font and
blend changes can be judged on screen instead of on hardware.

Every stage the real pixel passes through is reproduced here in the same order
and at the same precision: glyph raster -> 4bpp coverage quantisation ->
coverage-to-weight -> the 16-step interpolation -> the >>4 -> the RGB565 pack.
What comes out is what the framebuffer would hold. The only liberty taken is the
final magnification for viewing, and even that is honest: the Pocket scales
400x360 to its 1600x1440 panel by an exact factor of 4, so 4x nearest-neighbour
is what the panel genuinely does to these pixels.

The point is to spend zero hardware cycles on a question that is entirely
decidable at the desk. Two of the three variables here (which quantiser, which
weight table) are not matters of taste at all -- they have right answers, and
the render only has to confirm the arithmetic landed where the model said it
would. The third (type size and weight) IS taste, and taste needs eyes.

    python tools/font_preview.py                  # writes docs/font_preview.png
    python tools/font_preview.py --out other.png
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
TTF = ROOT / "third_party" / "font" / "Inter-SemiBold.ttf"

CELL_W, CELL_H = 16, 16
BASELINE = 12

# The palette, straight from fw/player.c.
UI_WHITE, UI_DIM = 0xFFFF, 0x94B2
GRAD_TOP, GRAD_BOT = 0x2124, 0x0000
ACCENT = 0x7E55          # seafoam -- a mid-brightness accent, the honest case

# The fitted table now in mp3_fb.sv (tools/gen_text_gamma.py).
COV_WEIGHT = [0, 4, 6, 7, 8, 10, 10, 11, 12, 13, 13, 14, 15, 15, 16, 16]
# What the hardware did before: coverage used directly as the weight.
COV_LINEAR = [c + (1 if c == 15 else 0) for c in range(16)]


def mix565(a, b, t, n):
    ar, ag, ab = a >> 11, (a >> 5) & 0x3F, a & 0x1F
    br, bg, bb = b >> 11, (b >> 5) & 0x3F, b & 0x1F
    return (((ar + (br - ar) * t // n) << 11)
            | ((ag + (bg - ag) * t // n) << 5)
            | (ab + (bb - ab) * t // n))


def rgb888(c):
    r, g, b = c >> 11, (c >> 5) & 0x3F, c & 0x1F
    # Replicate high bits into the low ones, as any 565 panel does.
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def blend(fg, bg, w):
    """The RTL's own arithmetic, widths and truncation included."""
    fr, fgc, fb = fg >> 11, (fg >> 5) & 0x3F, fg & 0x1F
    br, bgc, bb = bg >> 11, (bg >> 5) & 0x3F, bg & 0x1F
    r = min(((fr * w + br * (16 - w)) >> 4), 31)
    g = min(((fgc * w + bgc * (16 - w)) >> 4), 63)
    b = min(((fb * w + bb * (16 - w)) >> 4), 31)
    return (r << 11) | (g << 5) | b


def load_font(size, weight, opsz):
    f = ImageFont.truetype(str(TTF), size)
    try:
        f.set_variation_by_axes([opsz, weight])
    except Exception:
        pass
    return f


def glyph_cov(font, ch, quant):
    """Raster one glyph and quantise exactly as gen_font_rom.py does."""
    img = Image.new("L", (CELL_W, CELL_H), 0)
    ImageDraw.Draw(img).text((0, BASELINE), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    cov = [[0] * CELL_W for _ in range(CELL_H)]
    for y in range(CELL_H):
        for x in range(CELL_W):
            v = px[x, y]
            cov[y][x] = (v >> 4) if quant == "trunc" else (v * 15 + 127) // 255
    ink = 0
    for x in range(CELL_W):
        if any(px[x, y] > 8 for y in range(CELL_H)):
            ink = x + 1
    adv = ink + 2 if ink else max(3, int(font.getlength(ch)))
    return cov, min(adv, CELL_W)


def draw_text(dst, x0, y0, text, fgc, font, quant, table, scale, bg_of_row):
    """Paint a string the way the engine does: whole cell, fg over bg, with the
    engine's Bresenham replication for scales above 1x."""
    pen = x0
    for ch in text:
        if not (0x20 <= ord(ch) <= 0x7E):
            ch = "?"
        cov, adv = glyph_cov(font, ch, quant)
        for oy in range(CELL_H * scale):
            sy = oy // scale
            for ox in range(adv * scale):
                sx = ox // scale
                px, py = pen + ox, y0 + oy
                if not (0 <= px < dst.width and 0 <= py < dst.height):
                    continue
                w = table[cov[sy][sx]]
                dst.putpixel((px, py), rgb888(blend(fgc, bg_of_row(py), w)))
        pen += adv * scale


def panel(title, quant, table, size, weight, opsz, w=360, h=132):
    img = Image.new("RGB", (w, h))
    # The real vertical ramp, in 40 bands, as ui_gradient() draws it.
    def bg_of_row(y):
        return mix565(GRAD_TOP, GRAD_BOT, min(y * 40 // h, 40), 40)
    for y in range(h):
        c = rgb888(bg_of_row(y))
        for x in range(w):
            img.putpixel((x, y), c)

    f = load_font(size, weight, opsz)
    # A title at 2x and body lines at 1x -- the two cases that actually appear.
    draw_text(img, 8, 6, "Stockholm Syndrome", UI_WHITE, f, quant, table, 2, bg_of_row)
    draw_text(img, 8, 44, "Muse - Absolution", UI_DIM, f, quant, table, 1, bg_of_row)
    draw_text(img, 8, 64, "MP3 320kbps 44.1kHz JOINT", UI_WHITE, f, quant, table, 1, bg_of_row)
    draw_text(img, 8, 84, "SHUFFLE  REPEAT ALL  EQ: ROCK", ACCENT, f, quant, table, 1, bg_of_row)
    draw_text(img, 8, 104, "12 of 47   -2:41   VOL 65%", UI_DIM, f, quant, table, 1, bg_of_row)
    return img


VARIANTS = [
    # label,                          quant,   table,       size, wght, opsz
    ("A  BEFORE  (shipped today)",    "trunc", COV_LINEAR,  15,   600,  14),
    ("B  + gamma weights",            "trunc", COV_WEIGHT,  15,   600,  14),
    ("C  + rounded coverage  <- now", "round", COV_WEIGHT,  15,   600,  14),
    ("D  C, Medium 500",              "round", COV_WEIGHT,  15,   500,  14),
    ("E  C, Bold 700",                "round", COV_WEIGHT,  15,   700,  14),
    ("F  C, 16px SemiBold",           "round", COV_WEIGHT,  16,   600,  14),
]


def body_panel(quant, table, size, weight, opsz, w=300, h=26):
    """Just one 1x info line -- the case the complaint was actually about, big
    enough to see individual pixels decide the shape of a stem."""
    img = Image.new("RGB", (w, h))

    def bg_of_row(y):
        return mix565(GRAD_TOP, GRAD_BOT, 22, 40)
    c = rgb888(bg_of_row(0))
    for y in range(h):
        for x in range(w):
            img.putpixel((x, y), c)
    f = load_font(size, weight, opsz)
    draw_text(img, 4, 4, "MP3 320kbps 44.1kHz JOINT", UI_WHITE, f,
              quant, table, 1, bg_of_row)
    return img


def focus_sheet(out, zoom=7):
    rows = []
    for lab, q, t, sz, wt, op in VARIANTS:
        rows.append((lab, body_panel(q, t, sz, wt, op)))
    pw, ph = rows[0][1].size
    label_h, pad = 20, 6
    W = pw * zoom + 2 * pad
    H = len(rows) * (ph * zoom + label_h) + pad
    sheet = Image.new("RGB", (W, H), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    ui = ImageFont.load_default()
    for i, (lab, img) in enumerate(rows):
        y = pad + i * (ph * zoom + label_h)
        d.text((pad, y), lab, fill=(235, 235, 240), font=ui)
        sheet.paste(img.resize((pw * zoom, ph * zoom), Image.NEAREST),
                    (pad, y + label_h))
    sheet.save(out)
    print("wrote %s  (%dx%d, %dx zoom)" % (out, W, H, zoom))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "docs" / "font_preview.png"))
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--focus", action="store_true",
                    help="one 1x line per variant, heavily zoomed")
    args = ap.parse_args()

    if args.focus:
        focus_sheet(str(Path(args.out).with_name("font_preview_body.png")))
        return

    label_h, pad = 22, 10
    tiles = []
    for lab, q, t, sz, wt, op in VARIANTS:
        tiles.append((lab, panel(lab, q, t, sz, wt, op)))

    pw, ph = tiles[0][1].size
    cols, rows = 2, (len(tiles) + 1) // 2
    W = cols * (pw * args.zoom + pad) + pad
    H = rows * (ph * args.zoom + label_h + pad) + pad
    sheet = Image.new("RGB", (W, H), (24, 24, 28))
    d = ImageDraw.Draw(sheet)
    ui = ImageFont.load_default()

    for i, (lab, img) in enumerate(tiles):
        cx, cy = i % cols, i // cols
        x = pad + cx * (pw * args.zoom + pad)
        y = pad + cy * (ph * args.zoom + label_h + pad)
        d.text((x, y + 5), lab, fill=(235, 235, 240), font=ui)
        sheet.paste(img.resize((pw * args.zoom, ph * args.zoom), Image.NEAREST),
                    (x, y + label_h))

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.out)
    print("wrote %s  (%dx%d, %dx zoom)" % (args.out, W, H, args.zoom))


if __name__ == "__main__":
    main()
