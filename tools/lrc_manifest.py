#!/usr/bin/env python3
"""Report which tracks have a .lrc lyrics sidecar, and name the ones that do not.

Same shape as xing_check.py: it inspects a folder and tells you what is there,
so a decision is made against the card's real contents rather than a guess.

THE CONVENTION, and why this file is the thing that fixes it:

    <track>.mp3   ->   <track>.lrc     (same folder, same basename)

Sidecar rather than an in-file tag, for stage 1 of the lyrics work. It needs no
tag parsing at all, it is a different data slot from the streaming one -- which
matters, because a far read on the STREAMING slot corrupts the refills behind
it -- and .lrc is what lyric tools actually emit, timestamps included.

Case: FAT is case-insensitive and APF reports paths in whatever case it likes,
so the firmware must compare case-insensitively. This script does the same, and
warns if two files would collide once case is folded.

NOTE: the firmware cannot read these yet. Lyrics is a ROADMAP entry, not a
feature -- there is no parser and no display. This script exists so the naming
is settled BEFORE anything is built against it, and so a card can be checked
once the feature lands.

    python tools/lrc_manifest.py "D:/Assets/mp3player/common"
    python tools/lrc_manifest.py <path> --missing-only
"""
import argparse
import pathlib
import sys

AUDIO = {".mp3", ".flac"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="the core's common/ folder on the card")
    ap.add_argument("--missing-only", action="store_true",
                    help="list only the tracks with no sidecar")
    a = ap.parse_args()

    root = pathlib.Path(a.root)
    if not root.is_dir():
        sys.exit(f"not a directory: {root}")

    total = have = 0
    for folder in sorted([root] + [d for d in root.iterdir() if d.is_dir()]):
        tracks = sorted(p for p in folder.iterdir()
                        if p.is_file() and p.suffix.lower() in AUDIO)
        if not tracks:
            continue

        # Fold case the way FAT and APF do, so a collision is caught here
        # rather than as a mysterious wrong-lyrics bug on hardware.
        lrc = {}
        for p in folder.iterdir():
            if p.is_file() and p.suffix.lower() == ".lrc":
                k = p.stem.lower()
                if k in lrc:
                    print(f"  !! case collision: {p.name} vs {lrc[k].name}")
                lrc[k] = p

        rows = []
        for t in tracks:
            total += 1
            hit = lrc.get(t.stem.lower())
            if hit:
                have += 1
            if hit and a.missing_only:
                continue
            rows.append(("  ok  " if hit else "  --  ") + t.name)

        if rows:
            rel = folder.relative_to(root) if folder != root else pathlib.Path(".")
            print(f"\n{rel}/   ({len(tracks)} tracks)")
            for r in rows:
                print(r)

    print(f"\n{have} of {total} tracks have a .lrc sidecar "
          f"({total - have} missing).")
    if have < total:
        print("Expected name for a missing one: the track's own filename with "
              ".lrc in place of its extension, in the same folder.")


main()
