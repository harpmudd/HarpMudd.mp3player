#!/usr/bin/env python3
"""Generate the extended font: everything the FPGA font ROM cannot hold.

The ROM in the bitstream is ASCII 0x20..0x7E and block RAM is at 97%, so
every other character lives in SDRAM instead. APF loads this file into the
0x1xxxxxxx bridge window at boot, and mp3_fb.sv's CHAR command reads a glyph's
rows from there when the firmware sends glyph 0x7F with an index in the size
register.

Two regions, because two kinds of source want two formats:

  4bpp  Inter, the same typeface and renderer as the ROM (tools/gen_font_rom.py)
        so an accented letter matches the plain one beside it. Latin, Greek,
        Cyrillic, punctuation. A glyph Inter lacks falls back to Unifont,
        promoted to full coverage. Fixed at 1024 glyphs so the engine can find
        the 1bpp region with a constant.
        One glyph = 16 rows x 4 words x 16 bits = 128 bytes.

  1bpp  GNU Unifont, Japanese variant (unifont_jp): kana, CJK ideographs with
        Japanese letterforms, symbols, full/half-width forms. A bitmap face
        drawn for 16 px is sharper at this size than any outline font rendered
        down to it. One glyph = 16 rows x 1 word = 32 bytes.

Word layout matches what the engine reads back:
  4bpp row: words w0..w3 little-endian; rowlo = {w1,w0}, rowhi = {w3,w2}, and
            pixel x is bits [x*4+3 : x*4] of {rowhi,rowlo} -- the ROM's packing.
  1bpp row: bit (15-x) is pixel x, as Unifont's .hex draws it.

Engine index, carried in the 18-bit size register:
  bit 17 = 1bpp region, bits 16..0 = glyph number within the region.

Both fonts are SIL OFL 1.1 (Unifont is dual-licensed OFL / GPLv2+ with the
font embedding exception).

  python tools/gen_font_ext.py            # writes the .bin, the header, a preview
"""
import gzip
import struct
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
TTF = ROOT / "third_party" / "font" / "Inter-SemiBold.ttf"
UNI = ROOT / "third_party" / "unifont" / "unifont_jp-17.0.05.hex.gz"
BIN = ROOT / "dist" / "Assets" / "mp3player" / "common" / "mp3font.bin"
HDR = ROOT / "fw" / "font_ext.h"
PREVIEW = ROOT / "tools" / "font_ext_preview.png"

CELL = 16
N4_MAX = 1024                      # 4bpp region size, in glyphs -- RTL constant
W4, W1 = 64, 16                    # words per glyph
FMT1 = 1 << 17

# Sorted by code point. The firmware searches this in order.
R4 = [  # (name, first, last) -- Inter where it has the glyph
    ("Latin-1 Supplement",  0x00A0, 0x00FF),
    ("Latin Extended-A",    0x0100, 0x017F),
    ("Greek and Coptic",    0x0370, 0x03FF),
    ("Cyrillic",            0x0400, 0x04FF),
    ("General Punctuation", 0x2000, 0x206F),
    ("Currency Symbols",    0x20A0, 0x20CF),
    ("Letterlike Symbols",  0x2100, 0x214F),
]
# 1bpp. The small mixed-width ranges come first and CJK Unified LAST, so the
# width bitmap only has to cover glyph numbers below the ideographs, which are
# all full width.
R1 = [
    ("Arrows",                0x2190, 0x21FF),
    ("Geometric Shapes",      0x25A0, 0x25FF),
    ("Miscellaneous Symbols", 0x2600, 0x26FF),
    ("CJK Symbols and Punct", 0x3000, 0x303F),
    ("Hiragana",              0x3040, 0x309F),
    ("Katakana",              0x30A0, 0x30FF),
    ("Halfwidth/Fullwidth",   0xFF00, 0xFFEF),
    ("CJK Unified Ideographs", 0x4E00, 0x9FFF),
]

# ---- sources ---------------------------------------------------------------
uni = {}
for line in gzip.open(UNI, "rt"):
    cp, h = line.strip().split(":")
    uni[int(cp, 16)] = h

cmap = TTFont(str(TTF)).getBestCmap()
font = ImageFont.truetype(str(TTF), 15)
try:
    font.set_variation_by_axes([14.0, 600.0])
except Exception:
    pass
BASELINE = 12                       # must match gen_font_rom.py


def uni_rows(cp):
    """16 row words, bit 15 = leftmost pixel, and the glyph's width (8/16)."""
    h = uni.get(cp)
    if h is None:
        return [0] * CELL, 16
    if len(h) == 64:
        return [int(h[i * 4:i * 4 + 4], 16) for i in range(CELL)], 16
    return [int(h[i * 2:i * 2 + 2], 16) << 8 for i in range(CELL)], 8


SQUASHED = []                       # glyphs fitted to the cell, for the report


def inter_cov(cp):
    """16x16 coverage 0..15 and advance, rendered as the ROM is.

    Rendered on a taller canvas first, because an accented CAPITAL does not fit
    the ROM's cell: the baseline is row 12 and caps already start at row 1, so
    the accent lands at rows -2..-5 and is simply cut off -- E-acute drew as a
    plain E. Compared on a rendered sheet against shifting the glyph down (the
    baseline visibly jumps) and squeezing only the accent (it shrinks to one
    row and vanishes): squashing the ink above the baseline into the cell keeps
    the accent readable and the baseline true, at the cost of a capital a pixel
    or two shorter, which does not read at this size. Only glyphs that actually
    overflow are touched."""
    ch = chr(cp)
    OFF, H = 8, CELL + 8
    tall = Image.new("L", (CELL, H), 0)
    ImageDraw.Draw(tall).text((0, BASELINE + OFF), ch, font=font, fill=255, anchor="ls")
    tp = tall.load()
    rows = [y for y in range(H) if any(tp[x, y] > 8 for x in range(CELL))]
    top = min(rows) if rows else OFF
    if top < OFF:
        base = OFF + BASELINE - 1               # last row of ink above the baseline
        above = tall.crop((0, top, CELL, base + 1)).resize((CELL, BASELINE), Image.LANCZOS)
        img = Image.new("L", (CELL, CELL), 0)
        img.paste(above, (0, 0))
        img.paste(tall.crop((0, base + 1, CELL, OFF + CELL)), (0, BASELINE))
        SQUASHED.append(cp)
    else:
        img = tall.crop((0, OFF, CELL, OFF + CELL))
    px = img.load()
    right = 0
    for x in range(CELL):
        if any(tp[x, y] > 8 for y in range(H)):
            right = x + 1
    adv = right + 2 if right else max(3, int(font.getlength(ch)))
    cov = [[(px[x, y] * 15 + 127) // 255 for x in range(CELL)] for y in range(CELL)]
    return cov, min(adv, CELL)


# ---- 4bpp region -----------------------------------------------------------
data4 = bytearray()
adv4 = []
ranges = []                          # (first, count, engine index of first)
n4 = 0
fallback4 = 0
for name, a, b in R4:
    ranges.append((a, b - a + 1, n4))
    for cp in range(a, b + 1):
        if cp in cmap:
            cov, adv = inter_cov(cp)
        else:
            rows, wid = uni_rows(cp)
            cov = [[15 if (rows[y] >> (15 - x)) & 1 else 0 for x in range(CELL)]
                   for y in range(CELL)]
            adv = wid
            fallback4 += cp in uni
        for y in range(CELL):
            v = 0
            for x in range(CELL):
                v |= cov[y][x] << (x * 4)
            data4 += struct.pack("<4H", v & 0xFFFF, (v >> 16) & 0xFFFF,
                                 (v >> 32) & 0xFFFF, (v >> 48) & 0xFFFF)
        adv4.append(adv)
        n4 += 1
assert n4 <= N4_MAX, n4
assert all(3 <= a <= 16 for a in adv4), "advance must fit (a-1) in a nibble"
data4 += bytes(N4_MAX * W4 * 2 - len(data4))

# ---- 1bpp region -----------------------------------------------------------
data1 = bytearray()
wide_bits = []                       # 1 = full width, per glyph number
n1 = 0
missing1 = 0
for name, a, b in R1:
    ranges.append((a, b - a + 1, FMT1 | n1))
    for cp in range(a, b + 1):
        rows, wid = uni_rows(cp)
        missing1 += cp not in uni
        data1 += struct.pack("<16H", *rows)
        if name != "CJK Unified Ideographs":
            wide_bits.append(1 if wid == 16 else 0)
        n1 += 1
N1_MIXED = len(wide_bits)

blob = bytes(data4) + bytes(data1)
BIN.parent.mkdir(parents=True, exist_ok=True)
BIN.write_bytes(blob)

# ---- firmware header -------------------------------------------------------
ranges.sort()
nib = []
for i in range(0, n4, 2):
    lo = adv4[i] - 1
    hi = (adv4[i + 1] - 1) if i + 1 < n4 else 0
    nib.append(lo | (hi << 4))
bitmap = [0] * ((N1_MIXED + 7) // 8)
for i, w in enumerate(wide_bits):
    bitmap[i >> 3] |= w << (i & 7)

h = [
    "/* GENERATED by tools/gen_font_ext.py -- DO NOT EDIT. */",
    "#ifndef FONT_EXT_H",
    "#define FONT_EXT_H",
    "",
    "/* Characters outside the ROM's ASCII, drawn from mp3font.bin in SDRAM.",
    " * Engine index = FEXT_1BPP (bit 17) | glyph number. See mp3_fb.sv. */",
    "#define FEXT_1BPP    0x20000u",
    "#define FEXT_GLYPH   0x7Fu        /* CHAR code meaning \"index is in SIZE\" */",
    "#define FEXT_N1_MIXED %du         /* 1bpp glyphs below this have a width bit */" % N1_MIXED,
    "#define FEXT_BYTES   %du      /* file size; smaller means not loaded */" % len(blob),
    "#define FEXT_NRANGES %du" % len(ranges),
    "",
    "/* { first code point, count, engine index of first } */",
    "static const struct { uint16_t first, count; uint32_t base; }",
    "fext_ranges[FEXT_NRANGES] = {",
]
for a, c, base in ranges:
    h.append("    { 0x%04X, %5d, 0x%05X }," % (a, c, base))
h += [
    "};",
    "",
    "/* 4bpp advances, two per byte, stored as (advance - 1). */",
    "static const unsigned char fext_adv4[%d] = {" % len(nib),
]
for i in range(0, len(nib), 16):
    h.append("    " + ",".join("0x%02X" % v for v in nib[i:i + 16]) + ",")
h += [
    "};",
    "",
    "/* 1bpp full-width flags for glyph numbers below FEXT_N1_MIXED. */",
    "static const unsigned char fext_wide1[%d] = {" % len(bitmap),
]
for i in range(0, len(bitmap), 16):
    h.append("    " + ",".join("0x%02X" % v for v in bitmap[i:i + 16]) + ",")
h += ["};", "", "#endif", ""]
HDR.write_text("\n".join(h), newline="\n")

# ---- preview, decoded back out of the .bin -----------------------------------
def draw_from_bin(text, img, x0, y0, fg=(240, 240, 240)):
    x = x0
    px = img.load()
    for ch in text:
        cp = ord(ch)
        hit = next(((f, c, bs) for f, c, bs in ranges if f <= cp < f + c), None)
        if hit is None:
            x += 8
            continue
        f, c, bs = hit
        n = (bs & ~FMT1) + (cp - f)
        if bs & FMT1:
            off = N4_MAX * W4 * 2 + n * W1 * 2
            rows = struct.unpack_from("<16H", blob, off)
            for y in range(CELL):
                for xx in range(CELL):
                    if (rows[y] >> (15 - xx)) & 1:
                        px[x + xx, y0 + y] = fg
            wide = 1 if n >= N1_MIXED else wide_bits[n]
            x += 16 if wide else 8
        else:
            off = n * W4 * 2
            for y in range(CELL):
                w = struct.unpack_from("<4H", blob, off + y * 8)
                v = w[0] | (w[1] << 16) | (w[2] << 32) | (w[3] << 48)
                for xx in range(CELL):
                    k = ((v >> (xx * 4)) & 15) * 17
                    if k:
                        px[x + xx, y0 + y] = tuple(int(q * k / 255) for q in fg)
            x += adv4[n]

samples = [
    "あいうえお アイウ 東京日本語音楽 ★♪♫",
    "宇多田ヒカル 「初恋」 ー、。 ＡＢＣ１２",
    "éèêüöäñçß ŁłŠš ‘’“”…–— €™",
    "Édith Ólafur ÅSA ŠKODA Žižek MØ",
    "ΑβγδΩ Живой →←●■",
]
img = Image.new("RGB", (420, 20 * len(samples) + 8), (18, 22, 36))
for i, s in enumerate(samples):
    draw_from_bin(s, img, 4, 4 + i * 20)
img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
img.save(PREVIEW)

print(f"4bpp: {n4} glyphs ({fallback4} from Unifont, {len(SQUASHED)} squashed to fit), "
      f"region {len(data4)} B")
print(f"1bpp: {n1} glyphs ({missing1} blank), {N1_MIXED} with width bits, {len(data1)} B")
print(f"mp3font.bin {len(blob):,} B; header {len(nib)} adv bytes + {len(bitmap)} width bytes"
      f" + {len(ranges)} ranges")
