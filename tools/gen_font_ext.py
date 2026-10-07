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

# ---- 24px title cell -------------------------------------------------------
# The title line was a 15px glyph in a 16px cell drawn at the engine's 1.5x
# scale. 1.5x is the ONLY scale that RESAMPLES -- the source position advances
# num/den per output pixel and takes whatever it lands on (mp3_fb.sv) -- so
# some stems came out a column wider than their neighbours and the line read
# as uneven rather than soft. Rendering the title at its real size and drawing
# it at 1x takes the resampler out of the path entirely; every other caller
# keeps the 16px cell and the scales it has today.
CELL24, BASE24 = 24, 18            # baseline 18: measured, see tools/title_fit.py
W24 = 144                          # 16-bit words per glyph: 24 rows x 6
# Bit 16, not a new bit. The 18-bit ext index is {w[8:0], h[8:0]} with bit 17
# selecting the 1bpp region; the 4bpp region is capped at N4_MAX = 1024, so
# with bit 17 clear only bits 9..0 can ever be set and bit 16 is free. Using
# it meant the engine needed no new command bit, port or MMIO register --
# mp3_fb.sv decodes it as q_w[7]. Keep the two in step.
FMT24 = 1 << 16
# The 24px region appends after the 1bpp one. Generator and mp3_fb.sv both
# hard-code where it starts, exactly as they already do for FONT1_OFF, so the
# assert below is what stops a range added to R1 from silently shifting the
# title font out from under the engine.
OFF24_WORDS = 416768

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


def inter_cov(cp, fnt=None, cell=CELL, bline=BASELINE):
    """cell x cell coverage 0..15 and advance, rendered as the ROM is.

    Rendered on a taller canvas first, because an accented CAPITAL does not fit
    the ROM's cell: the baseline is row 12 and caps already start at row 1, so
    the accent lands at rows -2..-5 and is simply cut off -- E-acute drew as a
    plain E. Compared on a rendered sheet against shifting the glyph down (the
    baseline visibly jumps) and squeezing only the accent (it shrinks to one
    row and vanishes): squashing the ink above the baseline into the cell keeps
    the accent readable and the baseline true, at the cost of a capital a pixel
    or two shorter, which does not read at this size. Only glyphs that actually
    overflow are touched.

    The 24px title cell (cell=24, bline=18) takes the identical path. Measured
    over ASCII + the R4 ranges, the glyphs that overflow there are exactly the
    ones that overflow at 15px -- the bar, and the ring accents -- and the
    squash ratio is the same 75%, but applied to 18 rows above the baseline
    instead of 12, so the accent survives with half as much again to say it in.
    """
    ch = chr(cp)
    fnt = fnt or font
    OFF, H = 8, cell + 8
    tall = Image.new("L", (cell, H), 0)
    ImageDraw.Draw(tall).text((0, bline + OFF), ch, font=fnt, fill=255, anchor="ls")
    tp = tall.load()
    rows = [y for y in range(H) if any(tp[x, y] > 8 for x in range(cell))]
    top = min(rows) if rows else OFF
    if top < OFF:
        base = OFF + bline - 1                  # last row of ink above the baseline
        above = tall.crop((0, top, cell, base + 1)).resize((cell, bline), Image.LANCZOS)
        img = Image.new("L", (cell, cell), 0)
        img.paste(above, (0, 0))
        img.paste(tall.crop((0, base + 1, cell, OFF + cell)), (0, bline))
        SQUASHED.append(cp)
    else:
        img = tall.crop((0, OFF, cell, OFF + cell))
    px = img.load()
    right = 0
    for x in range(cell):
        if any(tp[x, y] > 8 for y in range(H)):
            right = x + 1
    adv = right + 2 if right else max(3, int(fnt.getlength(ch)))
    cov = [[(px[x, y] * 15 + 127) // 255 for x in range(cell)] for y in range(cell)]
    return cov, min(adv, cell)


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

# ---- 24px title region -----------------------------------------------------
# ASCII first so the common case is one contiguous run, then the same Latin /
# Greek / Cyrillic / punctuation ranges the 4bpp region carries.
squashed4 = len(SQUASHED)          # SQUASHED spans both passes; split the count
font24 = ImageFont.truetype(str(TTF), CELL24)
try:
    font24.set_variation_by_axes([14.0, 600.0])
except Exception:
    pass

data24 = bytearray()
adv24 = []
ranges24 = []
n24 = 0
for name, a, b in [("ASCII", 0x20, 0x7E)] + R4:
    ranges24.append((a, b - a + 1, FMT24 | n24))
    for cp in range(a, b + 1):
        if cp in cmap:
            cov, adv = inter_cov(cp, font24, CELL24, BASE24)
        else:
            # No 24px Unifont to fall back to, and a 16px bitmap dropped into
            # a 24px cell would be worse than not offering the size at all.
            # Advance 0 marks "no 24px glyph" and the firmware draws the whole
            # string at 16px rather than mixing two sizes on one line.
            cov, adv = [[0] * CELL24 for _ in range(CELL24)], 0
        for y in range(CELL24):
            v = 0
            for x in range(CELL24):
                v |= cov[y][x] << (x * 4)
            data24 += struct.pack("<6H", *[(v >> (16 * k)) & 0xFFFF
                                           for k in range(6)])
        adv24.append(adv)
        n24 += 1
assert len(data24) == n24 * W24 * 2, len(data24)
assert all(0 <= a <= CELL24 for a in adv24), "24px advance must fit a byte"

assert len(data4) + len(data1) == OFF24_WORDS * 2, (
    "the 1bpp region now ends at word %d, not the %d mp3_fb.sv expects -- "
    "update FONT24_OFF there and OFF24_WORDS here together"
    % ((len(data4) + len(data1)) // 2, OFF24_WORDS))

blob = bytes(data4) + bytes(data1) + bytes(data24)
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
    "#define FEXT_24PX    0x%05Xu     /* bit 16: 24x24 title cell */" % FMT24,
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
    "/* ---- 24px title cell ------------------------------------------- */",
    "#define FEXT24_NRANGES %du" % len(ranges24),
    "#define FEXT24_CELL    %du" % CELL24,
    "",
    "/* { first code point, count, engine index of first } */",
    "static const struct { uint16_t first, count; uint32_t base; }",
    "fext24_ranges[FEXT24_NRANGES] = {",
]
for a, c, base in sorted(ranges24):
    h.append("    { 0x%04X, %5d, 0x%05X }," % (a, c, base))
h += [
    "};",
    "",
    "/* 24px advances, one byte each. ZERO means the glyph has no 24px form",
    " * and the caller must draw the whole string at 16px instead. */",
    "static const unsigned char fext24_adv[%d] = {" % len(adv24),
]
for i in range(0, len(adv24), 16):
    h.append("    " + ",".join("%d" % v for v in adv24[i:i + 16]) + ",")

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

print(f"4bpp: {n4} glyphs ({fallback4} from Unifont, {squashed4} squashed to fit), "
      f"region {len(data4)} B")
print(f"1bpp: {n1} glyphs ({missing1} blank), {N1_MIXED} with width bits, {len(data1)} B")
print(f"24px: {n24} glyphs ({sum(1 for a in adv24 if not a)} with no 24px form, "
      f"{len(SQUASHED) - squashed4} squashed to fit), region {len(data24)} B, "
      f"at word {OFF24_WORDS}")
print(f"mp3font.bin {len(blob):,} B; header {len(nib)} adv bytes + {len(bitmap)} width bytes"
      f" + {len(ranges)} ranges")
