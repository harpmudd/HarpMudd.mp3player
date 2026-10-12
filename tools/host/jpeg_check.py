#!/usr/bin/env python3
"""Run the SHIPPING picojpeg over a JPEG and say whether the core can decode it.

Album art that the core cannot decode looks, on the Pocket, exactly like album
art that is missing -- so "no cover" is not a diagnosis. picojpeg is baseline
only (`case M_SOF2: return PJPG_UNSUPPORTED_MODE;`) and is additionally picky
about sampling factors and colourspace, and all of those checks live in
pjpeg_decode_init. Rather than reason about which restriction a file trips,
this compiles third_party/picojpeg/picojpeg.c for RV32 and runs it under
tools/rv32sim.py against the real bytes.

Takes JPEG files, or .flac / .mp3 files whose embedded picture is extracted
first:

    python tools/host/jpeg_check.py cover.jpg
    python tools/host/jpeg_check.py "E:/.../Some Track.flac"

Exit status is non-zero if any input fails, so it also works as a gate.
"""
import io
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))

# Matches ART_IMG in fw/art.inc: the panel the cover is drawn into.
ART_IMG = 92
MCU_CAP = 512          # enough to prove Huffman decode runs, fast enough to wait for

ERRS = {
    0: 'ok',
    1: 'PJPG_NO_MORE_BLOCKS',
    2: 'PJPG_BAD_DHT_COUNTS',
    3: 'PJPG_BAD_DHT_INDEX',
    4: 'PJPG_BAD_DHT_MARKER',
    5: 'PJPG_BAD_DQT_MARKER',
    6: 'PJPG_BAD_DQT_TABLE',
    7: 'PJPG_BAD_PRECISION',
    8: 'PJPG_BAD_HEIGHT',
    9: 'PJPG_BAD_WIDTH',
    10: 'PJPG_TOO_MANY_COMPONENTS',
    11: 'PJPG_BAD_SOF_LENGTH',
    12: 'PJPG_BAD_VARIABLE_MARKER',
    13: 'PJPG_BAD_DRI_LENGTH',
    14: 'PJPG_BAD_SOS_LENGTH',
    15: 'PJPG_BAD_SOS_COMP_ID',
    16: 'PJPG_W_EXTRA_BYTES_BEFORE_MARKER',
    17: 'PJPG_NO_ARITHMETIC_SUPPORT',
    18: 'PJPG_UNEXPECTED_MARKER',
    19: 'PJPG_NOT_JPEG',
    20: 'PJPG_UNSUPPORTED_MARKER',
    21: 'PJPG_BAD_DQT_LENGTH',
    22: 'PJPG_TOO_MANY_BLOCKS',
    23: 'PJPG_UNDEFINED_QUANT_TABLE',
    24: 'PJPG_UNDEFINED_HUFF_TABLE',
    25: 'PJPG_NOT_SINGLE_SCAN',
    26: 'PJPG_UNSUPPORTED_COLORSPACE',
    27: 'PJPG_UNSUPPORTED_SAMP_FACTORS',
    28: 'PJPG_DECODE_ERROR',
    29: 'PJPG_BAD_RESTART_MARKER',
    30: 'PJPG_ASSERTION_ERROR',
    31: 'PJPG_BAD_SOS_SPECTRAL',
    32: 'PJPG_BAD_SOS_SUCCESSIVE',
    33: 'PJPG_STREAM_READ_ERROR',
    34: 'PJPG_NOTENOUGHMEM',
    35: 'PJPG_UNSUPPORTED_COMP_IDENT',
    36: 'PJPG_UNSUPPORTED_QUANT_TABLE',
    37: 'PJPG_UNSUPPORTED_MODE  (progressive -- picojpeg is baseline only)',
}


def flac_picture(path):
    with open(path, 'rb') as f:
        if f.read(4) != b'fLaC':
            return None
        while True:
            h = f.read(4)
            if len(h) < 4:
                return None
            last, t = h[0] & 0x80, h[0] & 0x7F
            n = (h[1] << 16) | (h[2] << 8) | h[3]
            b = f.read(n)
            if t == 6:
                q = 4
                ml = struct.unpack('>I', b[q:q + 4])[0]
                q += 4 + ml
                dl = struct.unpack('>I', b[q:q + 4])[0]
                q += 4 + dl + 16
                dlen = struct.unpack('>I', b[q:q + 4])[0]
                q += 4
                return b[q:q + dlen]
            if last:
                return None


def mp3_apic(path):
    with open(path, 'rb') as f:
        h = f.read(10)
        if h[:3] != b'ID3':
            return None
        size = (h[6] << 21) | (h[7] << 14) | (h[8] << 7) | h[9]
        tag = f.read(size)
    i = 0
    while i + 10 <= len(tag):
        fid = tag[i:i + 4]
        if not fid.strip(b'\0'):
            break
        fsz = struct.unpack('>I', tag[i + 4:i + 8])[0]
        if h[3] >= 4:      # ID3v2.4 syncsafe frame sizes
            b = tag[i + 4:i + 8]
            fsz = (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]
        body = tag[i + 10:i + 10 + fsz]
        if fid == b'APIC':
            j = body.find(b'\xff\xd8\xff')
            if j >= 0:
                return body[j:]
        i += 10 + fsz
    return None


def load(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == '.flac':
        return flac_picture(path)
    if ext == '.mp3':
        return mp3_apic(path)
    with open(path, 'rb') as f:
        return f.read()


def run(jpg, reduce_flag):
    gen = os.path.join(HERE, 'jpeg_gen.c')
    with io.open(gen, 'w', encoding='utf-8', newline='\n') as f:
        f.write('#include "hostio.h"\n')
        f.write('#include "picojpeg.h"\n\n')
        f.write('static const unsigned char IMG[] = {\n')
        for k in range(0, len(jpg), 20):
            f.write(','.join(str(b) for b in jpg[k:k + 20]) + ',\n')
        f.write('};\n')
        f.write('#define IMGLEN %du\n\n' % len(jpg))
        f.write('static unsigned int pos;\n')
        f.write('static unsigned char feed(unsigned char *pBuf,\n')
        f.write('        unsigned char buf_size, unsigned char *pRead,\n')
        f.write('        void *pData)\n{\n')
        f.write('    (void)pData;\n')
        f.write('    unsigned int n = IMGLEN - pos;\n')
        f.write('    if (n > buf_size) n = buf_size;\n')
        f.write('    for (unsigned int i = 0; i < n; i++) pBuf[i] = IMG[pos + i];\n')
        f.write('    pos += n;\n')
        f.write('    *pRead = (unsigned char)n;\n')
        f.write('    return 0;\n}\n\n')
        f.write('int main(void)\n{\n')
        f.write('    pjpeg_image_info_t info;\n')
        f.write('    unsigned char st = pjpeg_decode_init(&info, feed, 0, %d);\n'
                % reduce_flag)
        f.write('    hputu(st); hputc(32);\n')
        f.write('    hputu((unsigned)info.m_width); hputc(32);\n')
        f.write('    hputu((unsigned)info.m_height); hputc(32);\n')
        f.write('    hputu((unsigned)info.m_comps); hputc(32);\n')
        f.write('    hputu((unsigned)info.m_MCUSPerRow); hputc(32);\n')
        f.write('    hputu((unsigned)info.m_MCUSPerCol); hnl();\n')
        f.write('    if (st) return 0;\n')
        f.write('    unsigned int done = 0, cap = %du;\n' % MCU_CAP)
        f.write('    unsigned char e = 0;\n')
        f.write('    while (done < cap) {\n')
        f.write('        e = pjpeg_decode_mcu();\n')
        f.write('        if (e) break;\n')
        f.write('        done++;\n')
        f.write('    }\n')
        f.write('    hputu(done); hputc(32); hputu(e); hnl();\n')
        f.write('    return 0;\n}\n')

    gcc = os.path.join(ROOT, 'toolchain',
                       'xpack-riscv-none-elf-gcc-15.2.0-1', 'bin',
                       'riscv-none-elf-gcc.exe')
    elf = os.path.join(HERE, 'jpeg.elf')
    r = subprocess.run([gcc, '-march=rv32im', '-mabi=ilp32', '-mno-relax',
                        '-Os', '-ffreestanding', '-nostdlib', '-nostartfiles',
                        '-I', os.path.join(ROOT, 'third_party', 'picojpeg'),
                        '-I', HERE,
                        '-T', os.path.join(HERE, 'link.ld'), '-o', elf,
                        os.path.join(HERE, 'start.S'), gen,
                        os.path.join(ROOT, 'third_party', 'picojpeg',
                                     'picojpeg.c'),
                        '-lgcc'], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:2000])
        raise SystemExit(2)

    out = subprocess.run([sys.executable,
                          os.path.join(ROOT, 'tools', 'rv32sim.py'), elf],
                         capture_output=True, text=True)
    if out.returncode:
        print(out.stderr[:2000])
        raise SystemExit(2)
    return [l.split() for l in out.stdout.splitlines() if l.strip()]


def check(path):
    jpg = load(path)
    name = os.path.basename(path)
    if not jpg:
        print('%-46s no embedded picture' % name[:46])
        return False
    print('%s  (%d B)' % (name, len(jpg)))

    # The core uses picojpeg's 1/8 "reduce" path when that still lands at or
    # above the panel on both axes -- see art_decode() in fw/art.inc.
    rows = run(jpg, 1)
    st, w, h = int(rows[0][0]), int(rows[0][1]), int(rows[0][2])
    if st:
        print('   init FAILED: %s' % ERRS.get(st, 'code %d' % st))
        return False
    reduce_ok = ((w + 7) // 8) >= ART_IMG and ((h + 7) // 8) >= ART_IMG
    mode = 'reduce 1/8' if reduce_ok else 'FULL decode'
    print('   %dx%d, %s comps, %sx%s MCUs -> core path: %s'
          % (w, h, rows[0][3], rows[0][4], rows[0][5], mode))
    if not reduce_ok:
        rows = run(jpg, 0)
        if int(rows[0][0]):
            print('   full-res init FAILED: %s'
                  % ERRS.get(int(rows[0][0]), rows[0][0]))
            return False

    done, e = int(rows[1][0]), int(rows[1][1])
    if e and e != 1:
        print('   decoded %d MCUs then FAILED: %s' % (done, ERRS.get(e, e)))
        return False
    print('   decoded %d MCUs clean' % done)
    return True


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    ok = True
    for p in argv:
        if not check(p):
            ok = False
        print()
    print('DECODABLE' if ok else 'NOT DECODABLE BY THE CORE')
    return 0 if ok else 1


sys.exit(main(sys.argv[1:]))
