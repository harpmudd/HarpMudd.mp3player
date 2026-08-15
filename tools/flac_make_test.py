#!/usr/bin/env python3
"""Generates small, valid FLAC files for the cases the test card does not cover.

The README claims mono, 8-bit and 20-bit FLAC, and none of them had ever been
through the decoder -- the claim rested on reading the code. Worse, every file
on the test card uses a 4608-sample block, which is unusual: the `flac` encoder
defaults to 4096, so the block size the majority of real files use had never
been decoded once.

No encoder is installed and none is needed. FLAC allows VERBATIM subframes --
raw samples, no prediction -- so a conformant file is a header, a frame header
with a CRC-8, and the samples. That exercises exactly the parts that differ per
file: block size, channel count, channel assignment, bit depth, and the
conversion to 16-bit output. It does NOT exercise Rice or LPC decoding, which
the real card files already cover thoroughly.

The MD5 in STREAMINFO is computed properly, so tools/flac_verify.py can check
the decode is bit-exact rather than merely non-crashing.

    python flac_make_test.py OUTDIR
"""
import argparse
import hashlib
import math
import os
import struct


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, value, bits):
        if bits == 0:
            return
        value &= (1 << bits) - 1
        self.acc = (self.acc << bits) | value
        self.n += bits
        while self.n >= 8:
            self.n -= 8
            self.buf.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def write_signed(self, value, bits):
        self.write(value & ((1 << bits) - 1), bits)

    def align(self):
        if self.n:
            self.write(0, 8 - self.n)

    def bytes(self):
        assert self.n == 0, "not byte aligned"
        return bytes(self.buf)


def crc8(data):
    c = 0
    for x in data:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def crc16(data):
    c = 0
    for x in data:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def utf8_number(n):
    """FLAC's UTF-8-style coded number. Frame counts here stay under 128, but
    the general form is written out so a longer file cannot silently break."""
    if n < 0x80:
        return bytes([n])
    out, ranges = [], [(0x800, 2), (0x10000, 3), (0x200000, 4),
                       (0x4000000, 5), (0x80000000, 6)]
    for limit, extra in ranges:
        if n < limit:
            lead_bits = 8 - (extra + 2)
            lead = (0xFF << (8 - (extra + 1))) & 0xFF
            out.append(lead | (n >> (6 * extra)))
            for i in range(extra - 1, -1, -1):
                out.append(0x80 | ((n >> (6 * i)) & 0x3F))
            return bytes(out)
    raise ValueError("frame number too large")


BLOCK_CODE = {256: 8, 512: 9, 1024: 10, 2048: 11, 4096: 12,
              8192: 13, 16384: 14, 32768: 15, 4608: 5, 1152: 3}
RATE_CODE  = {88200: 1, 176400: 2, 192000: 3, 8000: 4, 16000: 5, 22050: 6,
              24000: 7, 32000: 8, 44100: 9, 48000: 10, 96000: 11}


def make_frame(number, samples, bps, rate, blocksize, chmode):
    """One frame, every subframe VERBATIM.

    chmode 1 = two independent channels, 10 = mid/side. Mid/side is included
    because it is what real encoders overwhelmingly choose, and it is the path
    where the side channel carries one EXTRA bit -- an off-by-one there would
    corrupt one channel only, which is exactly the kind of fault a listener
    reports as "sounds odd" rather than "broken".
    """
    hdr = BitWriter()
    hdr.write(0x3FFE, 14)               # sync
    hdr.write(0, 1)                     # reserved
    hdr.write(0, 1)                     # fixed blocksize -> frame number
    hdr.write(BLOCK_CODE[blocksize], 4)
    hdr.write(RATE_CODE[rate], 4)
    hdr.write(chmode, 4)
    hdr.write(0, 3)                     # sample size: take it from STREAMINFO
    hdr.write(0, 1)                     # reserved
    for b in utf8_number(number):
        hdr.write(b, 8)
    head = hdr.bytes()
    head += bytes([crc8(head)])

    body = BitWriter()
    for b in head:
        body.write(b, 8)

    nch = 1 if chmode == 0 else 2
    if chmode == 10:                    # mid/side
        chans = [[], []]
        for l, r in samples:
            chans[0].append((l + r) >> 1)
            chans[1].append(l - r)
        depths = [bps, bps + 1]
    elif nch == 1:
        chans = [[s[0] for s in samples]]
        depths = [bps]
    else:
        chans = [[s[0] for s in samples], [s[1] for s in samples]]
        depths = [bps, bps]

    for ch, depth in zip(chans, depths):
        body.write(0, 1)                # zero pad
        body.write(1, 6)                # subframe type 1 = VERBATIM
        body.write(0, 1)                # no wasted bits
        for v in ch:
            body.write_signed(v, depth)

    body.align()
    raw = body.bytes()
    return raw + struct.pack('>H', crc16(raw))


def pcm_md5(samples, bps, nch):
    """FLAC hashes the original PCM: little-endian, ceil(bps/8) bytes each,
    interleaved. Getting this wrong would make every generated file look
    corrupt to a verifier, so it follows the spec rather than convenience."""
    width = (bps + 7) // 8
    h = hashlib.md5()
    buf = bytearray()
    for frame in samples:
        for v in frame[:nch]:
            buf += (v & ((1 << (width * 8)) - 1)).to_bytes(width, 'little')
    h.update(bytes(buf))
    return h.digest()


def tone(n, rate, bps, nch, freq=440.0):
    """A quiet sine, and a different frequency per channel so a swapped or
    duplicated channel is audible rather than subtle."""
    peak = (1 << (bps - 1)) - 1
    amp = int(peak * 0.35)
    out = []
    for i in range(n):
        l = int(amp * math.sin(2 * math.pi * freq * i / rate))
        r = int(amp * math.sin(2 * math.pi * (freq * 1.5) * i / rate))
        out.append((l, r) if nch == 2 else (l,))
    return out


def build(path, bps, rate, nch, blocksize, seconds, chmode=None):
    total = blocksize * max(1, int(round(seconds * rate / blocksize)))
    samples = tone(total, rate, bps, nch)
    if chmode is None:
        chmode = 0 if nch == 1 else 1

    frames = b''
    for i in range(total // blocksize):
        chunk = samples[i * blocksize:(i + 1) * blocksize]
        frames += make_frame(i, chunk, bps, rate, blocksize, chmode)

    si = BitWriter()
    si.write(blocksize, 16)
    si.write(blocksize, 16)
    si.write(0, 24)                     # min frame size: unknown
    si.write(0, 24)                     # max frame size: unknown
    si.write(rate, 20)
    si.write(nch - 1, 3)
    si.write(bps - 1, 5)
    si.write(total, 36)
    for b in pcm_md5(samples, bps, nch):
        si.write(b, 8)
    sib = si.bytes()
    assert len(sib) == 34

    out = b'fLaC' + bytes([0x80]) + len(sib).to_bytes(3, 'big') + sib + frames
    with open(path, 'wb') as f:
        f.write(out)
    return len(out), total


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--seconds', type=float, default=6.0)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)

    cases = [
        # name                       bps rate   ch  block  chmode  why
        ('t1 stereo 16 blk4096.flac', 16, 44100, 2, 4096, 1,
         'the block size most real files use'),
        ('t2 stereo 16 midside.flac', 16, 44100, 2, 4096, 10,
         'mid/side, what encoders actually pick'),
        ('t3 mono 16.flac',           16, 44100, 1, 4096, 0,
         'single channel'),
        ('t4 stereo 8bit.flac',        8, 44100, 2, 4096, 1,
         'to16 shifting UP'),
        ('t5 stereo 20bit.flac',      20, 44100, 2, 4096, 1,
         'to16 shifting down by four'),
        ('t6 stereo 24 blk1152.flac', 24, 44100, 2, 1152, 10,
         'a small block, and 24-bit mid/side'),
    ]

    print('%-28s %-9s %s' % ('file', 'size', 'covers'))
    for name, bps, rate, nch, blk, mode, why in cases:
        size, total = build(os.path.join(a.outdir, name), bps, rate, nch,
                            blk, a.seconds, mode)
        print('%-28s %6.1f KB  %s' % (name, size / 1024.0, why))
    print('\nVerify each with:  python flac_verify.py "<file>"')
