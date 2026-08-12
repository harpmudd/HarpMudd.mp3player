#!/usr/bin/env python3
"""Render the VIZ_EYE magic-eye meter (EM84 twin tube) as ASCII, using the
firmware's exact integer math.

The point is to see the geometry BEFORE burning a hardware round trip: that the
tubes are centred, that nothing overflows the meter box on either panel width,
and that the lit strip plus its halo plus the dark remainder tile the strip
window exactly -- a gap there leaves debris behind as the bar moves.

Mirrors the VIZ_EYE block in fw/player.c.
"""

UI_MARGIN, UI_INNER_W = 12, 296
UI_WAVE_Y, UI_WAVE_H = 173, 72
ART_X = 276

EYE_TUBE_W, EYE_TUBE_G, EYE_BAR_W = 30, 8, 10


def geom(ww):
    pair = 2 * EYE_TUBE_W + EYE_TUBE_G
    th = UI_WAVE_H - 16
    ty = UI_WAVE_Y + 3
    x0 = UI_MARGIN + ((ww - pair) // 2 if ww > pair else 0)
    bh = th - 14
    by = ty + 7
    return pair, th, ty, x0, bh, by


def render(level_l, level_r, ww):
    pair, th, ty, x0, bh, by = geom(ww)
    g = [['.'] * ww for _ in range(UI_WAVE_H)]        # '.' = gradient bed

    for ch, lv in ((0, level_l), (1, level_r)):
        tx = x0 + ch * (EYE_TUBE_W + EYE_TUBE_G)
        assert tx + EYE_TUBE_W <= UI_MARGIN + ww, ("tube overflows width", ww)
        assert ty + th + 8 <= UI_WAVE_Y + UI_WAVE_H, "tube+base overflows"

        for y in range(ty, ty + th):                  # envelope (corners n/a)
            for x in range(tx, tx + EYE_TUBE_W):
                g[y - UI_WAVE_Y][x - UI_MARGIN] = ' '
        for y in range(ty + 9, ty + th - 9):          # glass highlight
            g[y - UI_WAVE_Y][tx + 2 - UI_MARGIN] = '|'

        bx = tx + (EYE_TUBE_W - EYE_BAR_W) // 2
        lit = (lv * bh) // 255
        ly = by + bh - lit

        painted = 0
        for y in range(by, ly):                       # dark remainder
            for x in range(bx - 2, bx + EYE_BAR_W + 2):
                g[y - UI_WAVE_Y][x - UI_MARGIN] = '-'
                painted += 1
        for y in range(ly, by + bh):                  # lit strip + halo
            for x in range(bx - 2, bx):
                g[y - UI_WAVE_Y][x - UI_MARGIN] = '+'
                painted += 1
            for x in range(bx, bx + EYE_BAR_W):
                g[y - UI_WAVE_Y][x - UI_MARGIN] = '#'
                painted += 1
            for x in range(bx + EYE_BAR_W, bx + EYE_BAR_W + 2):
                g[y - UI_WAVE_Y][x - UI_MARGIN] = '+'
                painted += 1

        # The spans must tile the strip window exactly -- a gap leaves the
        # previous frame's pixels on screen, an overlap is a wasted write.
        assert painted == (EYE_BAR_W + 4) * bh, (ch, lv, painted)
    return g


def show(l, r, ww, label):
    print(f"--- {label}: L={l} R={r} width={ww} ---")
    for row in render(l, r, ww):
        print('   ' + ''.join(row).rstrip())
    print()


if __name__ == '__main__':
    art_w = ART_X - UI_MARGIN - 4
    show(0, 0, art_w, 'silence')
    show(150, 96, art_w, 'playing, louder on the left')
    show(255, 255, art_w, 'full scale')

    print("=== tiling and bounds, all levels x both widths ===")
    n = 0
    for ww in (art_w, UI_INNER_W):
        for lv in range(256):
            render(lv, 255 - lv, ww)
            n += 1
    print(f"{n} renders: strip window tiled exactly, tubes inside the box")

    for ww in (art_w, UI_INNER_W):
        pair, th, ty, x0, bh, by = geom(ww)
        print(f"width {ww}: side margin {x0 - UI_MARGIN}px, "
              f"strip window {bh}px tall, envelope {th}px of {UI_WAVE_H}px")
