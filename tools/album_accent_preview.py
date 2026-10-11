#!/usr/bin/env python3
"""What would an accent derived from the cover art actually look like?

Deriving the accent from the artwork was tried and dropped once already -- the
note on ui_palette says it "changed on every track and read as inconsistency".
That objection is about TRACK-level churn, and an album's tracks share a cover,
so a per-ALBUM accent would be stable within an album and change only between
them. Different enough to be worth looking at rather than arguing about.

This renders, for every album on the card: the cover, three candidate
extraction rules, and the resulting UI gradient top -- put through the SAME
arithmetic ui_grad_set() uses, so what you see is what the core would draw,
not an approximation of it.

The twelve hand-picked palette entries are rendered on the same chart for
comparison, which is the real question: are the derived colours as usable as
the ones a person chose?

    python tools/album_accent_preview.py [card-path]

Writes docs/album_accent.png.
"""
import io
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print('needs Pillow')
    raise SystemExit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARD = sys.argv[1] if len(sys.argv) > 1 else 'E:/Assets/mp3player/common'
OUT = os.path.join(ROOT, 'docs', 'album_accent.png')

UI_GRAD_LUMA = 45           # fw/player.c
ART_IMG = 92

PALETTE = [
    (0xFC65, 'amber'), (0xBF08, 'lime'), (0xAF13, 'mint'), (0x7E55, 'seafoam'),
    (0x5D5C, 'sky'), (0x7C5A, 'indigo'), (0xAC5E, 'lilac'), (0xF534, 'blush'),
    (0xF32D, 'coral'), (0xB9E9, 'crimson'), (0xEE07, 'gold'),
]


def to565(r, g, b):
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 \
        | ((b * 31 + 127) // 255)


def from565(c):
    return (((c >> 11) & 0x1F) * 255 // 31,
            ((c >> 5) & 0x3F) * 255 // 63,
            (c & 0x1F) * 255 // 31)


def grad_top(accent565):
    """ui_grad_set(), transcribed. Normalise to UI_GRAD_LUMA, half-saturate."""
    r, g, b = from565(accent565)
    l = (2126 * r + 7152 * g + 722 * b) // 10000 or 1
    r = (r * UI_GRAD_LUMA + l // 2) // l
    g = (g * UI_GRAD_LUMA + l // 2) // l
    b = (b * UI_GRAD_LUMA + l // 2) // l
    r = min(255, (r + UI_GRAD_LUMA + 1) // 2)
    g = min(255, (g + UI_GRAD_LUMA + 1) // 2)
    b = min(255, (b + UI_GRAD_LUMA + 1) // 2)
    return to565(r, g, b)


def luma(c):
    r, g, b = from565(c)
    return (2126 * r + 7152 * g + 722 * b) / 10000.0


# ---- the three candidate rules ------------------------------------------
def rule_mean(px):
    n = len(px)
    return to565(sum(p[0] for p in px) // n,
                 sum(p[1] for p in px) // n,
                 sum(p[2] for p in px) // n)


def rule_satweighted(px):
    """Mean weighted by saturation -- grey pixels contribute almost nothing."""
    tr = tg = tb = tw = 0
    for r, g, b in px:
        w = max(r, g, b) - min(r, g, b)
        tr += r * w
        tg += g * w
        tb += b * w
        tw += w
    if not tw:
        return rule_mean(px)
    return to565(tr // tw, tg // tw, tb // tw)


def rule_dominant(px):
    """Coarse hue histogram, then the mean of the winning bucket."""
    import colorsys
    buckets = {}
    for r, g, b in px:
        mx, mn = max(r, g, b), min(r, g, b)
        if mx - mn < 24 or mx < 40:
            continue                      # grey or near-black: no hue to vote
        h = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)[0]
        k = int(h * 12) % 12
        acc = buckets.setdefault(k, [0, 0, 0, 0])
        acc[0] += r
        acc[1] += g
        acc[2] += b
        acc[3] += 1
    if not buckets:
        return rule_satweighted(px)
    k = max(buckets, key=lambda k: buckets[k][3])
    a = buckets[k]
    return to565(a[0] // a[3], a[1] // a[3], a[2] // a[3])


RULES = [('mean', rule_mean), ('saturation-weighted', rule_satweighted),
         ('dominant hue', rule_dominant)]


# ---- pull covers ----------------------------------------------------------
def flac_pic(p):
    with open(p, 'rb') as f:
        if f.read(4) != b'fLaC':
            return None
        while True:
            h = f.read(4)
            if len(h) < 4:
                return None
            last, t = h[0] & 0x80, h[0] & 0x7F
            b = f.read((h[1] << 16) | (h[2] << 8) | h[3])
            if t == 6:
                q = 4
                q += 4 + struct.unpack('>I', b[q:q + 4])[0]
                q += 4 + struct.unpack('>I', b[q:q + 4])[0] + 16
                n = struct.unpack('>I', b[q:q + 4])[0]
                return b[q + 4:q + 4 + n]
            if last:
                return None


def mp3_pic(p):
    with open(p, 'rb') as f:
        h = f.read(10)
        if h[:3] != b'ID3':
            return None
        tag = f.read((h[6] << 21) | (h[7] << 14) | (h[8] << 7) | h[9])
    i = 0
    while i + 10 <= len(tag):
        fid = tag[i:i + 4]
        if not fid.strip(b'\0'):
            break
        b4 = tag[i + 4:i + 8]
        n = ((b4[0] << 21) | (b4[1] << 14) | (b4[2] << 7) | b4[3]) \
            if h[3] >= 4 else struct.unpack('>I', b4)[0]
        if fid == b'APIC':
            j = tag[i + 10:i + 10 + n].find(b'\xff\xd8\xff')
            if j >= 0:
                return tag[i + 10 + j:i + 10 + n]
        i += 10 + n
    return None


def covers():
    """One cover per album folder, plus the loose MP3s, deduped by image."""
    out, seen = [], set()
    for dp, _, fns in os.walk(CARD):
        for fn in sorted(fns):
            ext = os.path.splitext(fn)[1].lower()
            if ext not in ('.mp3', '.flac'):
                continue
            p = os.path.join(dp, fn)
            try:
                d = flac_pic(p) if ext == '.flac' else mp3_pic(p)
            except Exception:
                d = None
            if not d:
                continue
            key = (len(d), d[:16])
            if key in seen:
                continue
            seen.add(key)
            label = os.path.basename(dp)
            if label == 'MP3s':
                label = os.path.splitext(fn)[0][:28]
            out.append((label[:28], d))
    return out


def main():
    if not os.path.isdir(CARD):
        print('no such folder: %s' % CARD)
        return 2
    items = covers()
    if not items:
        print('no covers found under %s' % CARD)
        return 2

    # layout
    TH, PAD = 64, 10
    SW = 64
    rows = len(items)
    W = PAD + TH + PAD + len(RULES) * (SW * 2 + PAD) + 220
    H = PAD + rows * (TH + PAD) + 150
    im = Image.new('RGB', (W, H), (12, 12, 14))
    d = ImageDraw.Draw(im)

    d.text((PAD, 6), 'cover', fill=(200, 200, 200))
    x = PAD + TH + PAD
    for name, _ in RULES:
        d.text((x, 6), '%s  (accent / gradient top)' % name, fill=(200, 200, 200))
        x += SW * 2 + PAD

    y = PAD + 14
    for label, jpg in items:
        try:
            cov = Image.open(io.BytesIO(jpg)).convert('RGB')
        except Exception:
            continue
        small = cov.resize((ART_IMG, ART_IMG))      # what the core ends up with
        px = list(small.getdata())
        im.paste(cov.resize((TH, TH)), (PAD, y))

        x = PAD + TH + PAD
        for _, fn in RULES:
            a = fn(px)
            g = grad_top(a)
            d.rectangle([x, y, x + SW, y + TH], fill=from565(a))
            d.rectangle([x + SW, y, x + SW * 2, y + TH], fill=from565(g))
            d.text((x + 2, y + TH - 12), '%.0f' % luma(a), fill=(0, 0, 0))
            x += SW * 2 + PAD
        d.text((x + 6, y + TH // 2 - 6), label, fill=(190, 190, 190))
        y += TH + PAD

    # the hand-picked palette, same treatment, for comparison
    y += 16
    d.text((PAD, y), 'the twelve the user picks from, same gradient maths:',
           fill=(200, 200, 200))
    y += 16
    x = PAD
    for c, nm in PALETTE:
        d.rectangle([x, y, x + 46, y + 28], fill=from565(c))
        d.rectangle([x, y + 28, x + 46, y + 46], fill=from565(grad_top(c)))
        d.text((x + 2, y + 48), '%s %.0f' % (nm[:6], luma(c)), fill=(170, 170, 170))
        x += 52

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im.save(OUT)
    print('%d covers -> %s' % (len(items), OUT))
    print()
    print('%-30s %-22s %-22s %s' % ('album', 'mean', 'sat-weighted', 'dominant'))
    for label, jpg in items:
        try:
            px = list(Image.open(io.BytesIO(jpg)).convert('RGB')
                      .resize((ART_IMG, ART_IMG)).getdata())
        except Exception:
            continue
        cells = []
        for _, fn in RULES:
            a = fn(px)
            cells.append('%04X luma %-3.0f' % (a, luma(a)))
        print('%-30s %-22s %-22s %s' % (label[:30], cells[0], cells[1], cells[2]))
    print()
    print('palette luma range: %.0f..%.0f  (what a usable accent looks like)'
          % (min(luma(c) for c, _ in PALETTE), max(luma(c) for c, _ in PALETTE)))
    return 0


sys.exit(main())
