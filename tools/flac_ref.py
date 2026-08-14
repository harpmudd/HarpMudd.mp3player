#!/usr/bin/env python3
"""A line-for-line Python mirror of fw/flac.c, so the algorithm can be proved
correct without hardware and without a host C compiler (there isn't one here).

The proof is exact, not a smell test: STREAMINFO carries an MD5 of the
original interleaved PCM. If a decode reproduces that digest, the decoder is
bit-exact for that file. Nothing weaker is worth running.

Keep this in step with flac.c. Where they disagree, this file is the one that
has been checked against real data.
"""
import sys, os, hashlib, struct

BLK_TAB = [0, 192, 576, 1152, 2304, 4608, 0, 0,
           256, 512, 1024, 2048, 4096, 8192, 16384, 32768]
FIXED = [[], [1], [2, -1], [3, -3, 1], [4, -6, 4, -1]]


class Bits:
    def __init__(self, data):
        self.d, self.p, self.acc, self.n = data, 0, 0, 0

    def need(self, k):
        while self.n < k:
            if self.p >= len(self.d):
                return False
            self.acc = (self.acc << 8) | self.d[self.p]
            self.p += 1
            self.n += 8
        return True

    def bits(self, k):
        if k == 0:
            return 0
        if not self.need(k):
            raise EOFError
        self.n -= k
        return (self.acc >> self.n) & ((1 << k) - 1)

    def sbits(self, k):
        v = self.bits(k)
        return v - (1 << k) if k and (v >> (k - 1)) & 1 else v

    def unary(self):
        c = 0
        while True:
            if not self.need(1):
                raise EOFError
            self.n -= 1
            if (self.acc >> self.n) & 1:
                return c
            c += 1

    def align(self):
        self.n -= self.n & 7


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
        raise ValueError("residual method %d" % method)
    pbits, escape = (5, 31) if method else (4, 15)
    porder = b.bits(4)
    parts = 1 << porder
    if blocksize % parts:
        raise ValueError("partition order does not divide blocksize")
    idx = base
    for p in range(parts):
        count = blocksize // parts - (order if p == 0 else 0)
        param = b.bits(pbits)
        if param == escape:
            raw = b.bits(5)
            for _ in range(count):
                out[idx] = b.sbits(raw); idx += 1
        else:
            for _ in range(count):
                v = (b.unary() << param) | b.bits(param)
                out[idx] = -((v >> 1) + 1) if v & 1 else (v >> 1)
                idx += 1


def subframe(b, blocksize, out, bps):
    b.bits(1)
    typ = b.bits(6)
    wasted = b.unary() + 1 if b.bits(1) else 0
    bps -= wasted
    n = blocksize

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
        for i in range(order, n):
            out[i] += sum(c[j] * out[i - 1 - j] for j in range(order))
    elif typ >= 32:
        order = typ - 31
        for i in range(order):
            out[i] = b.sbits(bps)
        prec = b.bits(4) + 1
        shift = b.sbits(5)
        if prec == 16 or shift < 0:
            raise ValueError("bad LPC prec/shift")
        coef = [b.sbits(prec) for _ in range(order)]
        residual(b, n, order, out, order)
        for i in range(order, n):
            p = 0
            for j in range(order):
                p += coef[j] * out[i - 1 - j]
            out[i] += p >> shift
    else:
        raise ValueError("subframe type %d" % typ)

    if wasted:
        for i in range(n):
            out[i] <<= wasted


def frame_header(b, ch0_cap):
    tries = 0
    while True:
        if not b.need(15):
            raise EOFError
        peek = (b.acc >> (b.n - 15)) & 0x7FFF
        if (peek >> 1) == 0x3FFE:
            break
        b.n -= 1
        tries += 1
        if tries > (1 << 22):
            raise ValueError("lost sync")
    b.bits(14); b.bits(1); b.bits(1)
    bs, sr, chmode = b.bits(4), b.bits(4), b.bits(4)
    b.bits(3); b.bits(1)

    c = b.bits(8)
    extra = 0
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
    if not blocksize or blocksize > ch0_cap:
        raise ValueError("blocksize %d" % blocksize)
    return blocksize, chmode


def decode(path, max_frames=None, verbose=True):
    data = open(path, 'rb').read()
    b = Bits(data)
    si = open_stream(b)
    if verbose:
        print(f"  {si['bps']}-bit {si['rate']} Hz {si['ch']}ch  "
              f"blocksize {si['minb']}..{si['maxb']}  {si['total']} samples")

    cap = si['maxb']
    ch0 = [0] * cap
    ch1 = [0] * cap
    md5 = hashlib.md5()
    nbytes = si['bps'] // 8
    frames = done = 0

    while True:
        if max_frames is not None and frames >= max_frames:
            break
        try:
            blocksize, m = frame_header(b, cap)
        except (EOFError, ValueError):
            break
        try:
            bps0 = si['bps'] + (1 if m == 9 else 0)
            subframe(b, blocksize, ch0, bps0)
            if si['ch'] == 1:
                for i in range(blocksize):
                    md5.update(int(ch0[i]).to_bytes(nbytes, 'little', signed=True))
            else:
                bps1 = si['bps'] + (1 if m in (8, 10) else 0)
                subframe(b, blocksize, ch1, bps1)
                out = bytearray()
                for i in range(blocksize):
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
                    out += int(l).to_bytes(nbytes, 'little', signed=True)
                    out += int(r).to_bytes(nbytes, 'little', signed=True)
                md5.update(out)
            b.align(); b.bits(16)
        except (EOFError, ValueError) as e:
            print(f"  FAILED in frame {frames}: {e}")
            return si, frames, done, None
        frames += 1
        done += blocksize

    return si, frames, done, md5.digest()


if __name__ == '__main__':
    path = sys.argv[1]
    lim = int(sys.argv[2]) if len(sys.argv) > 2 else None
    print(os.path.basename(path))
    si, frames, samples, digest = decode(path, lim)
    print(f"  decoded {frames} frames, {samples} samples")
    if digest is None:
        sys.exit(1)
    if lim is None:
        want = si['md5']
        ok = digest == want
        print(f"  STREAMINFO md5 {want.hex()}")
        print(f"  decoded    md5 {digest.hex()}")
        print("  ==> BIT-EXACT" if ok else "  ==> MISMATCH")
        sys.exit(0 if ok else 1)
