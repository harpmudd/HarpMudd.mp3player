"""Record what is actually on the SD card, so a before/after comparison is a
measurement rather than a recollection.

    python tools/card_snapshot.py before
    ... run the probe on hardware, remount the card ...
    python tools/card_snapshot.py after
    python tools/card_snapshot.py diff

Two things this does that eyeballing the folder does not.

It hashes, WHOLE FILES by default. A file can be extended and still report a
plausible size, and it can be rewritten in place at the same size; only content
settles it. An earlier draft hashed just the first and last 1 MB, on the theory
that damage lands at the tail -- and a test that planted "SPM3" at offset
5,505,024, exactly where the third event actually found it, came back "nothing
changed". The one signature that matters most sits in the middle of a file.
--quick restores the edges-only mode; do not use it for this test.

It prints sizes in HEX. The three recorded damage events were 21,037,825 /
21,365,505 / 20,382,465, which look unrelated in decimal and are 0x01410301 /
0x01460301 / 0x01370301 in hex -- the same low 16 bits three times. That
constant is the strongest lead there is, and reading the numbers in decimal is
what hid it for three events. Never look at a size on this card in decimal.

The drive letter moves between D: and E: depending on what else is mounted, so
the card is found by looking for the core rather than by assuming a letter.
"""

import argparse
import hashlib
import json
import os
import string
import sys
import time

MARKERS = ("Assets/mp3player", "Cores/HarpMudd.Mp3Player")
HASH_EDGE = 1 << 20          # bytes hashed at each end unless --full
STATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_snapshots")


def find_card(wait_s):
    """A drive holding this core. Waits rather than assuming a letter."""
    deadline = time.time() + wait_s
    seen = set()
    while True:
        for letter in string.ascii_uppercase:
            root = letter + ":\\"
            if not os.path.isdir(root):
                continue
            try:
                if all(os.path.isdir(os.path.join(root, m.replace("/", os.sep)))
                       for m in MARKERS):
                    return root
            except OSError:
                continue        # a drive that vanished mid-scan
            seen.add(letter)
        if time.time() >= deadline:
            return None
        print("  waiting for the card to mount...", flush=True)
        time.sleep(2.0)


def digest(path, size, quick):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        if not quick or size <= 2 * HASH_EDGE:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
            return h.hexdigest(), "full"
        h.update(f.read(HASH_EDGE))
        f.seek(-HASH_EDGE, os.SEEK_END)
        h.update(f.read(HASH_EDGE))
    return h.hexdigest(), "edges"


def scan(root, quick):
    out = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        if os.path.basename(dirpath).lower() == "system volume information":
            continue
        for name in sorted(filenames):
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            try:
                size = os.path.getsize(p)
                d, mode = digest(p, size, quick)
            except OSError as e:
                out[rel] = {"error": str(e)}
                continue
            out[rel] = {"size": size, "sha": d, "hash": mode}
    return out


def hx(n):
    return "0x%08X" % n if n < (1 << 32) else "0x%X" % n


def report_size(n):
    return "%s  %s  (low16 0x%04X)" % (format(n, ","), hx(n), n & 0xFFFF)


def cmd_snap(args, label):
    root = args.root or find_card(args.wait)
    if not root:
        sys.exit("no card found: no drive holds both %s" % " and ".join(MARKERS))
    print("card: %s" % root)
    files = scan(root, args.quick)
    os.makedirs(STATE, exist_ok=True)
    path = os.path.join(STATE, label + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"root": root, "when": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "quick": args.quick, "files": files}, f, indent=1, sort_keys=True)
    mp3s = [k for k in files if k.lower().endswith(".mp3")]
    print("%s: %d files (%d mp3) -> %s" % (label, len(files), len(mp3s), path))
    for k in sorted(mp3s):
        e = files[k]
        if "size" in e:
            print("   %-44s %s" % (k[-44:], report_size(e["size"])))
    return 0


def cmd_diff(args):
    try:
        a = json.load(open(os.path.join(STATE, "before.json"), encoding="utf-8"))
        b = json.load(open(os.path.join(STATE, "after.json"), encoding="utf-8"))
    except OSError as e:
        sys.exit("need both snapshots first (%s)" % e)

    fa, fb = a["files"], b["files"]
    changed = grown = 0

    for k in sorted(set(fa) | set(fb)):
        ea, eb = fa.get(k), fb.get(k)
        if ea is None:
            print("ADDED    %s  %s" % (k, report_size(eb.get("size", 0))))
            changed += 1
        elif eb is None:
            print("REMOVED  %s" % k)
            changed += 1
        elif ea.get("size") != eb.get("size"):
            d = eb["size"] - ea["size"]
            print("RESIZED  %s" % k)
            print("   before %s" % report_size(ea["size"]))
            print("   after  %s" % report_size(eb["size"]))
            print("   delta  %+d  (%s, %+.1f KB)" % (d, hx(abs(d)), d / 1024.0))
            changed += 1
            if d > 0:
                grown += 1
        elif ea.get("sha") != eb.get("sha"):
            print("REWRITTEN %s  (same size %s)" % (k, report_size(ea["size"])))
            changed += 1

    print()
    if not changed:
        print("VERDICT: nothing on the card changed.")
        print("One 0184 write left every other file byte-identical. If the write")
        print("itself reported success, the command is usable and the blocker")
        print("was never 0184 in isolation -- look next at what ELSE ran during")
        print("the three damage events.")
        return 0

    print("VERDICT: %d file(s) changed, %d grew." % (changed, grown))
    if grown:
        print("A single 32-byte write at offset 0 extended a file. 0184 is")
        print("unusable by this core: settings must never write the card, and")
        print("settings.bin stays a hand-edited preferences file.")
        print("Record the new sizes in hex above -- if the low 16 bits are 0301")
        print("a fourth time, that constant is the thread to pull.")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["before", "after", "diff"])
    ap.add_argument("--quick", action="store_true",
                    help="hash only the first and last 1 MB. MISSES an in-place "
                         "edit mid-file, which is the signature this test is "
                         "looking for -- not for use here")
    ap.add_argument("--wait", type=float, default=60.0,
                    help="seconds to wait for the card to mount (default 60)")
    ap.add_argument("--root", default=None,
                    help="scan this folder instead of auto-detecting the card")
    args = ap.parse_args()
    sys.exit(cmd_diff(args) if args.mode == "diff" else cmd_snap(args, args.mode))


if __name__ == "__main__":
    main()
