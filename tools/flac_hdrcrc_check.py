#!/usr/bin/env python3
"""Checks that the byte-aligned + CRC-8 frame header in fw/flac.c ACCEPTS every
real frame of a file.

The danger in adding a CRC check is not that it misses a false sync -- it is
that it rejects a GOOD one. A header parse that is subtly wrong about the
length it covers would fail on some frames and not others, and the symptom
would be intermittent dropouts in normal playback, far from the seek code that
motivated the change.

So this replaces flac_verify's frame_header with a model of the C version --
same alignment rule, same byte-by-byte accumulation, same CRC over exactly the
header bytes before the CRC byte -- and decodes whole files with it. If every
frame parses and the sample count still matches with zero resync scans, the
check accepts real frames. tools/flac_verify.py separately confirms the audio
those frames produce is bit-exact.
"""
import sys, os
import flac_verify
from flac_verify import verify

BLK_TAB = flac_verify.BLK_TAB


def crc8(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


rejected = []          # (frame_index, reason) for anything the CRC turned away
accepted = [0]


def frame_header(b, cap):
    """Models frame_header()/parse_frame_header() in fw/flac.c."""
    tries = 0
    while True:
        if b.n < 15:
            b._fill(15)

        # Frames are byte-aligned; a candidate at any other offset is false.
        if b.n & 7:
            drop = b.n & 7
            b.n -= drop
            b.acc &= (1 << b.n) - 1
            continue

        peek = (b.acc >> (b.n - 15)) & 0x7FFF
        if (peek >> 1) == 0x3FFE:
            h = []

            def hbyte():
                h.append(b.bits(8))
                return h[-1]

            hbyte(); hbyte(); hbyte(); hbyte()
            bs_code, sr_code = h[2] >> 4, h[2] & 0x0F
            chmode = h[3] >> 4

            c = hbyte()
            extra = None
            for mask, val, k in ((0x80, 0x00, 0), (0xE0, 0xC0, 1), (0xF0, 0xE0, 2),
                                 (0xF8, 0xF0, 3), (0xFC, 0xF8, 4), (0xFE, 0xFC, 5),
                                 (0xFF, 0xFE, 6)):
                if (c & mask) == val:
                    extra = k
                    break
            if extra is None:
                rejected.append((accepted[0], 'bad utf8 lead'))
                tries += 1
                continue
            for _ in range(extra):
                hbyte()

            bsi = len(h)
            if bs_code == 6:
                hbyte()
            elif bs_code == 7:
                hbyte(); hbyte()
            if sr_code == 12:
                hbyte()
            elif sr_code in (13, 14):
                hbyte(); hbyte()

            stored = b.bits(8)
            if crc8(bytes(h)) != stored:
                rejected.append((accepted[0], 'crc'))
                tries += 1
                if tries > (1 << 20):
                    raise ValueError("lost sync")
                continue

            if bs_code == 6:
                blocksize = h[bsi] + 1
            elif bs_code == 7:
                blocksize = ((h[bsi] << 8) | h[bsi + 1]) + 1
            else:
                blocksize = BLK_TAB[bs_code]
            if not blocksize or blocksize > cap:
                rejected.append((accepted[0], 'blocksize %d' % blocksize))
                tries += 1
                continue

            accepted[0] += 1
            return blocksize, chmode, tries

        b.n -= 8                       # whole byte, not one bit
        b.acc &= (1 << b.n) - 1
        tries += 1
        if tries > (1 << 20):
            raise ValueError("lost sync")


if __name__ == '__main__':
    paths = sys.argv[1:]
    if not paths:
        print("usage: flac_hdrcrc_check.py <file.flac> ...")
        sys.exit(2)

    flac_verify.frame_header = frame_header
    bad = 0
    for path in paths:
        del rejected[:]
        accepted[0] = 0
        print(os.path.basename(path), flush=True)
        try:
            si, frames, samples, digest, scans = verify(path, progress_every=0)
        except Exception as e:
            print("  ERROR %s: %s" % (type(e).__name__, e))
            bad += 1
            continue
        ok = (samples == si['total']) and scans == 0 and not rejected
        print("  %d frames accepted, %d/%d samples, %d rejected"
              % (accepted[0], samples, si['total'], len(rejected)))
        if rejected:
            print("  first rejections: %s" % rejected[:5])
        print("  ==> %s" % ("every real frame accepted" if ok else "FAILED"))
        bad += 0 if ok else 1
    sys.exit(1 if bad else 0)
