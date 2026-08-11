#!/usr/bin/env python3
"""Which .mp3 files carry a Xing/Info/VBRI header, and what the core does without one.

The 1.2x seek defect is file-dependent, and `track_secs` is the suspected
discriminator: it is set ONLY from a frame-count header. With one,
ui_byte_rate() returns an exact constant. Without one it falls through to
meas_rate -- the measured, self-correcting rate that the 2026-08-10
hold-to-seek bug lived in.

This reports which files are on which side, so a failing set can be compared
against a passing one instead of guessed at.

    python tools/xing_check.py D:/Assets/mp3player/common
"""
import struct
import sys
from pathlib import Path

BITRATE_V1L3 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
SAMPRATE_V1 = [44100, 48000, 32000, 0]


def id3_len(b):
    """Length of a leading ID3v2 tag, 0 if absent."""
    if len(b) < 10 or b[:3] != b"ID3":
        return 0
    n = ((b[6] & 0x7F) << 21) | ((b[7] & 0x7F) << 14) | ((b[8] & 0x7F) << 7) | (b[9] & 0x7F)
    return n + 10


def first_frame(b, off):
    """Find the first MPEG-1 Layer III frame header at/after off."""
    for i in range(off, min(len(b) - 4, off + 200000)):
        if b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
            continue
        ver, layer = (b[i + 1] >> 3) & 3, (b[i + 1] >> 1) & 3
        if ver != 3 or layer != 1:                 # MPEG-1 Layer III only
            continue
        br = BITRATE_V1L3[(b[i + 2] >> 4) & 0xF]
        sr = SAMPRATE_V1[(b[i + 2] >> 2) & 3]
        if not br or not sr:
            continue
        pad = (b[i + 2] >> 1) & 1
        chan = (b[i + 3] >> 6) & 3
        return i, br, sr, pad, chan
    return None


def scan(path):
    # Only the head. A frame-count header lives in the FIRST audio frame, so
    # reading whole multi-megabyte files off the card just to look at its first
    # few hundred bytes made this take minutes instead of seconds.
    size = path.stat().st_size
    with path.open("rb") as fh:
        b = fh.read(1 << 20)
    tag = id3_len(b)
    f = first_frame(b, tag)
    if not f:
        return dict(name=path.name, err="no MPEG-1 Layer III frame")
    off, br, sr, pad, chan = f

    # Xing/Info sits at a fixed offset into the first frame, which depends on
    # version and channel mode. VBRI is always 32 bytes in.
    side = 17 if chan == 3 else 32              # mono vs stereo, MPEG-1
    hdr, frames = None, None
    for probe, tagname in ((off + 4 + side, None), (off + 36, "VBRI")):
        if probe + 8 > len(b):
            continue
        magic = b[probe:probe + 4]
        if magic in (b"Xing", b"Info"):
            flags = struct.unpack(">I", b[probe + 4:probe + 8])[0]
            if flags & 1:
                frames = struct.unpack(">I", b[probe + 8:probe + 12])[0]
            hdr = magic.decode()
            break
        if magic == b"VBRI":
            frames = struct.unpack(">I", b[probe + 14:probe + 18])[0]
            hdr = "VBRI"
            break
    secs = round(frames * 1152 / sr) if (frames and sr) else None
    return dict(name=path.name, size=size, kbps=br, hz=sr,
                mode="mono" if chan == 3 else "stereo",
                hdr=hdr or "-", frames=frames, secs=secs)


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "D:/Assets/mp3player/common")
    files = sorted(root.glob("*.mp3"))
    if not files:
        sys.exit("no .mp3 files under %s" % root)

    print("%-44s %-6s %5s %6s %-6s %8s  %s"
          % ("file", "hdr", "kbps", "hz", "mode", "secs", "track_secs"))
    print("-" * 96)
    withhdr = 0
    for p in files:
        r = scan(p)
        if r.get("err"):
            print("%-44s %s" % (r["name"][:44], r["err"]))
            continue
        has = r["hdr"] != "-"
        withhdr += has
        print("%-44s %-6s %5d %6d %-6s %8s  %s"
              % (r["name"][:44], r["hdr"], r["kbps"], r["hz"], r["mode"],
                 r["secs"] if r["secs"] else "?",
                 "set (exact rate)" if has else "ZERO -> meas_rate"))
    print("-" * 96)
    print("%d of %d carry a frame-count header." % (withhdr, len(files)))
    print()
    print("Files reading ZERO take the meas_rate path -- the one the")
    print("hold-to-seek bug lived in. If the 1.2x failures are exactly this")
    print("set, that is the fault located.")


if __name__ == "__main__":
    main()
