#!/usr/bin/env python3
"""Rewrite a FLAC file's declared sample rate, leaving every audio byte alone.

This exists to test the core's two FLAC gates on real content, which is
otherwise impossible: no encoder is installed, and tools/flac_make_test.py
writes VERBATIM subframes -- raw samples, no Rice and no LPC -- so a file from
it decodes far too cheaply to say anything about whether the decoder keeps up.

Relabelling solves that. The decode work per sample is a property of the
ENCODED CONTENT, not of the number in the header, so a 44.1 kHz track declared
as 88.2 kHz makes the decoder do real Rice and LPC work at 88,200 sample-frames
a second -- which is exactly the question. It plays at double speed and sounds
like chipmunks; that is expected and is not what the test is looking at. The
instrument is the underrun-edge counter in a UI_SHOW_SPEED_DIAG build.

Host-side this proves nothing new: tools/host/flac_bench.py would report the
same instructions per sample either way, because the rate field is metadata.
Its whole value is ON HARDWARE, where the rate sets the deadline and brings the
emit path, the meters, the UI and the SD read rate along with it.

    python tools/flac_relabel.py IN.flac OUT.flac 88200
    python tools/flac_relabel.py IN.flac OUT.flac 48000 --max-frames 400

What gets rewritten, and nothing else:

  * the 20-bit rate field in STREAMINFO,
  * the 4-bit rate code in every frame header, only ever swapped for another
    code of the same LENGTH, so no frame moves by a single byte,
  * the two CRCs covering those bytes -- the header's CRC-8 and the frame's
    trailing CRC-16.

The STREAMINFO MD5 is over the DECODED AUDIO, which does not change, so it
stays valid and tools/flac_verify.py can still check the file end to end.
--max-frames is the exception: a truncated file has neither the right sample
count nor the right MD5, so both are set to "unknown", which FLAC permits.

FINDING THE FRAME BOUNDARIES is the whole difficulty, and two weaker rules
were tried and discarded on real files before this one:

  * "the next CRC-8-valid header ends this frame" -- an 8-bit CRC lets about
    one body byte in 256 through, and this aborted on frame 1038 of 2000.
  * "...and the CRC-16 must close there" -- better, but across ~100k candidate
    positions a 16-bit check is expected to pass by chance once or twice, and
    it did, truncating a frame and leaving the next one unopenable.

So the boundary must also be CONTINUOUS: a real successor carries the next
coded number, which is the frame number for a fixed-blocksize stream and the
sample number for a variable one. Three independent conditions make a false
accept effectively impossible, and any frame that cannot be closed aborts the
run rather than writing a plausible-looking file.
"""
import os
import struct
import sys

SYNC = b'\xff'

# Rate codes needing no extra header bytes, so substituting one for another
# leaves every offset in the file untouched.
RATE_CODE = {88200: 1, 176400: 2, 192000: 3, 8000: 4, 16000: 5, 22050: 6,
             24000: 7, 32000: 8, 44100: 9, 48000: 10, 96000: 11}
BLK = {1: 192, 2: 576, 3: 1152, 4: 2304, 5: 4608, 8: 256, 9: 512, 10: 1024,
       11: 2048, 12: 4096, 13: 8192, 14: 16384, 15: 32768}

T8 = []
for _b in range(256):
    _c = _b
    for _ in range(8):
        _c = ((_c << 1) ^ 0x07) & 0xFF if _c & 0x80 else (_c << 1) & 0xFF
    T8.append(_c)

T16 = []
for _b in range(256):
    _c = _b << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x8005) & 0xFFFF if _c & 0x8000 else (_c << 1) & 0xFFFF
    T16.append(_c)


def crc8(d):
    c = 0
    for x in d:
        c = T8[c ^ x]
    return c


def crc16(d):
    c = 0
    for x in d:
        c = ((c << 8) ^ T16[(c >> 8) ^ x]) & 0xFFFF
    return c


def candidates(d, start):
    """Every sync whose CRC-8 validates, as a dict per candidate. Mirrors the
    header walk in tools/flac_framepos_check.py, plus the coded number."""
    n = len(d)
    p = start
    out = []
    while p + 5 < n:
        p = d.find(SYNC, p)
        if p < 0 or p + 5 >= n:
            return out
        if (d[p + 1] & 0xFC) != 0xF8:
            p += 1
            continue
        h = [d[p], d[p + 1], d[p + 2], d[p + 3]]
        i = p + 4
        c = d[i]
        extra = None
        for mask, val, k in ((0x80, 0x00, 0), (0xE0, 0xC0, 1), (0xF0, 0xE0, 2),
                             (0xF8, 0xF0, 3), (0xFC, 0xF8, 4), (0xFE, 0xFC, 5),
                             (0xFF, 0xFE, 6)):
            if (c & mask) == val:
                extra = k
                break
        if extra is None:
            p += 1
            continue
        num_at = len(h)
        h.append(c)
        i += 1
        for _ in range(extra):
            h.append(d[i])
            i += 1
        bsi = len(h)
        bs_code, sr_code = h[2] >> 4, h[2] & 0x0F
        for _ in range({6: 1, 7: 2}.get(bs_code, 0)):
            h.append(d[i])
            i += 1
        for _ in range({12: 1, 13: 2, 14: 2}.get(sr_code, 0)):
            h.append(d[i])
            i += 1
        if i >= n or crc8(bytes(h)) != d[i]:
            p += 1
            continue
        v = h[num_at] if extra == 0 else (h[num_at] & (0x3F >> extra))
        for k in range(1, extra + 1):
            v = (v << 6) | (h[num_at + k] & 0x3F)
        if bs_code == 6:
            bs = h[bsi] + 1
        elif bs_code == 7:
            bs = ((h[bsi] << 8) | h[bsi + 1]) + 1
        else:
            bs = BLK.get(bs_code, 0)
        out.append(dict(off=p, h=h, c8=i, num=v, bs=bs, var=h[1] & 1))
        p = i + 1
    return out


def walk(d, start, limit=0):
    """Real frames, as (candidate, stop), plus whether an incomplete trailing
    frame was dropped. A boundary must satisfy all three of: CRC-8 header,
    CRC-16 closing the frame, and coded-number continuity."""
    cand = candidates(d, start)
    if len(cand) < 2:
        return [], 0, False
    marks, rejected = [], 0
    ci = 0
    while ci < len(cand):
        a = cand[ci]
        want = a['num'] + (a['bs'] if a['var'] else 1)
        c, pos, stop, j = 0, a['off'], None, ci + 1
        while j <= len(cand):
            last = j >= len(cand)
            end = len(d) if last else cand[j]['off']
            if end - 2 > pos:
                for x in d[pos:end - 2]:
                    c = ((c << 8) ^ T16[(c >> 8) ^ x]) & 0xFFFF
                pos = end - 2
            if c == ((d[end - 2] << 8) | d[end - 1]) and \
                    (last or cand[j]['num'] == want):
                stop = end
                break
            j += 1
        if stop is None:
            # A final frame that closes nowhere is an incomplete tail, not a
            # fault: real files get cut off. The Blue Hearts track on the test
            # card is one -- its STREAMINFO claims 0.8 s more audio than is
            # present, and its last frame stops 224 bytes in. Drop it and end
            # the output at the last whole frame. Anywhere else in the file
            # the same symptom means a boundary was misread, which must abort.
            if ci == len(cand) - 1 and marks:
                return marks, rejected, True
            raise ValueError('frame at %d does not close' % a['off'])
        rejected += max(0, j - ci - 1)
        marks.append((a, stop))
        if limit and len(marks) >= limit:
            break
        if j >= len(cand) or stop >= len(d):
            break
        ci = j
    return marks, rejected, False


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst, rate = argv[0], argv[1], int(argv[2])
    limit = 0
    if '--max-frames' in argv:
        limit = int(argv[argv.index('--max-frames') + 1])
    if rate not in RATE_CODE:
        print('rate %d has no single-nibble code; pick one of: %s'
              % (rate, ', '.join(str(r) for r in sorted(RATE_CODE))))
        return 2
    new_code = RATE_CODE[rate]

    d = bytearray(open(src, 'rb').read())
    if bytes(d[:4]) != b'fLaC':
        print('not a FLAC file')
        return 2

    i, si = 4, None
    while True:
        h = d[i]
        n = (d[i + 1] << 16) | (d[i + 2] << 8) | d[i + 3]
        if (h & 0x7F) == 0:
            si = i + 4
        i += 4 + n
        if h & 0x80:
            break
    audio_start = i
    old_rate = (d[si + 10] << 12) | (d[si + 11] << 4) | (d[si + 12] >> 4)
    bps = (((d[si + 12] & 0x01) << 4) | (d[si + 13] >> 4)) + 1
    maxb = struct.unpack('>H', bytes(d[si + 2:si + 4]))[0]
    print('%s' % os.path.basename(src))
    print('  %d Hz / %d-bit / max block %d  ->  %d Hz'
          % (old_rate, bps, maxb, rate))

    try:
        marks, rejected, cut = walk(d, audio_start, limit)
    except ValueError as e:
        print('  %s -- ABORT' % e)
        return 1
    print('  %d frames, %d false syncs rejected%s'
          % (len(marks), rejected,
             ', incomplete trailing frame dropped' if cut else ''))

    n_patched = n_already = 0
    for a, stop in marks:
        off, sr_code = a['off'], a['h'][2] & 0x0F
        if sr_code == new_code:
            n_already += 1
            continue
        if sr_code in (0, 12, 13, 14):
            print('  frame at %d uses rate code %d (not a plain code) -- ABORT'
                  % (off, sr_code))
            return 1
        d[off + 2] = (d[off + 2] & 0xF0) | new_code
        d[a['c8']] = crc8(bytes(d[off:a['c8']]))
        new16 = crc16(d[off:stop - 2])
        d[stop - 2] = new16 >> 8
        d[stop - 1] = new16 & 0xFF
        n_patched += 1

    d[si + 10] = (rate >> 12) & 0xFF
    d[si + 11] = (rate >> 4) & 0xFF
    d[si + 12] = ((rate & 0x0F) << 4) | (d[si + 12] & 0x0F)

    if limit or cut:
        d = d[:marks[-1][1]]
        # A shortened stream has neither the original sample count nor the
        # original MD5. FLAC spells both "unknown" as zero, which is legal and
        # is what an encoder writing a stream of unknown length emits.
        #
        # total_samples is bits 108..143 -- byte 13's LOW nibble and bytes
        # 14..17. Byte 12's low nibble is channels and the top bit of the bit
        # depth; clearing that instead, as a first version did, rewrites the
        # file as mono 8-bit and the mistake is invisible until playback.
        d[si + 13] = d[si + 13] & 0xF0
        for k in range(14, 18):
            d[si + k] = 0
        for k in range(18, 34):
            d[si + k] = 0
        print('  %d frames kept; sample count and MD5 set to unknown'
              % len(marks))

    open(dst, 'wb').write(bytes(d))
    print('  %d headers rewritten, %d already correct, %d bytes out'
          % (n_patched, n_already, len(d)))

    # Re-walk the OUTPUT from scratch under the same three rules. If a
    # rewritten CRC no longer identifies its boundary the walk cannot close,
    # which is itself the failure.
    e = bytes(open(dst, 'rb').read())
    try:
        m2, _, _ = walk(e, audio_start)
    except ValueError as err:
        print('  verify: %s -- FAILED' % err)
        return 1
    wrong = sum(1 for a, _ in m2 if (a['h'][2] & 0x0F) != new_code)
    print('  verify: %d frames re-read and closed, %d with the wrong rate code'
          % (len(m2), wrong))
    return 1 if (wrong or len(m2) != len(marks)) else 0


sys.exit(main(sys.argv[1:]))
