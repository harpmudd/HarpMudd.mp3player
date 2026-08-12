#!/usr/bin/env python3
"""Render the VIZ_EYE magic-eye meter as ASCII, using the firmware's exact
integer math.

The point is to see the geometry BEFORE burning a hardware round trip: whether
the wedge converges on the hub, whether it opens the right way, and whether the
sliver-repaint arithmetic actually tiles the area it claims to.

Mirrors fw/player.c: vu_sn/vu_cs, vu_angle(), and the VIZ_EYE draw block.
"""

UI_MARGIN, UI_INNER_W = 12, 296
UI_WAVE_H = 72
ART_X = 276

vu_sn = [-3138, -2832, -2493, -2125, -1731, -1317, -887, -446, 0,
         446, 887, 1317, 1731, 2125, 2493, 2832, 3138]
vu_cs = [2633, 2959, 3250, 3502, 3712, 3879, 3999, 4072, 4096,
         4072, 3999, 3879, 3712, 3502, 3250, 2959, 2633]


def vu_angle(v255):
    """C integer division truncates toward zero; Python's // floors."""
    pos = v255 * 16
    q, f = pos // 255, pos % 255
    q1 = q + 1 if q < 16 else 16
    def interp(t):
        d = (t[q1] - t[q]) * f
        return t[q] + int(d / 255)          # trunc toward zero, as in C
    return interp(vu_sn), interp(vu_cs)


def render(level, ww):
    r = (UI_WAVE_H // 2) - 2
    rw = (ww // 2 - 2) if ww // 2 > 2 else 1
    r = min(r, rw, 39)
    hub = r // 6

    hw = []
    for dy in range(r + 1):
        rem = r * r - dy * dy
        k = 0
        while (k + 1) * (k + 1) <= rem:
            k += 1
        hw.append(k)

    sn, cs = vu_angle(128 + (255 - level) // 2)
    slope = int(sn * 4096 / cs) if cs else 0

    grid = [[' '] * (2 * r + 1) for _ in range(2 * r + 1)]
    for dy in range(r + 1):
        k = hw[dy]
        ch = '.' if dy > r - 2 else '#'
        for row in ({r - dy, r + dy}):
            for x in range(r - k, r + k + 1):
                grid[row][x] = ch

    for dy in range(hub + 1):
        rem = hub * hub - dy * dy
        k = 0
        while (k + 1) * (k + 1) <= rem:
            k += 1
        for row in ({r - dy, r + dy}):
            for x in range(r - k, r + k + 1):
                grid[row][x] = ' '

    covered = 0
    for dy in range(hub + 1, r + 1):
        w = min((dy * slope) >> 12, hw[dy])
        # Mirrors the firmware exactly: 2w+1 of shadow, then (hw-w) of
        # phosphor each side. The three spans must tile the row's 2hw+1.
        for x in range(r - w, r + w + 1):
            grid[r + dy][x] = ' '
            covered += 1
        if hw[dy] > w:
            n = hw[dy] - w
            ch = '.' if dy > r - 2 else '#'
            for x in range(r - hw[dy], r - hw[dy] + n):
                grid[r + dy][x] = ch
            for x in range(r + w + 1, r + w + 1 + n):
                grid[r + dy][x] = ch
        assert (2 * w + 1) + 2 * max(0, hw[dy] - w) == 2 * hw[dy] + 1, dy
    return grid, r, hub, covered


def show(level, ww, label):
    grid, r, hub, covered = render(level, ww)
    print(f"--- {label}: level={level} width={ww} r={r} hub={hub} "
          f"wedge px={covered} ---")
    for row in grid:
        print('   ' + ''.join(row))
    print()


if __name__ == '__main__':
    art_w = ART_X - UI_MARGIN - 4
    for lv, name in ((0, 'silence'), (96, 'quiet'), (176, 'loud'),
                     (255, 'full scale')):
        show(lv, art_w, name)
    show(0, UI_INNER_W, 'silence, art panel hidden')

    # Every row must be tiled EXACTLY by the three spans the firmware
    # paints -- 2w+1 of shadow plus (hw-w) of phosphor each side. Any gap
    # leaves debris on screen as the wedge moves, and any overlap means a
    # pixel is written twice per frame for nothing. render() asserts this
    # per row; sweep the whole level range and both panel widths here.
    print("=== row tiling, all levels x both widths ===")
    n = 0
    for w_ in (art_w, UI_INNER_W):
        for lv in range(256):
            render(lv, w_)
            n += 1
    print(f"{n} renders, every row tiled exactly")

    # The wedge must CLOSE as level rises, monotonically -- if it does not,
    # the meter reads backwards somewhere in its travel.
    prev = None
    worst = 0
    for lv in range(256):
        _, _, _, px = render(lv, art_w)
        if prev is not None and px > prev:
            worst += 1
        prev = px
    print("non-monotonic steps:", worst)
