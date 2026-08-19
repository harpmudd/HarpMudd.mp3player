#!/usr/bin/env python3
"""Writes playlists.m3u -- the list of playlists the core searches by name.

WHY THIS EXISTS: the core can remember which playlist you were using, but it
only has one settings word to remember it IN, which holds twelve characters.
That cannot be widened enough to matter: spending every free settings slot
reaches thirty-two characters, and an ordinary album name like

    Crash Test Dummies - God Shuffled His Feet.m3u

is forty-two. So the core stores a HASH of the name instead, and looks the
name up in this file at boot. Length stops mattering.

A hash rather than a line number, which means this file can be reordered,
added to and pruned freely -- entries are matched by name, not by position.
A line number would keep working right up until the file was edited and then
silently load the wrong album.

Without this file the core falls back to the twelve-character stem, which is
exactly how it behaved before, so nothing here is required -- it only lifts
the limit.

    python make_playlist_index.py "D:/Assets/mp3player/common"
    python make_playlist_index.py "D:/Assets/mp3player/common" --dry-run
"""
import argparse
import os
import re
import sys

INDEX_NAME = 'playlists.m3u'
DEFAULT_NAME = 'playlist.m3u'


def sort_key(name):
    parts = re.split(r'(\d+)', name.lower())
    return [int(p) if p.isdigit() else p for p in parts]


def fnv1a_31(name):
    """The core's pl_name_hash(), for reporting collisions before they bite.

    Case-folded because the two sides come from different places -- APF's
    datatable on one, this text file on the other -- and folded to 31 bits
    because APF stores settings words signed and clamps anything that looks
    negative.
    """
    h = 2166136261
    for ch in name.upper():
        h ^= ord(ch) & 0xFF
        h = (h * 16777619) & 0xFFFFFFFF
    h &= 0x7FFFFFFF
    return h or 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root', help='the core folder, e.g. .../mp3player/common')
    ap.add_argument('--dry-run', action='store_true',
                    help='report what would be written, change nothing')
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        sys.exit('not a folder: %s' % args.root)

    found = []
    for folder, dirs, files in os.walk(args.root):
        dirs.sort(key=sort_key)
        for f in sorted(files, key=sort_key):
            if not f.lower().endswith('.m3u'):
                continue
            if f.lower() in (INDEX_NAME, DEFAULT_NAME):
                continue          # the index itself, and the default list
            rel = os.path.relpath(os.path.join(folder, f), args.root)
            found.append(rel.replace('\\', '/'))

    if not found:
        print('no playlists found under %s' % args.root)
        print('nothing to write -- the core falls back to the 12-character '
              'stem, as before.')
        return 0

    # A collision would mean the core reopening the wrong album, silently.
    # 31 bits over a handful of names makes this vanishingly unlikely, which
    # is exactly why it is worth saying so out loud rather than assuming it.
    seen = {}
    clashes = []
    for rel in found:
        base = rel.rsplit('/', 1)[-1]
        h = fnv1a_31(base)
        if h in seen and seen[h] != base:
            clashes.append((seen[h], base))
        seen[h] = base

    width = max(len(f) for f in found)
    for rel in found:
        base = rel.rsplit('/', 1)[-1]
        long = '  <- needs this file to be remembered' if len(base) - 4 > 12 else ''
        print('  %-*s  %08X%s' % (width, rel, fnv1a_31(base), long))

    out = os.path.join(args.root, INDEX_NAME)
    if args.dry_run:
        print('\nwould write %s with %d entr%s'
              % (out, len(found), 'y' if len(found) == 1 else 'ies'))
    else:
        with open(out, 'w', newline='\n', encoding='utf-8') as f:
            f.write('# Playlists on this card. The core searches this file by\n'
                    '# name to reopen the list you last used, which is what\n'
                    '# lets a playlist have a name longer than 12 characters.\n'
                    '# Order does not matter; entries are matched by name.\n')
            f.write('\n'.join(found) + '\n')
        print('\nwrote %s with %d entr%s'
              % (out, len(found), 'y' if len(found) == 1 else 'ies'))

    if clashes:
        print('\nWARNING: two names hash alike, which would reopen the wrong '
              'one. Rename one of each pair:')
        for a, b in clashes:
            print('  %s  <->  %s' % (a, b))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
