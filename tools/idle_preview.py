#!/usr/bin/env python3
"""Render the getting-started (idle) screen through the real pixel pipeline.

This is the first thing a new user sees, and composition is the one thing that
cannot be argued about in the abstract -- so draw it rather than describe it.

Reuses tools/splash_preview.py, which in turn reuses font_preview, so the type
is the real atlas at the real metrics with the real gamma-corrected blend over
ui_grad_at()'s dithered ramp. The lines and their y positions are PARSED OUT of
ui_idle_screen() in fw/player.c, so this cannot quietly disagree with the
build: change the firmware and the preview follows.

    python tools/idle_preview.py            # both states
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import splash_preview as SP          # noqa: E402  (path set above)
import font_preview as FP            # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SRC = (ROOT / "fw" / "player.c").read_text(encoding="utf-8", errors="replace")

FB_W, FB_H = 400, 360
MARGIN = SP.const("UI_MARGIN", 20)
INNER_W = FB_W - 2 * MARGIN

COLOR = {"UI_WHITE": 0xFFFF, "UI_DIM": 0x94B2, "UI_RED": 0xF800,
         "ui_accent": SP.ACCENT}

# ts_half from fw/player.c: size = 16 * half / 2. Kept in HALF units, not as a
# float, because that is what the engine's Bresenham actually indexes with.
HALF = {"TS_1X": 2, "TS_15X": 3, "TS_2X": 4, "TS_3X": 6}


def draw_frac(dst, x0, y0, text, fgc, font, half, bg_of_row):
    """fb_text at a fractional scale.

    font_preview.draw_text only replicates by an integer (sy = oy // scale), so
    it cannot draw the 1.5x heading at all. The engine advances the source
    position by num/den per output pixel, which in half units is
    sy = oy * 2 // half -- reproduced here rather than patched into the shared
    tool, which the other previews depend on."""
    pen = x0
    for ch in text:
        if not (0x20 <= ord(ch) <= 0x7E):
            ch = "?"
        cov, adv = FP.glyph_cov(font, ch, "round")
        oh = FP.CELL_H * half // 2
        ow = adv * half // 2
        for oy in range(oh):
            sy = min(oy * 2 // half, FP.CELL_H - 1)
            for ox in range(ow):
                sx = min(ox * 2 // half, adv - 1)
                px, py = pen + ox, y0 + oy
                if not (0 <= px < dst.width and 0 <= py < dst.height):
                    continue
                w = FP.COV_WEIGHT[cov[sy][sx]]
                dst.putpixel((px, py),
                             FP.rgb888(FP.blend(fgc, bg_of_row(py), w)))
        pen += ow


def parse_lines():
    """Pull the ui_gs_line() calls straight out of the firmware."""
    body = SRC.split("static void ui_idle_screen(")[1].split("\n}")[0]
    out = []
    for y, s, fg, ts in re.findall(
            r'ui_gs_line\(\s*(\d+)u,\s*"((?:[^"\\]|\\.)*)"\s*,\s*'
            r'(\w+)\s*,\s*(\w+)\s*\)', body):
        out.append((int(y), s.replace('\\"', '"'), COLOR[fg], HALF[ts]))
    return out


def render(reason=None):
    im = SP.new_frame()
    f = FP.load_font(15, 600, 14)

    # The card, title and version, exactly as the splash draws them.
    SP.round_rect(im, MARGIN - 4, SP.UI_TITLE_Y - 14, INNER_W + 8,
                  SP.const("UI_CARD_H", 120), 10, SP.UI_PANEL)
    draw_frac(im, MARGIN, SP.UI_TITLE_Y, "MP3 PLAYER", SP.ACCENT, f, 4,
              lambda y: SP.UI_PANEL)
    draw_frac(im, MARGIN, SP.V_Y, "v" + SP.VER, SP.UI_DIM, f, 2,
              lambda y: SP.UI_PANEL)

    # The error line is drawn from a VARIABLE, not a literal, so parse_lines()
    # cannot see it -- it only matches quoted strings. Handled explicitly, and
    # its y is read out of the firmware so it still cannot drift. Getting this
    # wrong once already produced two "different" previews that were identical.
    if reason:
        m = re.search(r'if \(reason\) ui_gs_line\((\d+)u', SRC)
        draw_frac(im, MARGIN, int(m.group(1)), reason, COLOR["UI_RED"], f, 2,
                  SP.grad_at)

    for y, s, fg, half in parse_lines():
        draw_frac(im, MARGIN, y, s, fg, f, half, SP.grad_at)
    return im


def main():
    widest = 0
    for y, s, _fg, half in parse_lines():
        w = SP.width(s, half / 2.0)
        widest = max(widest, w)
        flag = "" if w <= INNER_W else "   <-- OVERFLOWS"
        print("  y=%-4d %-42s %4d px%s" % (y, '"' + s + '"', w, flag))
    print("\n  widest %d px of %d available (%d spare)"
          % (widest, INNER_W, INNER_W - widest))

    out = ROOT / "docs"
    for name, reason in (("idle_preview.png", None),
                         ("idle_preview_err.png",
                          "No playable tracks in playlist")):
        im = render(reason)
        im.resize((FB_W * 2, FB_H * 2), 0).save(out / name)
        print("  wrote docs/%s" % name)


if __name__ == "__main__":
    main()
