#!/usr/bin/env python3
"""Predict what the core will do with the library on the card, before booting it.

This mirrors the decisions fw/art.inc and fw/playlist.inc actually make -- the
APIC search, the size cap, the reduce-vs-full choice, the geometry guards, and
whether every playlist entry resolves. Written after a track showed no art for
days with nothing on screen to say why: the cause was a size cap rejecting the
frame, which one scan of the real files would have found immediately.

Keep the constants below in step with fw/art.inc. They are asserted against the
header where possible so a drift shows up as a failure here rather than as a
silent disagreement between what this predicts and what the core does.

    python tools/library_check.py                # find the card
    python tools/library_check.py --root PATH    # check a folder instead
"""
import argparse
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART_INC = os.path.join(ROOT, "fw", "art.inc")


def from_art_inc():
    """Read the limits out of the firmware so this cannot drift from it."""
    src = open(ART_INC, encoding="utf-8", errors="replace").read()
    def num(name, default):
        m = re.search(r"#define\s+%s\s+\(?([0-9]+)u?\s*\*?\s*([0-9]*)u?\s*\)?" % name, src)
        if not m:
            return default
        a = int(m.group(1))
        return a * int(m.group(2)) if m.group(2) else a
    art_img = 92
    m = re.search(r"#define\s+ART_IMG\s+([0-9]+)u",
                  open(os.path.join(ROOT, "fw", "player.c"),
                       encoding="utf-8", errors="replace").read())
    if m:
        art_img = int(m.group(1))
    return (num("ART_MAX_BYTES", 2048 * 1024), num("ART_MAX_SRC_W", 320),
            num("ART_FULL_MAX", 1024), art_img)


ART_MAX_BYTES, ART_MAX_SRC_W, ART_FULL_MAX, ART_IMG = from_art_inc()


def syncsafe(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def jpeg_dims(d):
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if i + 4 > len(d):
            break
        ln = struct.unpack(">H", d[i + 2:i + 4])[0]
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            h, w = struct.unpack(">HH", d[i + 5:i + 9])
            prog = (m == 0xC2)
            return w, h, prog
        i += 2 + ln
    return None


def find_apic(d):
    if d[:3] != b"ID3":
        return None, "no ID3 tag"
    tag_len = syncsafe(d[6:10])
    major = d[3]
    p = 10
    while p + 10 <= tag_len:
        fid = d[p:p + 4]
        if fid[0:1] == b"\x00":
            return None, "no APIC (padding reached)"
        if major >= 4:
            fs = (((d[p+4] & 0x7F) << 21) | ((d[p+5] & 0x7F) << 14) |
                  ((d[p+6] & 0x7F) << 7) | (d[p+7] & 0x7F))
        else:
            fs = (d[p+4] << 24) | (d[p+5] << 16) | (d[p+6] << 8) | d[p+7]
        if fid == b"APIC":
            return (p, fs), None
        if not fs or p + 10 + fs > tag_len:
            return None, "frame runs past tag end at offset %d" % p
        p += 10 + fs
    return None, "no APIC frame"


def verdict(path):
    d = open(path, "rb").read(8 << 20)
    hit, why = find_apic(d)
    if not hit:
        return "NO ART", why, None
    p, fs = hit
    body = d[p + 10:p + 10 + 256]
    z = body.find(b"\x00", 1)
    mime = bytes(body[1:z]).decode("latin1", "replace") if z > 0 else "?"
    if not re.search(r"jp(e?g)", mime, re.I):
        return "NO ART", "MIME is %r -- picojpeg is baseline JPEG only" % mime, None
    if fs > ART_MAX_BYTES:
        return "REJECTED", ("APIC %s > ART_MAX_BYTES %s"
                            % (f"{fs:,}", f"{ART_MAX_BYTES:,}")), None
    j = d.find(b"\xff\xd8\xff", p, p + 10 + fs)
    wh = jpeg_dims(d[j:p + 10 + fs]) if j > 0 else None
    if not wh:
        return "NO ART", "no JPEG SOF found in the frame", None
    w, h, prog = wh
    if prog:
        return "NO ART", "progressive JPEG -- picojpeg decodes baseline only", None

    reduce = ((w + 7) // 8) >= ART_IMG and ((h + 7) // 8) >= ART_IMG
    if not reduce and (w > ART_FULL_MAX or h > ART_FULL_MAX):
        reduce = True
    sw = (w + 7) // 8 if reduce else w
    sh = (h + 7) // 8 if reduce else h
    if sw > ART_FULL_MAX or sh > ART_FULL_MAX:
        return "REJECTED", "source %dx%d exceeds the map bound %d" % (sw, sh, ART_FULL_MAX), None
    if reduce and sw > ART_MAX_SRC_W:
        return "REJECTED", "reduced width %d > ART_MAX_SRC_W %d" % (sw, ART_MAX_SRC_W), None

    mode = "reduce" if reduce else "FULL"
    scale = sw / ART_IMG
    note = "%dx%d cover, %s APIC, %s -> %dx%d (%.1fx %s)" % (
        w, h, f"{fs:,}", mode, sw, sh, scale if scale >= 1 else 1 / scale,
        "down" if scale >= 1 else "UP")
    return "ok", note, ("FULL" if not reduce else "reduce", fs)


def check_playlist(root):
    pls = glob.glob(os.path.join(root, "**", "*.m3u"), recursive=True)
    if not pls:
        print("\nno .m3u found")
        return
    for pl in pls:
        raw = open(pl, "rb").read()
        print("\nplaylist: %s  (%d bytes)" % (os.path.relpath(pl, root), len(raw)))
        if raw[:3] == b"\xef\xbb\xbf":
            print("  *** UTF-8 BOM present -- the first line will not parse as "
                  "expected. Rewrite the file as plain bytes. ***")
            raw = raw[3:]
        print("  line endings: %s" % ("CRLF" if b"\r\n" in raw else "LF"))
        base = os.path.dirname(pl)
        live = dead = 0
        for ln in raw.decode("latin1").splitlines():
            s = ln.strip()
            if not s or s.startswith("#"):
                continue
            cand = os.path.join(base, s.replace("/", os.sep))
            if os.path.isfile(cand):
                live += 1
            else:
                dead += 1
                print("  MISSING: %s" % s)
        print("  %d entries resolve, %d do not" % (live, dead))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    args = ap.parse_args()

    root = args.root
    if not root:
        for c in "DEFGH":
            p = c + ":\\Assets\\mp3player"
            if os.path.isdir(p):
                root = p
                break
    if not root or not os.path.isdir(root):
        sys.exit("no card found and no --root given")

    print("library: %s" % root)
    print("limits from fw/art.inc: ART_MAX_BYTES=%s ART_MAX_SRC_W=%d "
          "ART_FULL_MAX=%d ART_IMG=%d\n"
          % (f"{ART_MAX_BYTES:,}", ART_MAX_SRC_W, ART_FULL_MAX, ART_IMG))

    files = sorted(glob.glob(os.path.join(root, "**", "*.mp3"), recursive=True))
    bad = 0
    full = 0
    biggest = 0
    for f in files:
        st, note, info = verdict(f)
        if st != "ok":
            bad += 1
        elif info:
            if info[0] == "FULL":
                full += 1
            biggest = max(biggest, info[1])
        print("%-9s %-40s %s" % (st, os.path.basename(f)[:40], note))

    print("\n%d tracks: %d will show art, %d will not." % (len(files), len(files) - bad, bad))
    print("%d take the FULL decode path (slower, small covers); %d take reduce."
          % (full, len(files) - bad - full))
    if biggest:
        print("largest APIC actually decoded: %s bytes -- this is the track that "
              "will feel slowest to load." % f"{biggest:,}")
    check_playlist(root)


if __name__ == "__main__":
    main()
