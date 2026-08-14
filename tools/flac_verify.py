#!/usr/bin/env python3
"""Fast bit-exactness check for the decoder in fw/flac.c.

Same algorithm as tools/flac_ref.py, which mirrors the C line for line. This
one exists only because the readable version is too slow to finish a whole
track in pure Python, and the check is worthless on a fragment: FLAC's
STREAMINFO carries an MD5 of the entire original PCM, so either the whole
stream reproduces it or nothing is proved.

Three changes buy the speed, none of them touching the arithmetic:
  - a 64-bit sliding bit window refilled four bytes at a time,
  - unary run lengths from int.bit_length() instead of a bit-at-a-time loop,
  - PCM accumulated in array('h')/array('i') and hashed once per frame,
    rather than int.to_bytes() per sample.
"""
import sys, os, hashlib
from array import array

BLK_TAB = [0, 192, 576, 1152, 2304, 4608, 0, 0,
           256, 512, 1024, 2048, 4096, 8192, 16384, 32768]
FIXED = [[], [1], [2, -1], [3, -3, 1], [4, -6, 4, -1]]


class Bits:
    __slots__ = ('d', 'p', 'acc', 'n')

    def __init__(self, data):
        self.d, self.p, self.acc, self.n = data, 0, 0, 0

    def _fill(self, k):
        while self.n < k:
            chunk = self.d[self.p:self.p + 4]
            if not chunk:
                raise EOFError
            self.acc = (self.acc << (8 * len(chunk))) | int.from_bytes(chunk, 'big')
            self.n += 8 * len(chunk)
            self.p += len(chunk)

    def bits(self, k):
        if not k:
            return 0
        if self.n < k:
            self._fill(k)
        self.n -= k
        v = (self.acc >> self.n) & ((1 << k) - 1)
        self.acc &= (1 << self.n) - 1
        return v

    def sbits(self, k):
        v = self.bits(k)
        return v - (1 << k) if k and (v >> (k - 1)) & 1 else v

    def unary(self):
        c = 0
        while True:
            if self.n == 0:
                self._fill(1)
            # leading zeros within the window = n - bit_length
            if self.acc == 0:
                c += self.n
                self.n = 0
                continue
            lead = self.n - self.acc.bit_length()
            c += lead
            self.n -= lead + 1                      # consume zeros and the 1
            self.acc &= (1 << self.n) - 1
            return c

    def align(self):
        drop = self.n & 7
        self.n -= drop
        self.acc &= (1 << self.n) - 1


def open_stream(b):
    if b.bits(32) != 0x664C6143:
        raise ValueError("not FLAC")
    si = None
    while True:
        last, typ, ln = b.bits(1), b.bits(7), b.bits(24)
        if typ == 0:
            minb, maxb = b.bits(16), b.bits(16)
            b.bits(24); b.bits(24)
            rate, ch, bps = b.bits(20), b.bits(3) + 1, b.bits(5) + 1
            total = (b.bits(4) << 32) | b.bits(32)
            md5 = bytes(b.bits(8) for _ in range(16))
            for _ in range(34, ln):
                b.bits(8)
            si = dict(minb=minb, maxb=maxb, rate=rate, ch=ch, bps=bps,
                      total=total, md5=md5)
        else:
            for _ in range(ln):
                b.bits(8)
        if last:
            return si


def residual(b, blocksize, order, out, base):
    method = b.bits(2)
    if method > 1:
        raise ValueError("residual method")
    pbits, escape = (5, 31) if method else (4, 15)
    parts = 1 << b.bits(4)
    if blocksize % parts:
        raise ValueError("partition order")
    idx, per = base, blocksize // parts
    bb, un = b.bits, b.unary
    for p in range(parts):
        count = per - (order if p == 0 else 0)
        param = bb(pbits)
        if param == escape:
            raw = bb(5)
            for _ in range(count):
                out[idx] = b.sbits(raw); idx += 1
        else:
            for _ in range(count):
                v = (un() << param) | bb(param)
                out[idx] = -((v >> 1) + 1) if v & 1 else (v >> 1)
                idx += 1


def subframe(b, n, out, bps):
    b.bits(1)
    typ = b.bits(6)
    wasted = b.unary() + 1 if b.bits(1) else 0
    bps -= wasted

    if typ == 0:
        v = b.sbits(bps)
        for i in range(n):
            out[i] = v
    elif typ == 1:
        for i in range(n):
            out[i] = b.sbits(bps)
    elif 8 <= typ <= 12:
        order = typ - 8
        for i in range(order):
            out[i] = b.sbits(bps)
        residual(b, n, order, out, order)
        c = FIXED[order]
        if order == 1:
            for i in range(1, n): out[i] += out[i - 1]
        elif order == 2:
            for i in range(2, n): out[i] += 2 * out[i - 1] - out[i - 2]
        elif order == 3:
            for i in range(3, n):
                out[i] += 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]
        elif order == 4:
            for i in range(4, n):
                out[i] += (4 * out[i - 1] - 6 * out[i - 2]
                           + 4 * out[i - 3] - out[i - 4])
    elif typ >= 32:
        order = typ - 31
        for i in range(order):
            out[i] = b.sbits(bps)
        prec = b.bits(4) + 1
        shift = b.sbits(5)
        if prec == 16 or shift < 0:
            raise ValueError("LPC prec/shift")
        coef = [b.sbits(prec) for _ in range(order)]
        residual(b, n, order, out, order)
        rc = list(reversed(coef))
        for i in range(order, n):
            acc = 0
            w = out[i - order:i]
            for cj, sj in zip(rc, w):
                acc += cj * sj
            out[i] += acc >> shift
    else:
        raise ValueError("subframe type %d" % typ)

    if wasted:
        for i in range(n):
            out[i] <<= wasted


def frame_header(b, cap):
    tries = 0
    while True:
        if b.n < 15:
            b._fill(15)
        peek = (b.acc >> (b.n - 15)) & 0x7FFF
        if (peek >> 1) == 0x3FFE:
            break
        b.n -= 1
        b.acc &= (1 << b.n) - 1
        tries += 1
        if tries > (1 << 22):
            raise ValueError("lost sync")
    b.bits(14); b.bits(1); b.bits(1)
    bs, sr, chmode = b.bits(4), b.bits(4), b.bits(4)
    b.bits(3); b.bits(1)
    c = b.bits(8)
    for mask, val, k in ((0x80, 0x00, 0), (0xE0, 0xC0, 1), (0xF0, 0xE0, 2),
                         (0xF8, 0xF0, 3), (0xFC, 0xF8, 4), (0xFE, 0xFC, 5),
                         (0xFF, 0xFE, 6)):
        if (c & mask) == val:
            extra = k
            break
    for _ in range(extra):
        b.bits(8)
    if bs == 6:
        blocksize = b.bits(8) + 1
    elif bs == 7:
        blocksize = b.bits(16) + 1
    else:
        blocksize = BLK_TAB[bs]
    if sr == 12:
        b.bits(8)
    elif sr in (13, 14):
        b.bits(16)
    b.bits(8)
    if not blocksize or blocksize > cap:
        raise ValueError("blocksize %d" % blocksize)
    return blocksize, chmode, tries


def verify(path, progress_every=200):
    b = Bits(open(path, 'rb').read())
    si = open_stream(b)
    print(f"  {si['bps']}-bit {si['rate']} Hz {si['ch']}ch  blocksize "
          f"{si['minb']}..{si['maxb']}  {si['total']} samples", flush=True)

    cap = si['maxb']
    ch0, ch1 = [0] * cap, [0] * cap
    md5 = hashlib.md5()
    bps, nch = si['bps'], si['ch']
    typecode = 'h' if bps == 16 else 'i'
    frames = done = scans = 0

    while True:
        try:
            n, m, tries = frame_header(b, cap)
        except (EOFError, ValueError):
            break
        scans += 1 if tries else 0
        try:
            subframe(b, n, ch0, bps + (1 if m == 9 else 0))
            if nch == 1:
                out = ch0[:n]
            else:
                subframe(b, n, ch1, bps + (1 if m in (8, 10) else 0))
                out = [0] * (n * 2)
                if m == 8:
                    for i in range(n):
                        a = ch0[i]; out[2*i] = a; out[2*i+1] = a - ch1[i]
                elif m == 9:
                    for i in range(n):
                        s = ch1[i]; out[2*i] = s + ch0[i]; out[2*i+1] = s
                elif m == 10:
                    for i in range(n):
                        s = ch1[i]; mid = (ch0[i] << 1) | (s & 1)
                        out[2*i] = (mid + s) >> 1; out[2*i+1] = (mid - s) >> 1
                else:
                    for i in range(n):
                        out[2*i] = ch0[i]; out[2*i+1] = ch1[i]
            if bps == 24:
                buf = bytearray()
                for v in out:
                    buf += (v & 0xFFFFFF).to_bytes(3, 'little')
                md5.update(buf)
            else:
                md5.update(array(typecode, out).tobytes())
            b.align(); b.bits(16)
        except (EOFError, ValueError) as e:
            print(f"  FAILED in frame {frames}: {e}", flush=True)
            return si, frames, done, None, scans
        frames += 1
        done += n
        if progress_every and frames % progress_every == 0:
            pct = 100.0 * done / si['total'] if si['total'] else 0
            print(f"    {frames} frames, {pct:.0f}%", flush=True)

    return si, frames, done, md5.digest(), scans


if __name__ == '__main__':
    p = sys.argv[1]
    print(os.path.basename(p), flush=True)
    si, frames, samples, digest, scans = verify(p)
    print(f"  {frames} frames, {samples} samples, {scans} frames needed a sync scan")
    if digest is None:
        sys.exit(1)
    # An all-zero digest is FLAC's "not computed", not a mismatch. Every
    # 24-bit file on the test card has one -- the tagging pipeline zeroes it --
    # so reporting MISMATCH here would be a false alarm on most of the corpus.
    if si['md5'] == b' ' * 16:
        print("  STREAMINFO md5 ABSENT -- the encoder stored none")
        print(f"  decoded    md5 {digest.hex()}")
        print("  ==> CANNOT VERIFY arithmetic; but the parse is exact:")
        print(f"      {samples} samples decoded vs {si['total']} declared"
              f" ({'match' if samples == si['total'] else 'MISMATCH'})")
        print(f"      {scans} frames needed a sync scan"
              f" ({'exact bit consumption' if scans == 0 else 'DESYNC'})")
        sys.exit(0 if (samples == si['total'] and scans == 0) else 1)
    ok = digest == si['md5']
    print(f"  STREAMINFO md5 {si['md5'].hex()}")
    print(f"  decoded    md5 {digest.hex()}")
    print("  ==> BIT-EXACT" if ok else "  ==> MISMATCH")
    sys.exit(0 if ok else 1)
