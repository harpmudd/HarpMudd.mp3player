#!/usr/bin/env python3
"""Builds a long playlist of tiny silent MP3s, to test resume past track 127.

The resume point packs the track index into a split field -- low 7 bits in
one place, the 8th in bit 25 -- and the writer once filled only the first
half, so any index above 127 came back 128 too low. tools/host/resume_check.py
proves the packing exhaustively; this is the end-to-end half, and it needs a
playlist longer than 128 entries, which a normal library may not have.

DISTINCT FILENAMES MATTER. The resume point carries an 8-bit fingerprint of
the filename and throws the position away when it disagrees, so entries
sharing a name would make the guard unable to tell track 130 from track 2 --
which is exactly the discrimination under test. Every entry is its own file.

The audio is silence (valid MPEG-1 Layer III frames with empty side info),
two seconds each, so the whole set is well under a megabyte. Tags give each
track a number you can read off the screen.

Everything lands in ONE folder with its own playlist, so nothing mixes with
real music and removing it is deleting that folder.

    python tools/make_resume_test.py --out "D:/Assets/mp3player/common"
    python tools/make_resume_test.py --out ... --clean      (remove it again)
"""
import argparse
import io
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from make_lang_tests import HDR, FRAME, tag              # noqa: E402

FOLDER = '_rgtest'
PLAYLIST = '_rgtest.m3u'
COUNT = 150              # > 128, with margin either side of the fold
SECONDS = 2


def silence(seconds):
    frames = int(seconds * 44100 / 1152)
    return (HDR + bytes(FRAME - len(HDR))) * frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True,
                    help='the common/ folder on the card')
    ap.add_argument('--count', type=int, default=COUNT)
    ap.add_argument('--clean', action='store_true',
                    help='delete the fixture instead of creating it')
    a = ap.parse_args()

    base = os.path.abspath(a.out)
    folder = os.path.join(base, FOLDER)
    m3u = os.path.join(base, PLAYLIST)

    if a.clean:
        n = 0
        if os.path.isdir(folder):
            n = len([f for f in os.listdir(folder)])
            shutil.rmtree(folder)
        if os.path.exists(m3u):
            os.remove(m3u)
        print('removed %s (%d files) and %s' % (folder, n, PLAYLIST))
        return 0

    if not os.path.isdir(base):
        sys.exit('no such folder: %s' % base)
    os.makedirs(folder, exist_ok=True)

    audio = silence(SECONDS)
    lines = ['#EXTM3U']
    for i in range(1, a.count + 1):
        name = 'rg%03d.mp3' % i
        body = tag('Track %03d' % i, 'Resume Test', 'Resume Test', 0) + audio
        io.open(os.path.join(folder, name), 'wb').write(body)
        lines.append('#EXTINF:%d,Resume Test - Track %03d' % (SECONDS, i))
        lines.append('%s/%s' % (FOLDER, name))

    text = '\r\n'.join(lines) + '\r\n'
    io.open(m3u, 'w', encoding='utf-8', newline='').write(text)

    # The 12 KB cap covers the WHOLE file, comments included.
    size = len(text.encode('utf-8'))
    print('%d files in %s/  (%.0f KB total)'
          % (a.count, FOLDER, a.count * len(body) / 1024.0))
    print('%s is %d B of the 12288 B cap  (%.0f%%)'
          % (PLAYLIST, size, size * 100.0 / 12288))
    if size > 12288:
        print('  WARNING: over the cap -- the core will say CLIPPED AT n')

    # playlists.m3u is what lets a long name be remembered; add ourselves.
    idx = os.path.join(base, 'playlists.m3u')
    if os.path.exists(idx):
        s = io.open(idx, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in s else '\n'
        if PLAYLIST not in s:
            if not s.endswith(nl):
                s += nl
            io.open(idx, 'w', encoding='utf-8', newline='').write(
                s + PLAYLIST + nl)
            print('added %s to playlists.m3u' % PLAYLIST)
        else:
            print('playlists.m3u already lists it')
    return 0


if __name__ == '__main__':
    sys.exit(main())
