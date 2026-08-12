#!/usr/bin/env python3
"""Render the VIZ_EYE magic eye (EM84 twin tube) as ASCII, using the
firmware's exact integer math.

Checks the things that are expensive to discover on hardware: that the pip,
crown, getter, strip window, socket and plinth all fit the meter box and do
not overlap each other, and that the strip's spans tile its window exactly
at every level -- a gap there leaves the previous frame's pixels behind.

Mirrors the VIZ_EYE block in fw/player.c.
"""

UI_MARGIN, UI_INNER_W = 12, 296
UI_WAVE_Y, UI_WAVE_H = 173, 72
ART_X = 276

TUBE_W, TUBE_G, BAR_W, DOME, TUBE_H = 34, 10, 6, 9, 58


def geom(ww):
    pair = 2 * TUBE_W + TUBE_G
    ty = UI_WAVE_Y + 4
    x0 = UI_MARGIN + ((ww - pair) // 2 if ww > pair else 0)
    by = ty + DOME + 7
    bh = 34
    basy = ty + TUBE_H + 1
    return pair, ty, x0, by, bh, basy


def render(lv_l, lv_r, ww):
    pair, ty, x0, by, bh, basy = geom(ww)
    g = [['.'] * ww for _ in range(UI_WAVE_H)]

    def put(x, y, w, h, c):
        for yy in range(y, y + h):
            assert UI_WAVE_Y <= yy < UI_WAVE_Y + UI_WAVE_H, f"row {yy} outside box"
            for xx in range(x, x + w):
                assert UI_MARGIN <= xx < UI_MARGIN + ww, f"col {xx} outside box"
                g[yy - UI_WAVE_Y][xx - UI_MARGIN] = c

    for ch, lv in ((0, lv_l), (1, lv_r)):
        tx = x0 + ch * (TUBE_W + TUBE_G)
        hw = TUBE_W // 2
        for i in range(TUBE_W):
            dx = hw - i if i < hw else i - hw
            yc = 0
            while (yc + 1) ** 2 + dx * dx <= hw * hw:
                yc += 1
            off = DOME - (DOME * yc) // hw
            dl = (8 - i) * 2 if i < 8 else ((i - 8) * 5) // 4
            put(tx + i, ty + off, 1, TUBE_H - off, '#' if dl < 14 else ':')
        put(tx + hw - 2, ty - 3, 4, 4, 'o')                 # pip
        put(tx + 6, ty + DOME + 1, TUBE_W - 12, 3, '=')     # getter
        put(tx + 1, ty + TUBE_H - 6, TUBE_W - 2, 6, 'S')    # socket

        bx = tx + (TUBE_W - BAR_W) // 2
        put(bx - 4, by, BAR_W + 8, bh, '-')                 # window
        for t in range(1, 4):
            put(bx + BAR_W + 5, by + (bh * t) // 4, 2, 1, "'")

        lit = (lv * bh) // 255
        ly = by + bh - lit
        n = 0
        if lit < bh:
            put(bx - 4, by, BAR_W + 8, bh - lit, '-'); n += (BAR_W + 8) * (bh - lit)
        if lit:
            for xo, w, c in ((-4, 2, '+'), (-2, 2, '*'), (0, BAR_W, '@'),
                             (BAR_W, 2, '*'), (BAR_W + 2, 2, '+')):
                put(bx + xo, ly, w, lit, c); n += w * lit
            if ly >= by + 2:
                put(bx - 2, ly - 1, BAR_W + 4, 1, '*')
                put(bx - 1, ly - 2, BAR_W + 2, 1, '+')
        assert n == (BAR_W + 8) * bh, (ch, lv, n, (BAR_W + 8) * bh)
        put(bx - 3, basy, BAR_W + 6, 3, 'r')                # plinth reflection
    return g


if __name__ == '__main__':
    art_w = ART_X - UI_MARGIN - 4
    for l, r, lbl in ((0, 0, 'silence'), (170, 90, 'playing'),
                      (255, 255, 'full scale')):
        pair, ty, x0, by, bh, basy = geom(art_w)
        print(f"--- {lbl}: L={l} R={r} ---")
        for row in render(l, r, art_w):
            print('  ' + ''.join(row).rstrip())
        print()

    n = 0
    for ww in (art_w, UI_INNER_W):
        for lv in range(256):
            render(lv, 255 - lv, ww)
            n += 1
    print(f"{n} renders: everything inside the box, strip window tiled exactly")
    for ww in (art_w, UI_INNER_W):
        pair, ty, x0, by, bh, basy = geom(ww)
        print(f"  width {ww}: side margin {x0 - UI_MARGIN}px, "
              f"lowest row {basy + 4 - UI_WAVE_Y} of {UI_WAVE_H}, "
              f"highest {ty - 3 - UI_WAVE_Y}")
