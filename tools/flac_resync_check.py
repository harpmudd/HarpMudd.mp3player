"""Replays the post-seek resync rule over real files.

Two things must both hold, and they pull in opposite directions:
  - a FALSE sync must never be believed (that is what broke seek), and
  - a real correction must never be rejected (or the fix does nothing).

So this drives the rule from both ends: from the true frame at each seek
target, and from each false sync the scan actually contains.
"""
import sys, os, importlib.util
spec = importlib.util.spec_from_file_location('fp', 'flac_framepos_check.py')
fp = importlib.util.module_from_spec(spec); spec.loader.exec_module(fp)

WINDOW, TRIES = 30, 12

def resync(seq, start_i, tgt, blk, rate, is_samp):
    """The C rule: hold a candidate, require the next frame to be consecutive
    AND the position to land within WINDOW of the seek target."""
    prev, tries, i = seq[start_i][1], 0, start_i + 1
    while i < len(seq) and tries < TRIES:
        num = seq[i][1]
        ok = (num > prev and num - prev <= blk) if is_samp else (num == prev + 1)
        pos = (num if is_samp else num * blk) / rate
        if ok and abs(pos - tgt) <= WINDOW:
            return pos, tries
        tries += 1
        prev = num
        i += 1
    return None, tries

for path in sys.argv[1:]:
    d = open(path, 'rb').read()
    si, first = fp.streaminfo(d)
    p, seek = 4, None
    while True:
        last, typ = d[p] >> 7, d[p] & 0x7F
        ln = int.from_bytes(d[p+1:p+4], 'big')
        if typ == 3: seek = d[p+4:p+4+ln]
        p += 4 + ln
        if last: break
    audio0, rate, blk = p, si['rate'], si['maxblk']

    raw = list(fp.frames(d, audio0))
    strategy = raw[0][2]
    tbl, false_syncs, run, idx = [], [], 0, 0
    for off, num, is_samp, b in raw:
        want = run if strategy else idx
        if is_samp != strategy or num != want:
            false_syncs.append((off, num)); continue
        tbl.append((off, num, is_samp, b)); run += b; idx += 1

    dur = si['total'] / rate
    print("%s\n  %d real frames, %d false syncs, %s"
          % (os.path.basename(path), len(tbl), len(false_syncs),
             "seektable" if seek else "no seektable"))

    # 1. real corrections must be ACCEPTED
    acc = rej = 0
    worst = 0.0
    for pct in range(5, 100, 5):
        tgt = dur * pct / 100.0
        # where the scan starts: first real frame at/after the interpolated byte
        want_b = audio0 + int((len(d) - audio0) * pct / 100.0)
        si_i = next((k for k, t in enumerate(tbl) if t[0] >= want_b), len(tbl) - 2)
        pos, tries = resync(tbl, si_i, tgt, blk, rate, strategy)
        if pos is None: rej += 1
        else:
            acc += 1
            true = (tbl[si_i+1][1] if strategy else tbl[si_i+1][1] * blk) / rate
            worst = max(worst, abs(pos - true))
    print("  real corrections: %d accepted, %d rejected (worst error vs truth %.2fs)"
          % (acc, rej, worst))

    # 2. false syncs must be REJECTED as the starting candidate
    caught = believed = 0
    for off, num in false_syncs:
        pos_f = (num if strategy else num * blk) / rate
        # the true time at that byte, for the seek target
        k = next((i for i, t in enumerate(tbl) if t[0] >= off), len(tbl) - 2)
        tgt = (tbl[k][1] if strategy else tbl[k][1] * blk) / rate
        # candidate = the false sync; next real frame will not be consecutive
        fake = [(off, num, strategy, blk)] + tbl[k:]
        pos, tries = resync(fake, 0, tgt, blk, rate, strategy)
        if pos is not None and abs(pos - pos_f) < 0.01:
            believed += 1
        else:
            caught += 1
    print("  false syncs as candidate: %d rejected, %d believed  ==> %s\n"
          % (caught, believed,
             "OK" if believed == 0 and rej == 0 else "FAILED"))
