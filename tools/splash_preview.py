#!/usr/bin/env python3
"""Render the boot screen, before and after, through the real pixel pipeline.

Composition is the one thing that cannot be argued about in the abstract, and
the complaint here was placement rather than content: the old splash put a
left-aligned title at UI_MARGIN with 120 empty rows above it and 89 below, so
209 of 360 rows carried nothing.

Reuses tools/font_preview.py for glyphs, so the type is the real atlas at the
real metrics with the real gamma-corrected blend, and reproduces ui_grad_at()'s
dithered accent-tinted ramp underneath. Layout constants are READ OUT of
fw/player.c rather than copied, so this cannot quietly disagree with the build.

    python tools/splash_preview.py
"""
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

import font_preview as FP

ROOT = Path(__file__).resolve().parent.parent
SRC = (ROOT / "fw" / "player.c").read_text(encoding="utf-8", errors="replace")

FB_W, FB_H = 400, 360


_consts = {}


def const(name, default=None):
    """Resolve a #define to a number, evaluating simple expressions built from
    constants already resolved. Layout values are sometimes derived from each
    other (the version line is anchored off the card), and hardcoding the
    arithmetic here is exactly the drift this tool exists to prevent."""
    if name in _consts:
        return _consts[name]
    m = re.search(r"#define\s+%s\s+(.+)" % name, SRC)
    if m:
        body = m.group(1).split("/*")[0].strip()
        expr = re.sub(r"(\d)u\b", r"\1", body)
        # Resolve any constants the expression names, recursively, so the order
        # things are asked for here cannot matter. UI_BOOT_Y is defined in terms
        # of UI_SPL_INFO_Y, and a fixed order would break the next time two
        # layout constants are chained.
        env = dict(_consts)
        for ident in set(re.findall(r"[A-Za-z_]\w*", expr)):
            if ident not in env:
                v = const(ident, default=-1)
                if v != -1:
                    env[ident] = v
        try:
            v = int(eval(expr, {"__builtins__": {}}, env))
            _consts[name] = v
            return v
        except Exception:
            pass
    if default is None:
        raise SystemExit("constant %s not resolvable in player.c" % name)
    return default


UI_MARGIN = const("UI_MARGIN")
UI_WAVE_Y = const("UI_WAVE_Y")
UI_WAVE_H = const("UI_WAVE_H")
UI_BOOT_Y = const("UI_BOOT_Y")
UI_DOT_N = const("UI_DOT_N")
UI_DOT_W = const("UI_DOT_W")
UI_DOT_GAP = const("UI_DOT_GAP")
UI_TITLE_Y = const("UI_TITLE_Y")
UI_CARD_H = const("UI_CARD_H")
V_Y = const("UI_SPL_VER_Y")
I_Y = const("UI_SPL_INFO_Y")
VER = re.search(r'#define\s+APP_VER\s+"([^"]+)"', SRC).group(1)

UI_DIM = 0x94B2
UI_WHITE = 0xFFFF
ACCENT = 0x7E55                       # seafoam, as in the current screenshot
BAYER = (1, 5, 3, 7)


def grad_top(accent, L=45):
    r = ((accent >> 11) & 0x1F) * 255 // 31
    g = ((accent >> 5) & 0x3F) * 255 // 63
    b = (accent & 0x1F) * 255 // 31
    l = (2126 * r + 7152 * g + 722 * b) // 10000 or 1
    r, g, b = (r * L + l // 2) // l, (g * L + l // 2) // l, (b * L + l // 2) // l
    r, g, b = (r + L + 1) // 2, (g + L + 1) // 2, (b + L + 1) // 2
    return (min(r, 255) * 31 + 127) // 255 << 11 | \
           (min(g, 255) * 63 + 127) // 255 << 5 | (min(b, 255) * 31 + 127) // 255


TOP = grad_top(ACCENT)


def grad_at(y):
    """ui_grad_at(), reproduced."""
    den, rem, t = FB_H - 1, FB_H - 1 - y, BAYER[y & 3]
    out = []
    for lv in ((TOP >> 11) & 0x1F, (TOP >> 5) & 0x3F, TOP & 0x1F):
        num = lv * rem
        base = num // den
        if (num - base * den) * 8 > t * den:
            base += 1
        out.append(base)
    return out[0] << 11 | out[1] << 5 | out[2]


def new_frame():
    im = Image.new("RGB", (FB_W, FB_H))
    px = im.load()
    for y in range(FB_H):
        c = FP.rgb888(grad_at(y))
        for x in range(FB_W):
            px[x, y] = c
    return im


def text(im, x, y, s, fg, scale):
    FP.draw_text(im, x, y, s, fg, FP.load_font(15, 600, 14),
                 "round", FP.COV_WEIGHT, scale, grad_at)


def width(s, scale):
    """Advance width, matching fb_text_width()."""
    f = FP.load_font(15, 600, 14)
    return sum(FP.glyph_cov(f, c if 0x20 <= ord(c) <= 0x7E else "?", "round")[1]
               for c in s) * scale


def rect(im, x, y, w, h, c):
    d = ImageDraw.Draw(im)
    if w > 0 and h > 0:
        d.rectangle([x, y, x + w - 1, y + h - 1], fill=FP.rgb888(c))


def meter(im, seed=12345):
    """A representative mid-settle frame of the boot animation."""
    rng = seed
    n, ww = 28, FB_W - 2 * UI_MARGIN
    for i in range(n):
        rng ^= (rng << 13) & 0xFFFFFFFF
        rng ^= rng >> 17
        rng ^= (rng << 5) & 0xFFFFFFFF
        h = 6 + rng % (UI_WAVE_H - 10)
        x = UI_MARGIN + i * ww // n
        xn = UI_MARGIN + (i + 1) * ww // n
        w = max(xn - x - 3, 1)
        y = UI_WAVE_Y + UI_WAVE_H - h
        for yy in range(y, UI_WAVE_Y + UI_WAVE_H):
            t = (yy - UI_WAVE_Y) * 31 // UI_WAVE_H
            rect(im, x, yy, w, 1, FP.blend(ACCENT, grad_at(yy), 6 + t // 2))


def dots(im, x, y):
    for i in range(UI_DOT_N):
        lit = (28, 16, 8)[i]
        c = FP.blend(ACCENT, grad_at(y), lit >> 1)
        rect(im, x + i * (UI_DOT_W + UI_DOT_GAP), y, UI_DOT_W, UI_DOT_W, c)


def before():
    im = new_frame()
    meter(im)
    text(im, UI_MARGIN, 120, "MP3 PLAYER", ACCENT, 2)
    text(im, UI_MARGIN, UI_BOOT_Y, "LOADING PLAYLIST", UI_WHITE, 1)
    dots(im, UI_MARGIN + width("LOADING PLAYLIST", 1) + 8, UI_BOOT_Y + 4)
    return im


def after(loading=False):
    """Exactly what fw/player.c now draws. The status row carries the loading
    indicator first, then the playlist summary in its place."""
    im = new_frame()
    round_rect(im, UI_MARGIN - 8, UI_TITLE_Y - 14,
               (FB_W - 2 * UI_MARGIN) + 16, UI_CARD_H, 8, 0x2945)
    FP.draw_text(im, UI_MARGIN, UI_TITLE_Y, "MP3 PLAYER", ACCENT,
                 FP.load_font(15, 600, 14), "round", FP.COV_WEIGHT, 2,
                 lambda _y: 0x2945)
    FP.draw_text(im, UI_MARGIN, V_Y, "v" + VER, UI_DIM,
                 FP.load_font(15, 600, 14), "round", FP.COV_WEIGHT, 1,
                 lambda _y: 0x2945)
    meter(im)
    if loading:
        lbl = "LOADING PLAYLIST"
        text(im, UI_MARGIN, UI_BOOT_Y, lbl, UI_WHITE, 1)
        dots(im, UI_MARGIN + width(lbl, 1) + 8, UI_BOOT_Y + 4)
    else:
        text(im, UI_MARGIN, I_Y, "PLAYLIST", UI_DIM, 1)
        s2 = "10 TRACKS"
        text(im, FB_W - UI_MARGIN - width(s2, 1), I_Y, s2, ACCENT, 1)
    return im


UI_PANEL = 0x2945


def round_rect(im, x, y, w, h, r, c):
    rect(im, x, y + r, w, h - 2 * r, c)
    for i in range(r):
        dy = r - i
        inner = 0
        while (inner + 1) ** 2 + dy * dy <= r * r:
            inner += 1
        cut = r - inner
        rect(im, x + cut, y + i, w - 2 * cut, 1, c)
        rect(im, x + cut, y + h - 1 - i, w - 2 * cut, 1, c)


def main():
    panels = [("BEFORE  original", before()),
              ("AFTER  reading the playlist", after(loading=True)),
              ("AFTER  playlist read", after())]
    zoom, pad, lab = 2, 12, 20
    W = len(panels) * (FB_W * zoom + pad) + pad
    H = FB_H * zoom + lab + 2 * pad
    sheet = Image.new("RGB", (W, H), (20, 20, 24))
    d = ImageDraw.Draw(sheet)
    ui = ImageFont.load_default()
    for i, (name, im) in enumerate(panels):
        x = pad + i * (FB_W * zoom + pad)
        d.text((x, pad), name, fill=(235, 235, 240), font=ui)
        sheet.paste(im.resize((FB_W * zoom, FB_H * zoom), Image.NEAREST),
                    (x, pad + lab))
    out = ROOT / "docs" / "splash_preview.png"
    sheet.save(out)
    print("layout read from player.c: card y=%d h=%d, ver y=%d, status y=%d"
          % (UI_TITLE_Y - 14, UI_CARD_H, V_Y, I_Y))
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
