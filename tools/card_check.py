#!/usr/bin/env python3
"""Card health: reproduce the core's own file-size cross-check on the host.

read_track_head() shows "! FILE SIZE WRONG - CHECK SD CARD" when the size the
directory reports exceeds the file's own Xing/VBRI BYTES field by more than
25%, which means a damaged FAT entry: the size is wrong while the cluster
chain and the audio are intact. The host reads the same directory entry APF
does, so the check reproduces exactly -- and this runs over every file at once
instead of one per track load.

Also flags covers the core cannot decode, since picojpeg is baseline only and
a progressive JPEG is the other thing that looks like a card fault but is not.
(tools/host/jpeg_check.py is the authority there; this is the quick sweep.)

    python tools/card_check.py                       # default card path
    python tools/card_check.py E:/Assets/mp3player/common
"""
import os
import struct
import sys

DEFAULT = 'E:/Assets/mp3player/common'

# Matches read_track_head(): slot_size > real + real/4
TOLERANCE = 0.25


def id3v2_len(b):
    if len(b) < 10 or b[:3] != b'ID3':
        return 0
    return 10 + ((b[6] << 21) | (b[7] << 14) | (b[8] << 7) | b[9]) \
             + (10 if (b[5] & 0x10) else 0)


def mp3_declared(path):
    """(audio_start, BYTES) as the core would parse them, or None."""
    with open(path, 'rb') as f:
        head = f.read(16)
        start = id3v2_len(head)
        f.seek(start)
        buf = f.read(4096)
    off = None
    for i in range(len(buf) - 4):
        if buf[i] != 0xFF or (buf[i + 1] & 0xE0) != 0xE0:
            continue
        ver, lay = (buf[i + 1] >> 3) & 3, (buf[i + 1] >> 1) & 3
        bri, sri = (buf[i + 2] >> 4) & 15, (buf[i + 2] >> 2) & 3
        if ver == 1 or lay != 1 or bri in (0, 15) or sri == 3:
            continue
        off = i
        break
    if off is None:
        return None
    # The core scans the ring for the literal rather than computing an offset.
    win = buf[:2048]
    for i in range(len(win) - 20):
        tag = win[i:i + 4]
        if tag in (b'Xing', b'Info'):
            flags = int.from_bytes(win[i + 4:i + 8], 'big')
            if not (flags & 1) or not (flags & 2):
                return None
            return start + off, int.from_bytes(win[i + 12:i + 16], 'big')
        if tag == b'VBRI':
            return start + off, int.from_bytes(win[i + 10:i + 14], 'big')
    return None


def jpeg_mode(d):
    i = 2
    while i < len(d) - 1:
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m in (0xD8, 0xD9, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if m in (0xC0, 0xC1):
            return 'baseline'
        if m in (0xC2, 0xC3, 0xC9, 0xCA):
            return 'progressive'
        i += 2 + ((d[i + 2] << 8) | d[i + 3])
    return 'unknown'


def flac_pic(path):
    with open(path, 'rb') as f:
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
                q += 4 + int.from_bytes(b[q:q + 4], 'big')
                q += 4 + int.from_bytes(b[q:q + 4], 'big') + 16
                n = int.from_bytes(b[q:q + 4], 'big')
                return b[q + 4:q + 4 + n]
            if last:
                return None


def mp3_pic(path):
    with open(path, 'rb') as f:
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


def main(argv):
    root = argv[0] if argv else DEFAULT
    if not os.path.isdir(root):
        print('no such folder: %s' % root)
        return 2

    sized = suspect = checked = 0
    art = prog = 0
    bad_size, bad_art = [], []

    for dp, _, fns in os.walk(root):
        for fn in sorted(fns):
            ext = os.path.splitext(fn)[1].lower()
            if ext not in ('.mp3', '.flac'):
                continue
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root)
            total = os.path.getsize(p)

            if ext == '.mp3':
                sized += 1
                d = mp3_declared(p)
                if d:
                    checked += 1
                    real = d[0] + d[1]
                    if total > real + real * TOLERANCE:
                        suspect += 1
                        bad_size.append((rel, real, total, total / real))

            pic = flac_pic(p) if ext == '.flac' else mp3_pic(p)
            if pic:
                art += 1
                if jpeg_mode(pic) != 'baseline':
                    prog += 1
                    bad_art.append(rel)

    print('%s' % root)
    print()
    print('directory-size cross-check (the core\'s own test)')
    print('  %d MP3s, %d declare a BYTES field and were checked' % (sized, checked))
    if bad_size:
        print('  %d WOULD RAISE "! FILE SIZE WRONG":' % suspect)
        for rel, real, tot, r in bad_size:
            print('    %-54s real %d, directory %d (%.2fx)'
                  % (rel[:54], real, tot, r))
        print('  -> run chkdsk on the card, then recopy the files listed')
    else:
        print('  no mismatches')
    print()
    print('cover art')
    print('  %d files carry a picture' % art)
    if bad_art:
        print('  %d are PROGRESSIVE and cannot be decoded (panel shows '
              '"PROG. JPEG"):' % prog)
        for rel in bad_art:
            print('    %s' % rel[:66])
        print('  -> re-encode baseline; tools/host/jpeg_check.py confirms')
    else:
        print('  all baseline')
    print()
    print('CARD OK' if not (bad_size or bad_art) else 'ISSUES ABOVE')
    return 1 if (bad_size or bad_art) else 0


sys.exit(main(sys.argv[1:]))
