#!/usr/bin/env python3
"""Report what a FLAC file actually is, and whether this core could play it.

Everything here comes from STREAMINFO, which is the first metadata block and
34 bytes long -- so the core can make the same judgement before decoding a
single sample, and say why rather than stuttering.

The CPU estimate is an ESTIMATE. FLAC decode cost has not been benchmarked on
this hardware; 0.65x MP3 per sample is a literature figure for a decoder with
no MDCT and no polyphase filterbank.
"""
import sys, os, glob

MP3_44_MHZ, BUDGET_MHZ, FLAC_RATIO = 45.7, 60.0, 0.65
IO_KBPS = 736  # measured on hardware 2026-08-14

BLOCK = {0: 'STREAMINFO', 1: 'PADDING', 2: 'APPLICATION', 3: 'SEEKTABLE',
         4: 'VORBIS_COMMENT', 5: 'CUESHEET', 6: 'PICTURE'}


def probe(path):
    with open(path, 'rb') as f:
        head = f.read(1 << 16)
    if head[:4] != b'fLaC':
        return None
    info, blocks, off = {}, [], 4
    while off + 4 <= len(head):
        h = head[off]
        last, btype = h >> 7, h & 0x7F
        ln = int.from_bytes(head[off + 1:off + 4], 'big')
        blocks.append((BLOCK.get(btype, str(btype)), ln))
        if btype == 0 and off + 4 + ln <= len(head):
            s = head[off + 4:off + 4 + ln]
            v, n = int.from_bytes(s, 'big'), len(s) * 8
            info = dict(minblk=(v >> (n - 16)) & 0xFFFF,
                        maxblk=(v >> (n - 32)) & 0xFFFF,
                        rate=(v >> (n - 100)) & 0xFFFFF,
                        ch=((v >> (n - 103)) & 0x7) + 1,
                        bps=((v >> (n - 108)) & 0x1F) + 1,
                        total=(v >> (n - 144)) & 0xFFFFFFFFF)
        if last:
            break
        off += 4 + ln
    info['blocks'] = blocks
    info['bytes'] = os.path.getsize(path)
    return info


def verdict(i):
    mhz = MP3_44_MHZ * FLAC_RATIO * (i['rate'] / 44100.0)
    if i['ch'] > 2:
        return 'REJECT', f"{i['ch']} channels", mhz
    if mhz < BUDGET_MHZ * 0.8:
        return 'PLAYS', 'comfortable', mhz
    if mhz < BUDGET_MHZ:
        return 'MARGINAL', 'no CPU headroom', mhz
    return 'REJECT', 'over CPU budget', mhz


for path in sorted(sys.argv[1:] or glob.glob('*.flac')):
    i = probe(path)
    name = os.path.basename(path)
    if not i:
        print(f"{name}\n  not a FLAC file\n")
        continue
    secs = i['total'] / i['rate'] if i['rate'] else 0
    kbps = i['bytes'] * 8 / secs / 1000 if secs else 0
    v, why, mhz = verdict(i)
    print(f"{name}")
    print(f"  {i['bps']}-bit / {i['rate']} Hz / {i['ch']}ch   blocksize "
          f"{i['minblk']}..{i['maxblk']}")
    print(f"  {secs/60:.1f} min   {i['bytes']/1e6:.0f} MB   {kbps:.0f} kbps "
          f"= {kbps/8:.0f} KB/s  ({IO_KBPS/(kbps/8):.1f}x IO margin)")
    print(f"  blocks: {', '.join(f'{n}' for n, _ in i['blocks'])}")
    print(f"  -> {v}: {why}  (~{mhz:.0f} MHz of {BUDGET_MHZ:.0f})\n")
