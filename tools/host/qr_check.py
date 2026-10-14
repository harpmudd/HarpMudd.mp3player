#!/usr/bin/env python3
"""Generate a real QR from the vendored encoder, at the size the Pocket draws.

Runs third_party/qrcodegen under tools/rv32sim.py -- the TARGET toolchain, not
the host's -- with a payload of plausible scrobbles, and renders the result to
a PNG at the module size the core would use. Point a phone at that PNG and the
whole chain is proven before any firmware UI exists.

    python tools/host/qr_check.py              # a synthetic 14-play session
    python tools/host/qr_check.py 21           # how many plays to pack in

Prints the instruction count too, which is what the export will cost the CPU.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
TC = os.path.join(ROOT, 'toolchain', 'xpack-riscv-none-elf-gcc-15.2.0-1', 'bin')
GCC = os.path.join(TC, 'riscv-none-elf-gcc.exe')
QR = os.path.join(ROOT, 'third_party', 'qrcodegen')
OUT = os.path.join(ROOT, 'docs', 'qr_sample.png')

# Real tracks from the test card, so the length distribution is honest.
TRACKS = [
    ("Golden Brown", "The Stranglers"),
    ("The Only One I Know", "The Charlatans"),
    ("Linda Linda", "The Blue Hearts"),
    ("Sultans of Swing", "Dire Straits"),
    ("Feel Good Inc.", "Gorillaz"),
    ("Soul Meets Body", "Death Cab for Cutie"),
    ("Interstate Love Song", "Stone Temple Pilots"),
    ("Daft Punk Is Playing at My House", "LCD Soundsystem"),
    ("Galaxie", "Blind Melon"),
    ("A Walk", "Bad Religion"),
    ("You're A Wolf", "Sea Wolf"),
    ("Chalk Dust Torture", "Phish"),
    ("Jumpin' Jack Flash", "Ananda Shankar"),
    ("Hypersonic Super-Asterid", "Mandrake Handshake"),
    ("Kuro to Shiro", "Hiroshi Suzuki"),
    ("Goodpeople", "Widespread Panic"),
    ("Black James Dean", "Pigeon"),
    ("Rush", "Big Audio Dynamite II"),
    ("Thin Air", "Widespread Panic"),
    ("Couldn't get it right", "Stockholm Syndrome"),
    ("Narcissus Soaking Wet", "Chris Robinson Brotherhood"),
]


def payload(n):
    """One line per scrobble: unix-time, title, artist. Tab separated."""
    t = 1792000000
    out = ["https://harpmudd.github.io/s#"]
    for i in range(n):
        ti, ar = TRACKS[i % len(TRACKS)]
        out.append("%d\t%s\t%s" % (t + i * 240, ti, ar))
    return ("\n".join(out)).encode('utf-8')


def main(argv):
    n = int(argv[0]) if argv else 14
    data = payload(n)
    blob = os.path.join(HERE, 'qr_payload.bin')
    open(blob, 'wb').write(data)

    elf = os.path.join(HERE, 'qr_harness.elf')
    r = subprocess.run([GCC, '-march=rv32im', '-mabi=ilp32', '-mno-relax',
                        '-Os', '-DNDEBUG', '-ffreestanding', '-nostdlib', '-nostartfiles',
                        '-I', QR, '-I', HERE,
                        '-T', os.path.join(HERE, 'link.ld'), '-o', elf,
                        os.path.join(HERE, 'start.S'),
                        os.path.join(HERE, 'qr_harness.c'),
                        os.path.join(QR, 'qrcodegen.c'), '-lgcc'],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:2000])
        return 2

    out = subprocess.run([sys.executable,
                          os.path.join(ROOT, 'tools', 'rv32sim.py'), elf, blob],
                         capture_output=True, text=True)
    if out.returncode:
        print(out.stderr[:2000])
        return 2
    lines = [l for l in out.stdout.splitlines() if l.strip()]
    if not lines or not lines[0].startswith('SIZE'):
        print('encoder said: %s' % (lines[0] if lines else '(nothing)'))
        return 1

    hdr = lines[0].split()
    size, nbytes, instr = int(hdr[1]), int(hdr[3]), int(hdr[5])
    grid = lines[1:1 + size]
    ver = (size - 17) // 4

    print('%d scrobbles -> %d bytes' % (n, nbytes))
    print('  QR version %d, %d modules' % (ver, size))
    print('  %s instructions (%.0f ms at 40.4 M instr/s)'
          % (format(instr, ','), instr / 40.4e6 * 1000))

    # The Pocket draws 400x360; pick the largest whole module that fits with a
    # 4-module quiet zone, which is what the firmware will do.
    # The quiet zone is 4 modules EACH SIDE and has to come out of the same
    # budget as the code itself -- counting it outside the division is how the
    # first version of this produced a 372 px image for a 360 px screen.
    scale = max(1, min(400, 360) // (size + 8))
    px = (size + 8) * scale
    print('  on the Pocket: %d px/module -> %d x %d px' % (scale, px, px))
    if px > 360:
        print('  WARNING: taller than the 360 px screen')

    try:
        from PIL import Image
    except ImportError:
        print('(install Pillow to write the PNG)')
        return 0
    im = Image.new('1', (px, px), 1)
    for y, row in enumerate(grid):
        for x, c in enumerate(row):
            if c == '#':
                for dy in range(scale):
                    for dx in range(scale):
                        im.putpixel(((x + 4) * scale + dx,
                                     (y + 4) * scale + dy), 0)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    im.save(OUT)
    print('  wrote %s -- scan it with your phone' % OUT)
    return 0


sys.exit(main(sys.argv[1:]))
