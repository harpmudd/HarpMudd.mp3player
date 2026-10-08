#!/usr/bin/env python3
"""Write an ID3v2.3 USLT (unsynchronised lyrics) frame into a copy of an MP3.

Exists because the real library cannot test this path: 4,618 of 7,169 files
carry a USLT frame and 4,563 of those are 12-byte EMPTY placeholders --
encoding byte, "XXX", a BOM and nothing else. Only two hold real lyrics, and
fourteen more hold a filesharing watermark. So a test file has to be built.

Two things are deliberately exercised:

  * UTF-16 (encoding 1) is what every tagged file in that library uses, and
    it is the awkward case -- BOM detection, a NUL-terminated descriptor that
    is TWO bytes wide, and surrogates.
  * LRC-FORMATTED text inside USLT. Nothing stops anyone pasting timestamps
    into a lyrics tag, and if the firmware feeds the tag body to the same
    parser the sidecar uses, those files get SYNCED lyrics with no sidecar.

    usage: embed_uslt.py <in.mp3> <out.mp3> <lyrics.txt> [--utf8]
"""
import struct, sys


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def frame(fid, body):
    """ID3v2.3: a PLAIN 32-bit size, not the syncsafe one v2.4 uses."""
    return fid + struct.pack('>I', len(body)) + b'\x00\x00' + body


def text_frame(fid, s):
    return frame(fid, b'\x01' + b'\xff\xfe' + s.encode('utf-16-le'))


def uslt(text, utf8=False, desc=''):
    """desc is the frame's content descriptor. An EMPTY one HIDES BUGS: a
    correct 16-bit terminator scan and a wrong 8-bit one both exit at once
    and land on the same offset, so a file without a descriptor cannot tell
    them apart -- a deliberately broken decoder passed against exactly that.
    Always keep one test file with a non-empty descriptor."""
    if utf8:
        return frame(b'USLT', bytes([3]) + b'eng' +
                     desc.encode('utf-8') + bytes([0]) + text.encode('utf-8'))
    # enc 1: descriptor and text each carry their own BOM, and the
    # descriptor's terminator is a 16-bit NUL.
    return frame(b'USLT', bytes([1]) + b'eng' +
                 bytes([0xFF, 0xFE]) + desc.encode('utf-16-le') + bytes([0, 0]) +
                 bytes([0xFF, 0xFE]) + text.encode('utf-16-le'))


def strip_tag(raw):
    if raw[:3] != b'ID3':
        return raw
    n = ((raw[6] & 0x7F) << 21 | (raw[7] & 0x7F) << 14 |
         (raw[8] & 0x7F) << 7 | (raw[9] & 0x7F))
    return raw[10 + n:]


def main(src, dst, lyr, utf8, desc=''):
    raw = open(src, 'rb').read()
    audio = strip_tag(raw)
    text = open(lyr, encoding='utf-8').read()

    title = 'USLT ' + ('UTF-8 plain' if utf8 else 'UTF-16 synced')
    body = (text_frame(b'TIT2', title) +
            text_frame(b'TPE1', 'Embedded lyrics test') +
            uslt(text, utf8, desc))
    tag = b'ID3' + bytes([3, 0]) + b'\x00' + syncsafe(len(body)) + body
    open(dst, 'wb').write(tag + audio)
    print('%s\n  tag %d B (USLT %d B, %s), audio %d B, total %d B'
          % (dst, len(tag), len(uslt(text, utf8, desc)),
             'UTF-8' if utf8 else 'UTF-16', len(audio), len(tag) + len(audio)))


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if x != '--utf8' and not x.startswith('--desc=')]
    d = next((x.split('=',1)[1] for x in sys.argv if x.startswith('--desc=')), '')
    main(a[0], a[1], a[2], '--utf8' in sys.argv, d)
