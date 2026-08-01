#!/usr/bin/env python3
"""Regression test for the .m3u reader in fw/playlist.inc.

Both playlist bugs found on hardware were in this code, and both would have been
caught here in a second instead of costing a copy-to-card-and-relaunch cycle:

  1. pl_read_raw() asked for a flat 4 KB. This host FAILS a read that extends
     past end-of-file rather than returning a short one, so every playlist
     smaller than 4 KB -- essentially all of them -- read nothing at all.

  2. pl_parse() wrote its NUL terminator OVER the newline, destroying the byte
     the next iteration's skip loop looks for. Every line after the first then
     started at that NUL, and a line starting with NUL is not '#', so every
     comment line counted as a track. A 23-line comment header parsed as 23
     tracks.

This mirrors the C rather than calling it, so it can drift. Keep the two in
step when either function changes.

    python sim/test_m3u_parse.py
"""
import sys

TAG_SIZE    = 4096
PL_TEXT_MAX = 8192
PL_MAX      = 128


def read_raw(data, fails_past_eof=True):
    """pl_read_raw(): descending-size ladder over a host that refuses a read
    running past the end of the file."""
    size = len(data)
    got = 0
    out = bytearray()
    while got < PL_TEXT_MAX:
        want = min(PL_TEXT_MAX - got, TAG_SIZE)
        n = 0
        while want >= 1:
            inside = (got + want <= size) if fails_past_eof else (got < size)
            if inside:
                buf = data[got:got + want]
                n = 0
                while n < want and buf[n] != 0:
                    n += 1
                if n:
                    break
            want >>= 1
        if not n:
            break
        out += data[got:got + n]
        got += n
        if n < want:
            break
    return bytes(out)


def parse(raw):
    """pl_parse(): skip blanks and #-comments, one track per remaining line."""
    t = bytearray(raw)
    n = len(t)
    tracks = []
    i = 0
    while i < n and len(tracks) < PL_MAX:
        while i < n and t[i] in (0x0A, 0x0D):
            i += 1
        if i >= n:
            break
        start = i
        while i < n and t[i] not in (0x0A, 0x0D):
            i += 1
        end = i
        # Past the terminator BEFORE terminating -- this ordering is the bug.
        if i < n and t[i] == 0x0D:
            i += 1
        if i < n and t[i] == 0x0A:
            i += 1
        while end > start and t[end - 1] in (0x20, 0x09):
            end -= 1
        if end < len(t):
            t[end] = 0
        if end > start and t[start] != ord('#'):
            tracks.append(bytes(t[start:end]).decode('latin1'))
    return tracks


def check(name, raw, expect):
    got = parse(read_raw(raw))
    if got == expect:
        print("  PASS  %s" % name)
        return True
    print("  FAIL  %s" % name)
    print("        expected %r" % (expect,))
    print("        got      %r" % (got,))
    return False


def main():
    ok = True

    ok &= check("plain list, LF",
                b"a.mp3\nb.mp3\nc.mp3\n",
                ["a.mp3", "b.mp3", "c.mp3"])

    ok &= check("plain list, CRLF",
                b"a.mp3\r\nb.mp3\r\nc.mp3\r\n",
                ["a.mp3", "b.mp3", "c.mp3"])

    ok &= check("no trailing newline",
                b"a.mp3\nb.mp3",
                ["a.mp3", "b.mp3"])

    # Bug 2: a comment header must not be counted as tracks.
    ok &= check("comment header is skipped",
                b"#EXTM3U\n#\n# notes\n#\na.mp3\nb.mp3\n",
                ["a.mp3", "b.mp3"])

    ok &= check("extended m3u (#EXTINF between tracks)",
                b"#EXTM3U\n#EXTINF:123,Artist - Title\na.mp3\n"
                b"#EXTINF:456,Other\nb.mp3\n",
                ["a.mp3", "b.mp3"])

    ok &= check("blank lines and trailing spaces",
                b"\n\na.mp3   \n\n\nb.mp3\t\n\n",
                ["a.mp3", "b.mp3"])

    ok &= check("comments only -> no tracks",
                b"#EXTM3U\n# nothing here\n#\n",
                [])

    ok &= check("empty file", b"", [])

    # Bug 1: a file far smaller than one read chunk must still be delivered
    # whole, including a final byte the ladder used to drop.
    tail = b"#c\n" + b"x" * 40 + b".mp3"          # no trailing newline
    ok &= check("sub-chunk file, unterminated last line",
                tail, ["x" * 40 + ".mp3"])

    # Every length near the ladder's steps -- this is where the 4-byte floor
    # silently truncated the last track name.
    for pad in range(0, 40):
        raw = b"#\n" + b"a" * pad + b".mp3\n"
        want = [b"a" * pad + b".mp3"][0].decode()
        if parse(read_raw(raw)) != [want]:
            print("  FAIL  ladder at length %d (pad=%d)" % (len(raw), pad))
            ok = False
    else:
        print("  PASS  ladder delivers every length 7..46 intact")

    # A file larger than one chunk still assembles in order.
    many = b"".join(b"track%03d.mp3\n" % k for k in range(100))
    ok &= check("multi-chunk file (100 tracks)",
                many, ["track%03d.mp3" % k for k in range(100)])

    print("\n%s" % ("ALL PASS" if ok else "FAILURES ABOVE"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
