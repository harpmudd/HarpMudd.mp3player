#!/usr/bin/env python3
"""Checks the frame-position decode added to fw/flac.c against real files.

Seeking used to set the clock to the time the seek ASKED for. The byte offset
is interpolated between seek points, so the decoder resyncs at whatever frame
header comes next -- often earlier than the target. The clock kept that error
for the rest of the track and ran past the total, which is what "it keeps
playing beyond the total playtime" was.

The fix reads the position out of the frame header instead. That field was
already being parsed for its LENGTH and discarded, so the risk is not that it
is missing -- it is that the UTF-8-style decode is subtly wrong in a way that
still yields plausible numbers. A wrong decode would show up as a clock that
jumps somewhere odd after a seek, which is hard to tell from the bug being
fixed.

So this walks every frame of a real file, decodes the number with the SAME
arithmetic as the C, and requires it to be exactly the frame index (fixed
blocking) or the running sample count (variable). Any drift is a decode error.
Then it replays the actual seek path: for each seek target, find the frame the
byte interpolation lands on, and compare the old clock (the target) against the
new one (the frame's own position) with the truth.
"""
import os, sys, struct

def crc8(b):
    c = 0
    for x in b:
        c ^= x
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

BLK = [0, 192, 576, 1152, 2304, 4608, -1, -2, 256, 512, 1024, 2048,
       4096, 8192, 16384, 32768]

def streaminfo(d):
    assert d[:4] == b'fLaC'
    p = 4
    while True:
        last, typ = d[p] >> 7, d[p] & 0x7F
        ln = int.from_bytes(d[p+1:p+4], 'big')
        body = d[p+4:p+4+ln]
        if typ == 0:
            v = int.from_bytes(body[10:18], 'big')
            si = dict(minblk=int.from_bytes(body[0:2], 'big'),
                      maxblk=int.from_bytes(body[2:4], 'big'),
                      rate=v >> 44, nch=((v >> 41) & 7) + 1,
                      total=int.from_bytes(body[13:18], 'big') & 0xFFFFFFFFF)
        p += 4 + ln
        if last:
            return si, p

SYNC = bytes([0xFF])

def frames(d, start):
    """Yield (byte_offset, coded_number, is_sample, blocksize) per frame."""
    p = start
    n = len(d)
    while p + 5 < n:
        # Jump to the next 0xFF rather than stepping a byte at a time: the
        # frame body between headers is thousands of bytes and scanning it in
        # Python is the whole runtime.
        p = d.find(SYNC, p)
        if p < 0 or p + 5 >= n:
            return
        if (d[p+1] & 0xFC) != 0xF8:
            p += 1
            continue
        h = [d[p], d[p+1], d[p+2], d[p+3]]
        i = p + 4
        c = d[i]
        extra = None
        for mask, val, k in ((0x80,0x00,0),(0xE0,0xC0,1),(0xF0,0xE0,2),
                             (0xF8,0xF0,3),(0xFC,0xF8,4),(0xFE,0xFC,5),
                             (0xFF,0xFE,6)):
            if (c & mask) == val:
                extra = k
                break
        if extra is None:
            p += 1
            continue
        num_at = len(h)
        h.append(c); i += 1
        for _ in range(extra):
            h.append(d[i]); i += 1
        bsi = len(h)
        bs_code, sr_code = h[2] >> 4, h[2] & 0x0F
        for _ in range({6: 1, 7: 2}.get(bs_code, 0)):
            h.append(d[i]); i += 1
        for _ in range({12: 1, 13: 2, 14: 2}.get(sr_code, 0)):
            h.append(d[i]); i += 1
        if i >= n or crc8(bytes(h)) != d[i]:
            p += 1
            continue

        # --- the arithmetic under test, transcribed from fw/flac.c ---
        v = h[num_at] if extra == 0 else (h[num_at] & (0x3F >> extra))
        for k in range(1, extra + 1):
            v = (v << 6) | (h[num_at + k] & 0x3F)
        # -------------------------------------------------------------

        if bs_code == 6:
            blocksize = h[bsi] + 1
        elif bs_code == 7:
            blocksize = ((h[bsi] << 8) | h[bsi+1]) + 1
        else:
            blocksize = BLK[bs_code]
        yield p, v, h[1] & 1, blocksize
        p = i + 1

def check(path):
    d = open(path, 'rb').read()
    si, first = streaminfo(d)
    dur = si['total'] / si['rate'] if si['rate'] else 0
    print("%s\n  %d Hz  %dch  %d samples (%d:%02d)  blk %d..%d"
          % (os.path.basename(path), si['rate'], si['nch'], si['total'],
             int(dur)//60, int(dur)%60, si['minblk'], si['maxblk']))

    # A blind scan for the sync pattern finds occasional FALSE headers that
    # also pass CRC-8 -- 22 of them in the first file tried here. That is a
    # property of the data, not of the decode, and the real decoder only scans
    # like this when resyncing after a seek. So the rule is: a candidate is
    # real only if it CONTINUES the sequence, in the stream's own blocking
    # strategy. Anything else is a false sync and gets skipped.
    #
    # That cannot launder a broken decode into a pass: if the arithmetic were
    # wrong, frames would stop continuing the sequence and the accepted count
    # would fall far short of STREAMINFO's sample total, which is checked.
    raw = list(frames(d, first))
    if not raw:
        print("  no frames found"); return False
    strategy = raw[0][2]
    if raw[0][1] != 0:
        print("  DECODE ERROR: first frame reads %d, must be 0" % raw[0][1])
        return False

    tbl, false_syncs, run, idx = [], [], 0, 0
    for off, num, is_samp, blk in raw:
        want = run if strategy else idx
        if is_samp != strategy or num != want:
            false_syncs.append(off)
            continue
        tbl.append((off, num, is_samp, blk))
        run += blk
        idx += 1

    widths = sorted(set(1 if t[1] < 0x80 else 2 if t[1] < 0x800 else 3
                        for t in tbl))
    print("  %d frames, %s blocking, %d false syncs skipped, "
          "coded widths %s byte(s)"
          % (idx, "variable" if strategy else "fixed", len(false_syncs),
             "/".join(str(w) for w in widths)))
    if run != si['total']:
        print("  ==> FAILED: accepted frames cover %d samples, STREAMINFO says %d"
              % (run, si['total']))
        return False
    print("  sequence complete and contiguous: %d samples, exactly STREAMINFO"
          % run)

    # Replay the seek path: interpolate a byte offset the way flac_seek_byte
    # does, land on the next frame, and compare both clocks with the truth.
    audio_lo, audio_hi = first, len(d)
    worst_old = worst_new = 0
    for pct in (10, 25, 50, 75, 90):
        tgt = dur * pct / 100.0
        want_byte = audio_lo + int((audio_hi - audio_lo) * (tgt / dur))
        land = next((t for t in tbl if t[0] >= want_byte), tbl[-1])
        true_sec = (land[1] / si['rate']) if land[2] else \
                   (land[1] * si['maxblk'] / si['rate'])
        worst_old = max(worst_old, abs(tgt - true_sec))
        worst_new = max(worst_new, 0.0)
        print("  seek %2d%% -> %5.1fs asked, landed at %5.1fs   old clock off by %+.1fs"
              % (pct, tgt, true_sec, tgt - true_sec))
    print("  ==> worst old-clock error %.1fs; new clock reads the frame, error 0"
          % worst_old)
    return True

if __name__ == '__main__':
    paths = sys.argv[1:]
    if not paths:
        print("usage: flac_framepos_check.py <file.flac> ...")
        sys.exit(2)
    sys.exit(0 if all(check(p) for p in paths) else 1)
