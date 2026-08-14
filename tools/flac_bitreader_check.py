#!/usr/bin/env python3
"""Proves the rewritten bit reader in fw/flac.c, without hardware.

The reader was changed in three ways to chase the 24-bit hiccups -- a 64-bit
reservoir refilled four bytes at a time, unary by leading-zero count instead of
one bit per iteration, and an accumulator that is no longer masked after each
read. All three are the kind of change that still decodes *something* when it
is wrong, so "it played" would not have been evidence.

There is no host C compiler here, so this models the C exactly rather than
compiling it: a 64-bit register that TRUNCATES on shift, garbage left above the
live window, the same <=31 refill guard, and lead = clz(win) - (64 - bitcnt).
Then it runs the real decoder over real files and checks the same things
flac_verify.py does -- MD5 where the encoder stored one, and exact sample count
with zero resync scans where it did not.

If the model and the reference reader both reproduce a file, the ALGORITHM is
right. What that cannot cover is C-specific undefined behaviour, so the two
places it could bite are pinned by asserts here instead: the reservoir never
exceeds 63 bits (or `1 << bitcnt` is undefined) and no shift is ever 64.
"""
import sys, os
import flac_verify
from flac_verify import verify

M64 = (1 << 64) - 1


class CBits:
    """Bit reader modelled on the C in fw/flac.c, register widths included."""
    __slots__ = ('d', 'p', 'acc', 'n', 'max_n')

    def __init__(self, data):
        self.d, self.p, self.acc, self.n = data, 0, 0, 0
        self.max_n = 0

    # ---- need(): refill 4 bytes when the guard allows, else 1 -------------
    def _fill(self, k):
        while self.n < k:
            if self.p + 4 <= len(self.d) and self.n <= 31:
                self.acc = ((self.acc << 32) | int.from_bytes(
                    self.d[self.p:self.p + 4], 'big')) & M64
                self.p += 4
                self.n += 32
            else:
                if self.p >= len(self.d):
                    raise EOFError
                self.acc = ((self.acc << 8) | self.d[self.p]) & M64
                self.p += 1
                self.n += 8
            # The C indexes (1 << bitcnt) in unary; at 64 that is undefined.
            assert self.n <= 63, "reservoir reached %d bits" % self.n
            if self.n > self.max_n:
                self.max_n = self.n

    def bits(self, k):
        if not k:
            return 0
        if self.n < k:
            self._fill(k)
        self.n -= k
        assert self.n < 64, "shift of %d" % self.n
        mask = 0xFFFFFFFF if k == 32 else (1 << k) - 1
        # NOTE: no masking of acc afterwards -- the C leaves the consumed bits
        # in place and relies on the mask. Modelling that is the whole point.
        return (self.acc >> self.n) & mask

    def sbits(self, k):
        v = self.bits(k)
        return v - (1 << k) if k and (v >> (k - 1)) & 1 else v

    def unary(self):
        c = 0
        while True:
            if self.n == 0:
                self._fill(1)
            win = self.acc & ((1 << self.n) - 1)
            if win:
                lead = (64 - win.bit_length()) - (64 - self.n)   # = clz - (64-n)
                c += lead
                self.n -= lead + 1
                return c
            c += self.n
            self.n = 0
            if c > (1 << 20):
                return c

    def align(self):
        self.n -= self.n & 7

    # frame_header() in flac_verify pokes .acc/.n directly for the sync scan;
    # it masks acc as it goes, which is harmless here -- dropping bits ABOVE
    # the window cannot change any value the reader returns.
    def _fill_pub(self, k):
        self._fill(k)


if __name__ == '__main__':
    paths = sys.argv[1:]
    if not paths:
        print("usage: flac_bitreader_check.py <file.flac> ...")
        sys.exit(2)

    flac_verify.Bits = CBits          # swap the reader, keep the decoder
    bad = 0
    for path in paths:
        print(os.path.basename(path), flush=True)
        try:
            si, frames, samples, digest, scans = verify(path, progress_every=0)
        except Exception as e:
            print("  ERROR %s: %s" % (type(e).__name__, e))
            bad += 1
            continue
        ok = digest is not None and samples == si['total'] and scans == 0
        if digest is not None and si['md5'] != b'\0' * 16:
            ok = ok and digest == si['md5']
            print("  md5 %s" % ("MATCH" if digest == si['md5'] else "MISMATCH"))
        else:
            print("  md5 absent -- checking parse exactness instead")
        print("  %d frames, %d/%d samples, %d resync scans  ==> %s"
              % (frames, samples, si['total'], scans, "OK" if ok else "FAILED"))
        bad += 0 if ok else 1
    print("\n%s" % ("all files reproduce" if not bad else "%d FAILED" % bad))
    sys.exit(1 if bad else 0)
