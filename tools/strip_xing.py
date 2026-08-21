#!/usr/bin/env python3
"""Write a copy of an .mp3 with its Xing/Info/VBRI frame REMOVED.

The headerless-MP3 path is the one caveat left in the v1.4.0 stutter fix: those
files have no duration source, so they still probe during playback and may tic.
Testing that needs a file with no frame-count header, and the three originals
that had one (Stockholm Syndrome 256, Stone Temple Pilots 128, Widespread Panic
160) are no longer on the card.

Making one is more reliable than finding one. The frame-count header lives in a
single silent MPEG frame at the head of the audio; dropping that whole frame
leaves a valid CBR stream that decodes identically and reports no duration --
exactly the shape being tested.

NEVER writes in place. Source file is opened read-only and a new path is
written, because the one thing this project has actually damaged is the user's
music library.

    python tools/strip_xing.py in.mp3 out.mp3
"""
import sys
from pathlib import Path

BITRATE_V1L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
SAMPRATE_V1 = [44100, 48000, 32000, 0]


def id3_len(b):
    if len(b) < 10 or b[:3] != b"ID3":
        return 0
    n = ((b[6] & 0x7F) << 21) | ((b[7] & 0x7F) << 14) | ((b[8] & 0x7F) << 7) | (b[9] & 0x7F)
    return n + 10


def frame_at(b, i):
    """(length, has_count_header) for the MPEG-1 L3 frame at i, or None."""
    if i + 4 > len(b) or b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
        return None
    ver, layer = (b[i + 1] >> 3) & 3, (b[i + 1] >> 1) & 3
    if ver != 3 or layer != 1:
        return None
    br = BITRATE_V1L3[(b[i + 2] >> 4) & 0xF]
    sr = SAMPRATE_V1[(b[i + 2] >> 2) & 3]
    if not br or not sr:
        return None
    pad = (b[i + 2] >> 1) & 1
    length = (144 * br * 1000) // sr + pad
    tag = b[i:i + length]
    has = (b"Xing" in tag) or (b"Info" in tag) or (b"VBRI" in tag)
    return length, has


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    if dst.resolve() == src.resolve():
        print("refusing to write over the source")
        return 1
    b = src.read_bytes()

    off = id3_len(b)
    # Walk to the first real frame; the tag length can be slightly off.
    while off < len(b) - 4:
        f = frame_at(b, off)
        if f:
            break
        off += 1
    else:
        print("no MPEG-1 Layer III frame found")
        return 1

    length, has = f
    if not has:
        print(f"{src.name}: already headerless -- copying unchanged")
        out = b
    else:
        print(f"{src.name}: dropping {length}-byte count header at offset {off}")
        out = b[:off] + b[off + length:]

    dst.write_bytes(out)
    print(f"wrote {dst}  ({len(out)} bytes, was {len(b)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
