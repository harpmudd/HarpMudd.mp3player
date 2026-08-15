#!/usr/bin/env python3
"""Writes a playlist.m3u into every album folder of a music library.

For the common Artists/Album/tracks layout. Point it at the top of the tree and
it drops one playlist per folder that contains audio, listing that folder's
tracks in order -- so an album becomes a playlist without anyone typing one.

WHY THIS EXISTS RATHER THAN THE CORE DOING IT: APF gives a core no way to list
a directory. 0192 opens a file BY NAME, so the core can only open something it
already knows the name of, which is exactly what a playlist provides. The
enumeration has to happen somewhere that can see the filesystem -- here.

Track order is natural: a leading track number sorts numerically, so "2" comes
before "10" rather than after it. Failing that, plain name order.

    python make_album_playlists.py "D:/Assets/mp3player/common"
    python make_album_playlists.py "D:/Music" --name album.m3u --dry-run
"""
import argparse
import os
import re
import sys

AUDIO = ('.mp3', '.flac')


def sort_key(name):
    """Natural sort: digit runs compare as numbers, everything else as text."""
    parts = re.split(r'(\d+)', name.lower())
    return [int(p) if p.isdigit() else p for p in parts]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root', help='top of the music tree')
    ap.add_argument('--name', default='playlist.m3u',
                    help='playlist filename to write (default: playlist.m3u)')
    ap.add_argument('--dry-run', action='store_true',
                    help='report what would be written, change nothing')
    ap.add_argument('--force', action='store_true',
                    help='overwrite an existing playlist (default: skip it)')
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        sys.exit('not a folder: %s' % args.root)

    written = skipped = empty = 0
    for folder, dirs, files in os.walk(args.root):
        dirs.sort(key=sort_key)

        tracks = [f for f in files if f.lower().endswith(AUDIO)]
        if not tracks:
            empty += 1
            continue
        tracks.sort(key=sort_key)

        out = os.path.join(folder, args.name)
        rel = os.path.relpath(folder, args.root)
        if os.path.exists(out) and not args.force:
            print('skip  %-58s (playlist exists)' % rel[:58])
            skipped += 1
            continue

        if args.dry_run:
            print('would %-58s %3d tracks' % (rel[:58], len(tracks)))
        else:
            # Bare filenames, not paths. The core resolves a playlist entry
            # against the folder the PLAYLIST itself was opened from, so an
            # album playlist sitting beside its tracks needs nothing more --
            # and the same folder still works if it is moved or renamed.
            with open(out, 'w', newline='\n', encoding='utf-8') as f:
                f.write('\n'.join(tracks) + '\n')
            print('wrote %-58s %3d tracks' % (rel[:58], len(tracks)))
        written += 1

    print('\n%d playlist%s %s, %d skipped, %d folders had no audio'
          % (written, '' if written == 1 else 's',
             'would be written' if args.dry_run else 'written', skipped, empty))
    if written and not args.dry_run:
        print('Load any one of them from the core menu with Load Playlist.')


if __name__ == '__main__':
    main()
