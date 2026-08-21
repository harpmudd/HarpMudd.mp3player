#!/usr/bin/env python3
"""Renders the playlist overlay to the terminal, from the real constants.

Layout mistakes on this core are expensive: they cost a Quartus-free rebuild
but still a card swap and a look. Every UI element that got drawn blind here
needed several rounds -- the magic eye took about eight. Drawing it on the desk
first is how that stopped.

Reads the geometry out of fw/player.c and the glyph widths out of
fw/font_metrics.h, so it cannot drift from the firmware by being edited
separately. Names come from the card if it is mounted, otherwise a sample.

    python overlay_preview.py [--sel N] [--top N] [--playing N]
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW   = os.path.join(HERE, '..', 'fw')


def consts():
    """Pull the overlay geometry straight from the firmware."""
    src = open(os.path.join(FW, 'player.c'), encoding='utf-8', errors='replace').read()
    out = {}
    for name in ('PL_UI_ROWS', 'PL_UI_X', 'PL_UI_Y', 'PL_UI_ROW_H',
                 'PL_UI_LIST_Y', 'PL_UI_PAD_B', 'FB_W', 'FB_H'):
        m = re.search(r'#define\s+%s\s+\(?([0-9]+)' % name, src)
        if m:
            out[name] = int(m.group(1))
    out.setdefault('FB_W', 400)
    out.setdefault('FB_H', 360)
    out['PL_UI_W'] = out['FB_W'] - 2 * out['PL_UI_X']
    return out


def advances():
    src = open(os.path.join(FW, 'font_metrics.h'), encoding='utf-8').read()
    body = src.split('font_adv[95] = {', 1)[1].split('};', 1)[0]
    return [int(v) for v in re.findall(r'\d+', body)]


def width(s, adv):
    return sum(adv[ord(c) - 0x20] if 0 <= ord(c) - 0x20 < 95 else 4 for c in s)


def label(name):
    """Mirrors pl_ui_label(): last path component, extension trimmed."""
    base = name.replace('\\', '/').rsplit('/', 1)[-1]
    dot = base.rfind('.')
    if dot > 0 and len(base) - dot <= 5:
        base = base[:dot]
    return base


def clip(s, budget, adv):
    """Mirrors fb_text_boxed(): whole characters only, hard right edge."""
    w = 0
    for i, c in enumerate(s):
        cw = adv[ord(c) - 0x20] if 0 <= ord(c) - 0x20 < 95 else 4
        if w + cw > budget:
            return s[:i], True
        w += cw
    return s, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sel', type=int, default=2)
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--playing', type=int, default=2)
    a = ap.parse_args()

    C, adv = consts(), advances()
    rows, budget = C['PL_UI_ROWS'], C['PL_UI_W'] - 40   # 10 px to the scrollbar

    names = sorted(os.path.basename(p) for p in
                   glob.glob('D:/Assets/mp3player/common/*.mp3') +
                   glob.glob('D:/Assets/mp3player/common/*.flac'))
    src = 'the card'
    if not names:
        src = 'a sample (card not mounted)'
        names = ["Bad Religion - A Walk.mp3", "Blind Melon - Galaxie.mp3",
                 "Chris Robinson Brotherhood - Narcissus Soaking Wet.mp3",
                 "Death Cab for Cutie - Soul Meets Body.mp3",
                 "Gorillaz - Feel Good Inc..mp3", "Phish - Chalk Dust Torture.mp3",
                 "Pigeon - Black James Dean.mp3", "Sea Wolf - You're A Wolf.mp3",
                 "Stockholm Syndrome - Couldn't get it right.mp3",
                 "Stone Temple Pilots - Interstate Love Song.mp3",
                 "Widespread Panic - Goodpeople.mp3"]

    panel_h = (C['PL_UI_LIST_Y'] + rows * C['PL_UI_ROW_H']
               + C.get('PL_UI_PAD_B', 12) - C['PL_UI_Y'])
    cols = 46

    print("names from %s   %d tracks\n" % (src, len(names)))
    print("    +" + "-" * cols + "+")
    hdr = "PLAYLIST  %d / %d" % (a.sel + 1, len(names))
    print("    |%-*s|" % (cols, " " + hdr))
    print("    |" + " " * cols + "|")

    truncated = 0
    for i in range(rows):
        pos = a.top + i
        if pos >= len(names):
            print("    |" + " " * cols + "|")
            continue
        txt, cut = clip(label(names[pos]), budget, adv)
        truncated += cut
        mark = ">" if pos == a.playing else " "
        line = " %s %s" % (mark, txt)
        # scrollbar lane: "|" is the thumb, ":" the empty track
        bar = " "
        if len(names) > rows:
            span = len(names) - rows
            th   = max(1, rows * rows // len(names))
            ty   = (rows - th) * a.top // span if span else 0
            bar  = "|" if ty <= i < ty + th else ":"
        if pos == a.sel:
            print("    |\x1b[7m%-*s\x1b[0m%s|" % (cols - 1, line[:cols - 1], bar))
        else:
            print("    |%-*s%s|" % (cols - 1, line[:cols - 1], bar))
    print("    +" + "-" * cols + "+")

    print("\n  panel   y %d..%d of %d   %s"
          % (C['PL_UI_Y'], C['PL_UI_Y'] + panel_h, C['FB_H'],
             "fits" if C['PL_UI_Y'] + panel_h <= C['FB_H'] else "OVERFLOWS"))
    print("  rows    %d visible of %d tracks" % (rows, len(names)))
    print("  budget  %d px per label; %d of %d shown names are clipped"
          % (budget, truncated, min(rows, len(names))))
    print("  legend  '>' currently playing, inverse = cursor")


if __name__ == '__main__':
    main()
