#!/usr/bin/env python3
"""Copy the publishable files into a checkout of the public repo. Nothing else.

This exists so that publishing less is STRUCTURAL rather than a matter of
remembering. Two remotes on one working copy means a single mistyped push
distributes everything, permanently and irrevocably, and no amount of care
makes that safe. So the public checkout is a separate directory, this script
is the only thing that writes to it, and it works from an ALLOWLIST.

Allowlist, not denylist, on purpose: a directory added to the project next
year is excluded by default rather than published by default. Getting that
backwards is how these things leak.

    python tools/publish.py <public-checkout>            # report only
    python tools/publish.py <public-checkout> --apply    # actually copy

It NEVER commits and NEVER pushes. It copies files and prints what it did;
committing is a decision a person makes, looking at `git status`.

Before copying anything it checks the licence obligations that distributing a
binary carries, because those do not go away when the source stops being
published -- see LICENCE GATES below.
"""
import filecmp
import hashlib
import os
import shutil
import sys

# --------------------------------------------------------------------------
# What may be published. Paths are relative to the project root. A directory
# entry publishes the files directly inside it, not its subdirectories, unless
# it ends with '/**'.
ALLOW = [
    'dist/**',                       # the shipped artifacts: this is the product
    'README.md',
    'CHANGELOG.md',
    'LICENSE',
    '.github/workflows/release.yml', # the release automation runs in the public repo
    'third_party/font/OFL.txt',      # licence texts must travel with the binary
    'third_party/unifont/OFL-1.1.txt',
]

# Files that must never appear in a public checkout whatever else happens.
# Belt and braces: these should already be excluded by the allowlist.
# NOTE: '.rom' is NOT here. In the arcade ports of this workspace a .rom is
# copyrighted game data and must never be published; in this project it is the
# core's own firmware and is the main thing being shipped. Copying that list
# across without thinking is exactly the mistake this comment exists to stop --
# the gate caught it on the first run.
FORBID_EXT = {'.mp3', '.flac', '.m4a', '.wav', '.ogg', '.lrc',
              '.sof', '.qar', '.stp'}
FORBID_NAME = {'snapshot.json', 'jpeg_bench_baseline.json'}

# --------------------------------------------------------------------------
# LICENCE GATES. Shipping a binary does not end these obligations, and the one
# that matters is Helix: RPSL 1.0 requires a prominent notice saying where its
# source can be obtained. The others need their licence text to travel along.
# Each entry is (description, substrings that must ALL appear in README.md).
LICENCE_CHECKS = [
    ('Helix / RPSL source-availability notice',
     ['RPSL', 'helixcommunity.org']),
    ('VexRiscv attribution', ['VexRiscv']),
    ('picojpeg attribution', ['picojpeg']),
    ('Inter / OFL attribution', ['Inter', 'Open Font License']),
    ('Unifont attribution', ['Unifont']),
    ('agg23 attribution', ['agg23']),
]
LICENCE_FILES = ['third_party/font/OFL.txt', 'third_party/unifont/OFL-1.1.txt']

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def expand(root):
    """Every file the allowlist permits, as paths relative to root."""
    out = []
    for entry in ALLOW:
        if entry.endswith('/**'):
            base = os.path.join(root, entry[:-3])
            if not os.path.isdir(base):
                continue
            for dp, _, fns in os.walk(base):
                for fn in sorted(fns):
                    full = os.path.join(dp, fn)
                    out.append(os.path.relpath(full, root).replace('\\', '/'))
        else:
            if os.path.isfile(os.path.join(root, entry)):
                out.append(entry)
    return sorted(set(out))


def licence_gate():
    """Refuse to publish if the obligations are not visibly met."""
    bad = []
    try:
        readme = open(os.path.join(ROOT, 'README.md'),
                      encoding='utf-8', errors='replace').read()
    except OSError:
        return ['README.md is missing -- every attribution lives in it']
    for what, needles in LICENCE_CHECKS:
        missing = [n for n in needles if n not in readme]
        if missing:
            bad.append('%s -- README.md lacks %s'
                       % (what, ', '.join(repr(m) for m in missing)))
    for f in LICENCE_FILES:
        if not os.path.isfile(os.path.join(ROOT, f)):
            bad.append('%s is missing and must ship with the binary' % f)
    return bad


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 16), b''):
            h.update(b)
    return h.hexdigest()


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    dest = os.path.abspath(argv[0])
    apply_ = '--apply' in argv

    if not os.path.isdir(dest):
        print('not a directory: %s' % dest)
        return 2
    if os.path.abspath(dest) == ROOT:
        print('REFUSED: destination is the development repo itself')
        return 2
    if not os.path.isdir(os.path.join(dest, '.git')):
        print('REFUSED: %s is not a git checkout' % dest)
        print('Point this at a clone of the PUBLIC repo.')
        return 2

    problems = licence_gate()
    if problems:
        print('LICENCE GATE FAILED -- nothing copied')
        for p in problems:
            print('  %s' % p)
        print()
        print('Distributing a binary does not end these obligations.')
        return 1
    print('licence gate: %d checks passed' % (len(LICENCE_CHECKS) + len(LICENCE_FILES)))

    files = expand(ROOT)
    for rel in files:
        ext = os.path.splitext(rel)[1].lower()
        if ext in FORBID_EXT or os.path.basename(rel) in FORBID_NAME:
            print('REFUSED: allowlist would publish %s' % rel)
            return 1

    print('%d files permitted by the allowlist' % len(files))
    print()

    new = changed = same = 0
    for rel in files:
        src = os.path.join(ROOT, rel)
        dst = os.path.join(dest, rel)
        if not os.path.exists(dst):
            state, new = 'NEW    ', new + 1
        elif sha(src) != sha(dst):
            state, changed = 'CHANGED', changed + 1
        else:
            same += 1
            continue
        print('  %s %s' % (state, rel))
        if apply_:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)

    print()
    print('%d new, %d changed, %d already identical' % (new, changed, same))

    # Anything in the public checkout that this script did not put there is
    # reported, never deleted. Source frozen from an earlier release lives
    # there legitimately, and removing it would be a conspicuous single event.
    permitted = set(files)
    strays = []
    for dp, dns, fns in os.walk(dest):
        dns[:] = [d for d in dns if d != '.git']
        for fn in fns:
            rel = os.path.relpath(os.path.join(dp, fn), dest).replace('\\', '/')
            if rel not in permitted:
                strays.append(rel)
    if strays:
        print()
        print('%d file(s) in the public checkout are not on the allowlist.'
              % len(strays))
        print('Left alone -- they are not touched or removed by this script.')
        for rel in sorted(strays)[:10]:
            print('    %s' % rel)
        if len(strays) > 10:
            print('    ... and %d more' % (len(strays) - 10))

    print()
    if apply_:
        print('Copied. Nothing committed and nothing pushed -- review')
        print('`git status` in the public checkout and commit it yourself.')
    else:
        print('Report only. Re-run with --apply to copy.')
    return 0


sys.exit(main(sys.argv[1:]))
