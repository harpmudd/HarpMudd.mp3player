"""SUPERSEDED 2026-08-05 -- kept only for reading an old settings.bin.

Settings now persist through interact.json: APF stores them in
/Settings/HarpMudd.Mp3Player/Interact/_core/interact_persist.json and they are
changed from the Pocket's Core Settings menu, or with the buttons. The core no
longer reads settings.bin at all, so editing it here changes nothing.

Read and edit settings.bin on the card, without a hex editor.

    python tools/settings_edit.py                     # show what is stored
    python tools/settings_edit.py --volume 70 --accent SEAFOAM
    python tools/settings_edit.py --repeat all --meter VU --shuffle on

The core reads this file at launch and applies it. It does NOT write it back:
SETTINGS_WRITE is 0 because the 0184 write extended the user's .mp3 files three
times (see tools/settings_probe.md). So until that blocker is answered, this
script IS the persistence mechanism -- changes made with the buttons are
discarded on relaunch, and changes made here survive.

Two rules the format imposes, both enforced below:

  * The file must stay EXACTLY 32 bytes. APF cannot create a file, and the core
    ships one holding the defaults; a different size is a different file as far
    as the slot is concerned.
  * Every field is range-checked against the firmware's real limits, because
    settings_load() silently ignores any field that is out of range. A value of
    99 for the palette would not error, it would just quietly not apply -- which
    looks exactly like "settings are broken".
"""

import argparse
import os
import string
import sys
import time

MARKERS = ("Assets/mp3player", "Cores/HarpMudd.Mp3Player")
REL = os.path.join("Assets", "mp3player", "common", "settings", "settings.bin")

MAGIC = b"SPM3"
VERSION = 1
SIZE = 32

# Byte offsets -- must match the SET_B_* enum in fw/settings.inc.
B_VERSION, B_VOLUME, B_PALETTE = 4, 5, 6
B_REPEAT, B_SHUFFLE, B_ART, B_VIZ = 7, 8, 9, 10

VOL_MAX = 100
ACCENTS = ["AMBER", "LIME", "MINT", "SEAFOAM", "SKY", "INDIGO",
           "LILAC", "BLUSH", "CORAL", "CRIMSON", "GOLD", "CREAM"]
REPEATS = ["OFF", "ALL", "ONE"]
METERS = ["BARS", "WATERFALL", "LEVELS", "SCOPE", "OSCILLOSCOPE", "VU",
          "WAVEFORM", "MIRRORED", "DOTS"]


def find_card(wait_s=30.0):
    deadline = time.time() + wait_s
    while True:
        for letter in string.ascii_uppercase:
            root = letter + ":\\"
            try:
                if os.path.isdir(root) and all(
                        os.path.isdir(os.path.join(root, m.replace("/", os.sep)))
                        for m in MARKERS):
                    return root
            except OSError:
                continue
        if time.time() >= deadline:
            return None
        print("  waiting for the card to mount...", flush=True)
        time.sleep(2.0)


def pick(name, table, what):
    """Accept either the name or the index, case-insensitively."""
    s = str(name).strip().upper()
    if s.isdigit():
        i = int(s)
        if not 0 <= i < len(table):
            sys.exit("%s index %d out of range 0..%d" % (what, i, len(table) - 1))
        return i
    if s not in table:
        sys.exit("unknown %s %r -- choose from: %s"
                 % (what, name, ", ".join(table)))
    return table.index(s)


def show(rec):
    def nameof(table, v):
        return table[v] if 0 <= v < len(table) else "?? (%d)" % v
    print("  version   %d" % rec[B_VERSION])
    print("  volume    %d" % rec[B_VOLUME])
    print("  accent    %s" % nameof(ACCENTS, rec[B_PALETTE]))
    print("  repeat    %s" % nameof(REPEATS, rec[B_REPEAT]))
    print("  shuffle   %s" % ("on" if rec[B_SHUFFLE] else "off"))
    print("  art       %s" % ("shown" if rec[B_ART] else "hidden"))
    print("  meter     %s" % nameof(METERS, rec[B_VIZ]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--volume", type=int, help="0..%d" % VOL_MAX)
    ap.add_argument("--accent", help="name or index: " + ", ".join(ACCENTS))
    ap.add_argument("--repeat", help="off | all | one")
    ap.add_argument("--shuffle", choices=["on", "off"])
    ap.add_argument("--art", choices=["on", "off"])
    ap.add_argument("--meter", help="name or index: " + ", ".join(METERS))
    ap.add_argument("--path", help="settings.bin to edit (default: find the card)")
    args = ap.parse_args()

    path = args.path
    if not path:
        root = find_card()
        if not root:
            sys.exit("no card found: no drive holds both %s" % " and ".join(MARKERS))
        path = os.path.join(root, REL)
    if not os.path.isfile(path):
        sys.exit("not found: %s" % path)

    raw = open(path, "rb").read()
    if len(raw) != SIZE:
        sys.exit("settings.bin is %d bytes, expected exactly %d -- the core "
                 "would reject it" % (len(raw), SIZE))
    if raw[:4] != MAGIC:
        sys.exit("bad magic %r -- not a settings file the core wrote" % raw[:4])
    if raw[B_VERSION] != VERSION:
        sys.exit("record version %d, this script writes version %d -- the core "
                 "ignores a mismatched version rather than misreading it"
                 % (raw[B_VERSION], VERSION))

    rec = bytearray(raw)
    print("%s" % path)
    print("current:")
    show(rec)

    changed = []
    if args.volume is not None:
        if not 0 <= args.volume <= VOL_MAX:
            sys.exit("volume %d out of range 0..%d" % (args.volume, VOL_MAX))
        rec[B_VOLUME] = args.volume
        changed.append("volume")
    if args.accent is not None:
        rec[B_PALETTE] = pick(args.accent, ACCENTS, "accent")
        changed.append("accent")
    if args.repeat is not None:
        rec[B_REPEAT] = pick(args.repeat, REPEATS, "repeat")
        changed.append("repeat")
    if args.shuffle is not None:
        rec[B_SHUFFLE] = 1 if args.shuffle == "on" else 0
        changed.append("shuffle")
    if args.art is not None:
        rec[B_ART] = 1 if args.art == "on" else 0
        changed.append("art")
    if args.meter is not None:
        rec[B_VIZ] = pick(args.meter, METERS, "meter")
        changed.append("meter")

    if not changed:
        print()
        print("nothing to change. Pass --volume / --accent / --repeat /")
        print("--shuffle / --art / --meter to edit, then relaunch the core.")
        return 0

    assert len(rec) == SIZE, "record must stay exactly %d bytes" % SIZE
    with open(path, "wb") as f:
        f.write(bytes(rec))

    print()
    print("wrote %s (%d bytes, unchanged size)" % (", ".join(changed), SIZE))
    print("new:")
    show(bytearray(open(path, "rb").read()))
    print()
    print("Relaunch the core to pick these up. The buttons still will not save")
    print("anything -- see tools/settings_probe.md for why.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
