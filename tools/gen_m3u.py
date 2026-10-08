#!/usr/bin/env python3
"""Rewrite .m3u playlists with #EXTINF duration lines.

Why this exists
---------------
A headerless MP3 -- no Xing/Info/VBRI frame -- declares no duration, so the
core has to go and MEASURE the file size with far reads on the very slot the
decoder is streaming from. That costs 256 KB of playback before it may even
start (8.2 s at 256 kbps) and drops APF's fragment cache when it does, which is
heard as a stutter. See size_probe_pump() in fw/player.c, which says so.

Extended .m3u carries the duration ahead of each entry:

    #EXTINF:367,Stockholm Syndrome - Couldn't get it right
    MP3s/Stockholm Syndrome - Couldn't get it right.mp3

fw/playlist.inc reads that number and load_track() uses it as track_secs when
the file itself does not say. size_probe_pump() opens with `if (track_secs)
return 0;`, so one line in the playlist retires the whole search: the duration
is on screen with the first painted clock and the stutter never happens.

Durations are computed HERE, from the files, by this module's own parsers -- no
runtime dependency. With `mutagen` installed they are also cross-checked
against it, because a playlist full of confidently wrong numbers is worse than
no playlist at all: for a headerless file nothing downstream can correct it.

Usage
-----
    python tools/gen_m3u.py E:/Assets/mp3player/common            # all .m3u
    python tools/gen_m3u.py E:/Assets/mp3player/common/x.m3u      # just one
    python tools/gen_m3u.py <path> --dry-run                      # show only

An index playlist -- one whose entries are themselves .m3u files, like
playlists.m3u -- is recognised and left alone.
"""
import io
import os
import sys

# MPEG audio frame header tables.
BITRATE_V1_L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
                 320, 0]
BITRATE_V2_L3 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,
                 0]
SAMPLERATE = {3: [44100, 48000, 32000, 0],      # MPEG 1
              2: [22050, 24000, 16000, 0],      # MPEG 2
              0: [11025, 12000, 8000, 0]}       # MPEG 2.5


def id3v2_len(f):
    """Bytes of ID3v2 at the head of the file, 0 if none."""
    f.seek(0)
    h = f.read(10)
    if len(h) < 10 or h[:3] != b'ID3':
        return 0
    size = (h[6] << 21) | (h[7] << 14) | (h[8] << 7) | h[9]
    foot = 10 if (h[5] & 0x10) else 0
    return 10 + size + foot


def id3v1_len(f, total):
    """128 bytes of trailing ID3v1 is NOT audio and must not be counted."""
    if total < 128:
        return 0
    f.seek(total - 128)
    return 128 if f.read(3) == b'TAG' else 0


def mp3_duration(path):
    """Seconds, or None. Exact from a Xing/VBRI frame count, else size/rate."""
    total = os.path.getsize(path)
    with open(path, 'rb') as f:
        start = id3v2_len(f)
        tail = id3v1_len(f, total)

        # The first frame may not sit exactly at the tag boundary.
        f.seek(start)
        buf = f.read(16384)
        off = None
        for i in range(len(buf) - 4):
            if buf[i] != 0xFF or (buf[i + 1] & 0xE0) != 0xE0:
                continue
            ver = (buf[i + 1] >> 3) & 3
            lay = (buf[i + 1] >> 1) & 3
            bri = (buf[i + 2] >> 4) & 15
            sri = (buf[i + 2] >> 2) & 3
            if ver == 1 or lay != 1 or bri in (0, 15) or sri == 3:
                continue        # reserved version, not layer III, bad rate
            off = i
            break
        if off is None:
            return None

        h = buf[off:off + 4]
        ver = (h[1] >> 3) & 3
        bitrate = (BITRATE_V1_L3 if ver == 3 else BITRATE_V2_L3)[(h[2] >> 4) & 15]
        rate = SAMPLERATE[ver][(h[2] >> 2) & 3]
        if not bitrate or not rate:
            return None
        mono = ((h[3] >> 6) & 3) == 3
        spf = 1152 if ver == 3 else 576

        # Xing/Info sits just past the side-information block; VBRI at +32.
        side = (17 if mono else 32) if ver == 3 else (9 if mono else 17)
        xo = off + 4 + side
        frames = None
        if buf[xo:xo + 4] in (b'Xing', b'Info'):
            flags = int.from_bytes(buf[xo + 4:xo + 8], 'big')
            if flags & 1:
                frames = int.from_bytes(buf[xo + 8:xo + 12], 'big')
        elif buf[off + 4 + 32:off + 4 + 36] == b'VBRI':
            v = off + 4 + 32
            frames = int.from_bytes(buf[v + 14:v + 18], 'big')

        if frames:
            return frames * spf / float(rate)

        # Headerless. Exact for CBR, which is what these files are.
        audio = total - (start + off) - tail
        return audio / (bitrate * 1000.0 / 8.0)


def flac_duration(path):
    """Seconds from STREAMINFO, or None."""
    with open(path, 'rb') as f:
        if f.read(4) != b'fLaC':
            return None
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                return None
            last = hdr[0] & 0x80
            btype = hdr[0] & 0x7F
            blen = (hdr[1] << 16) | (hdr[2] << 8) | hdr[3]
            body = f.read(blen)
            if btype == 0 and len(body) >= 18:
                bits = int.from_bytes(body[10:18], 'big')
                rate = (bits >> 44) & 0xFFFFF
                samples = bits & 0xFFFFFFFFF
                if rate and samples:
                    return samples / float(rate)
                return None
            if last:
                return None


def duration(path):
    ext = os.path.splitext(path)[1].lower()
    try:
        if ext == '.mp3':
            return mp3_duration(path)
        if ext == '.flac':
            return flac_duration(path)
    except (OSError, ValueError, IndexError):
        return None
    return None


def reference(path):
    """mutagen's answer, for cross-checking. None when unavailable."""
    try:
        import mutagen
    except ImportError:
        return None
    try:
        m = mutagen.File(path)
        return m.info.length if m is not None else None
    except Exception:
        return None


def title_of(rel):
    return os.path.splitext(os.path.basename(rel))[0]


def is_index(entries):
    """An index playlist lists other playlists, not tracks."""
    return bool(entries) and all(e.lower().endswith('.m3u') for e in entries)


def process(m3u, root, dry):
    raw = io.open(m3u, encoding='utf-8', errors='replace').read()
    lines = raw.splitlines()

    entries = [l.strip() for l in lines
               if l.strip() and not l.strip().startswith('#')]
    if is_index(entries):
        print('%-52s index playlist, left alone' % os.path.basename(m3u)[:52])
        return 0, 0, 0

    out = ['#EXTM3U']
    named = missing = mismatch = 0
    for line in lines:
        s = line.strip()
        if not s:
            continue
        if s.startswith('#'):
            if s.upper().startswith('#EXTM3U') or s.upper().startswith('#EXTINF:'):
                continue                     # regenerated, not carried over
            out.append(s)                    # the author's own comments stay
            continue

        full = os.path.join(root, s.replace('/', os.sep))
        secs = duration(full) if os.path.isfile(full) else None
        if secs is None:
            missing += 1
            out.append(s)                    # no number is better than a wrong one
            note = 'MISSING' if not os.path.isfile(full) else 'unparsed'
            print('    %-62s %s' % (s[:62], note))
            continue

        ref = reference(full)
        if ref is not None and abs(ref - secs) > 1.5:
            mismatch += 1
            print('    %-62s %.1f vs mutagen %.1f  DISAGREE'
                  % (s[:62], secs, ref))

        out.append('#EXTINF:%d,%s' % (int(round(secs)), title_of(s)))
        out.append(s)
        named += 1

    if not dry:
        io.open(m3u, 'w', encoding='utf-8', newline='\r\n').write(
            '\n'.join(out) + '\n')
    print('%-52s %3d named, %d missing, %d disagree'
          % (os.path.basename(m3u)[:52], named, missing, mismatch))
    return named, missing, mismatch


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    target = argv[0]
    dry = '--dry-run' in argv

    if os.path.isdir(target):
        root = target
        files = sorted(os.path.join(target, f) for f in os.listdir(target)
                       if f.lower().endswith('.m3u'))
    else:
        root = os.path.dirname(target)
        files = [target]

    if reference(files[0] if files else '') is None:
        try:
            import mutagen            # noqa: F401
            print('cross-checking against mutagen')
        except ImportError:
            print('mutagen absent -- durations NOT cross-checked')
    print()

    tn = tm = td = 0
    for m in files:
        n, mi, d = process(m, root, dry)
        tn += n
        tm += mi
        td += d
    print()
    print('%d entries named, %d left bare, %d disagreements'
          % (tn, tm, td))
    if dry:
        print('(dry run -- nothing written)')
    return 1 if td else 0


sys.exit(main(sys.argv[1:]))
