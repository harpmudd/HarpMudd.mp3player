#!/usr/bin/env python3
"""Previews what drives the meter bars, against real audio, before any build.

The bars are driven by peak, and the user reported them pegged: "for many songs
many of the meters are peaked out, very evident on the bar meters." Changing
what drives them alters the look of all ten meters at once, and the magic eye
took about eight rounds to tune on hardware -- so it gets previewed here first.

Input is the table printed by tools/host/level_harness.c, which decodes real
music through the REAL fw/flac.c under tools/rv32sim.py and reports peak,
sum|x| and sum x^2 over 1152-pair windows. That window is the one the meter
actually sees: meters_feed() runs per emitted chunk and the bar is published
once per display frame. Measuring whole FLAC frames instead makes peak look far
better behaved than it is, because a maximum over a longer window is always
larger.

    python tools/meter_preview.py levels.txt [--gain 2.5]
"""
import argparse
import math
import sys

FS = 32768.0
BAR_H = 72          # UI_WAVE_H in fw/player.c


def load(path):
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.split()
        if len(f) != 7 or not f[0].isdigit():
            continue
        _, pk, ah, al, sh, sl, n = (int(x) for x in f)
        rows.append((pk, (ah << 32) | al, (sh << 32) | sl, n))
    return rows


def bars(vals, height=BAR_H):
    return [min(height, int(v * height / FS)) for v in vals]


def render(title, b, height=BAR_H):
    """Vertical bars, one column per window, so pegging is visible as a flat
    top rather than inferred from a percentile."""
    print('\n%s' % title)
    rows = 16
    for r in range(rows, 0, -1):
        lo = (r - 1) * height / rows
        line = ''.join('#' if v > lo else ' ' for v in b)
        edge = '%3d%%|' % int(100 * r / rows)
        print('%s%s|' % (edge, line))
    print('    +' + '-' * len(b) + '+')
    pegged = sum(1 for v in b if v >= height * 0.95)
    print('    span %d..%d of %d   at/over 95%%: %d of %d windows (%.0f%%)'
          % (min(b), max(b), height, pegged, len(b), 100.0 * pegged / len(b)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('table')
    ap.add_argument('--gain', type=float, default=None)
    a = ap.parse_args()

    rows = load(a.table)
    if not rows:
        sys.exit('no data rows in %s' % a.table)

    peak = [r[0] for r in rows]
    mean = [r[1] / r[3] for r in rows]
    rms = [math.sqrt(r[2] / r[3]) for r in rows]

    render('PEAK  (what drives the bars today)', bars(peak))

    if a.gain is not None:
        gains = [a.gain]
    else:
        # Pick the gain from the DATA: put the loudest window near 85% of
        # height, which is the roadmap's target -- loud passages high but with
        # room left to move, rather than flat against the top.
        target = 0.85 * FS
        gains = sorted({round(target / max(mean), 1), 2.0, 2.5, 3.0})

    for g in gains:
        render('MEAN |x| x %.1f' % g, bars([v * g for v in mean]))

    print('\nsummary over %d windows of 1152 pairs' % len(rows))
    print('  peak      mean %5.1f%% FS   max %5.1f%%   pegged %d'
          % (100 * sum(peak) / len(peak) / FS, 100 * max(peak) / FS,
             sum(1 for v in peak if v >= 0.95 * FS)))
    print('  mean|x|   mean %5.1f%% FS   max %5.1f%%'
          % (100 * sum(mean) / len(mean) / FS, 100 * max(mean) / FS))
    print('  rms       mean %5.1f%% FS   max %5.1f%%'
          % (100 * sum(rms) / len(rms) / FS, 100 * max(rms) / FS))
    print('  crest (peak/mean) %.2f' % (sum(peak) / sum(mean)))
    print('\n  gain that puts the loudest window at 85%%: %.1f'
          % (0.85 * FS / max(mean)))


if __name__ == '__main__':
    main()
