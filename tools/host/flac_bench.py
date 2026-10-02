#!/usr/bin/env python3
"""Decode cost and bit-exactness for fw/flac.c, without hardware.

Runs the SHIPPING decoder under tools/rv32sim.py over whole files and reports:

  * instructions per sample, split into the residual pass and the predictor
    pass -- which is the number that says whether optimising the predictor
    loop is worth writing,
  * a CRC32 of the decoded PCM, checked against a Python decode that
    tools/flac_verify.py has already proved against the file's own MD5.

So a change to the decoder is proven over a whole track rather than judged by
ear, and the cost of it is measured rather than assumed.

    python tools/host/flac_bench.py FILE.flac [FILE.flac ...]
    python tools/host/flac_bench.py --no-verify FILE.flac      # cost only
    python tools/host/flac_bench.py --frames=600 FILE.flac     # longer soak
"""
import io
import os
import re
import subprocess
import sys
import zlib
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

TC = os.path.join(ROOT, 'toolchain', 'xpack-riscv-none-elf-gcc-15.2.0-1', 'bin')
GCC = os.path.join(TC, 'riscv-none-elf-gcc.exe')
ELF = os.path.join(HERE, 'flac_bench.elf')


def build(profile=True, frames=200):
    cmd = [GCC, '-march=rv32im', '-mabi=ilp32', '-mno-relax', '-O2',
           '-ffreestanding', '-nostdlib', '-Wall', '-Wextra',
           '-DFLAC_PROFILE=%d' % (1 if profile else 0),
           '-DMAX_FRAMES=%du' % frames,
           '-T', os.path.join(HERE, 'link.ld'),
           os.path.join(HERE, 'start.S'),
           os.path.join(HERE, 'flac_harness.c'),
           os.path.join(ROOT, 'fw', 'flac.c'),
           '-o', ELF,
           # flac.c's bit reader uses __builtin_clzll, which lowers to a libgcc
           # helper on rv32 -- the firmware links it the same way.
           '-lgcc']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit('harness build failed')
    return ELF


def run(elf, path):
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'rv32sim.py'),
                        elf, path], capture_output=True, text=True)
    if r.returncode not in (0, None):
        print(r.stdout + r.stderr)
        raise SystemExit('simulation failed')
    out = {}
    for tok, val in re.findall(r'(\w+)\s+(0x[0-9A-Fa-f]+|\d+)', r.stdout):
        out[tok] = int(val, 0)
    return out, r.stdout


def reference_crc(path, frames=200):
    """CRC32 of the PCM a verified-bit-exact Python decode produces."""
    import flac_verify as fv
    b = fv.Bits(open(path, 'rb').read())
    si = fv.open_stream(b)
    cap = si['maxb']
    ch0, ch1 = [0] * cap, [0] * cap
    crc = 0
    bps, nch = si['bps'], si['ch']
    done = 0
    while done < frames:
        try:
            n, m, _ = fv.frame_header(b, cap)
        except (EOFError, ValueError):
            break
        try:
            fv.subframe(b, n, ch0, bps + (1 if m == 9 else 0))
            if nch == 1:
                out = ch0[:n]
            else:
                fv.subframe(b, n, ch1, bps + (1 if m in (8, 10) else 0))
                out = [0] * (n * 2)
                for i in range(n):
                    a, s = ch0[i], ch1[i]
                    if m == 8:
                        l, r = a, a - s
                    elif m == 9:
                        r, l = s, s + a
                    elif m == 10:
                        mid = (a << 1) | (s & 1)
                        l, r = (mid + s) >> 1, (mid - s) >> 1
                    else:
                        l, r = a, s
                    out[i * 2], out[i * 2 + 1] = l, r
            crc = zlib.crc32(array('h', out).tobytes(), crc)
            b.align()
            b.bits(16)
            done += 1
        except (EOFError, ValueError):
            break
    return crc & 0xFFFFFFFF


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    verify = '--no-verify' not in sys.argv
    frames = 200
    for a in sys.argv[1:]:
        if a.startswith('--frames='):
            frames = int(a.split('=', 1)[1])
    if not args:
        raise SystemExit(__doc__)

    elf = build(profile=True, frames=frames)
    print("%-28s %9s %9s %9s %9s  %s" %
          ('file', 'samples', 'instr/smp', 'residual', 'predict', 'bit-exact'))
    bad = 0
    for path in args:
        vals, raw = run(elf, path)
        if 'samples' not in vals or not vals['samples']:
            print('%-28s  no output:\n%s' % (os.path.basename(path)[:28], raw))
            bad += 1
            continue
        smp = vals['samples']
        tot = vals.get('instr_total', 0)
        res = vals.get('instr_residual', 0)
        pre = vals.get('instr_predict', 0)
        if verify:
            want = reference_crc(path, frames)
            ok = (vals.get('crc32', -1) == want)
            mark = 'yes' if ok else 'NO  (%08X vs %08X)' % (vals.get('crc32', 0), want)
            bad += not ok
        else:
            mark = '-'
        print("%-28s %9d %9.1f %9.1f %9.1f  %s" %
              (os.path.basename(path)[:28], smp, tot / smp, res / smp, pre / smp, mark))

    print('\nresidual and predict are channel 0 only -- channel 1 does the same '
          'work interleaved, so treat them as a ratio, not a total.')
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main())
