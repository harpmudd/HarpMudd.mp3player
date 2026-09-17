/* utf8.h -- the text model: every string in the player is UTF-8.
 *
 * Decoders convert INTO it at the door (ID3 UTF-16, Windows-1252 fallbacks),
 * and everything downstream -- buffers, marquees, the draw primitives -- walks
 * it a character at a time rather than a byte at a time. Header-only so
 * flac.c and player.c share one definition; -ffunction-sections drops what a
 * translation unit does not call. */
#ifndef UTF8_H
#define UTF8_H

#include <stdint.h>

/* One character, advancing *p past it. Anything malformed -- a stray
 * continuation byte, a lead byte whose sequence is cut short (including by the
 * terminating NUL), a byte UTF-8 never uses -- is one '?' for one byte, so a
 * bad byte costs a glyph and never swallows the text after it. */
static uint32_t u8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0], n;
    if (c < 0x80u) { *p += 1; return c; }
    if      (c >= 0xC2u && c <= 0xDFu) { n = 1u; c &= 0x1Fu; }
    else if (c >= 0xE0u && c <= 0xEFu) { n = 2u; c &= 0x0Fu; }
    else if (c >= 0xF0u && c <= 0xF4u) { n = 3u; c &= 0x07u; }
    else { *p += 1; return '?'; }
    for (uint32_t i = 1; i <= n; i++) {
        if ((s[i] & 0xC0u) != 0x80u) { *p += 1; return '?'; }
        c = (c << 6) | (s[i] & 0x3Fu);
    }
    *p += n + 1u;
    return c;
}

/* Byte index of the character before index i (i > 0). */
static uint32_t u8_prev(const char *s, uint32_t i)
{
    do i--; while (i && ((unsigned char)s[i] & 0xC0u) == 0x80u);
    return i;
}

/* Byte index of the character after index i. */
static uint32_t u8_skip(const char *s, uint32_t i)
{
    if (s[i]) {
        i++;
        while (((unsigned char)s[i] & 0xC0u) == 0x80u) i++;
    }
    return i;
}

/* Length n, shortened so it does not end partway through a character. Every
 * fixed buffer truncates somewhere; a cut through a three-byte kanji otherwise
 * leaves a lead byte that draws as '?' at the end of the title. */
static uint32_t u8_trim(const char *s, uint32_t n)
{
    if (!n) return 0;
    uint32_t st = n - 1u;
    while (st && ((unsigned char)s[st] & 0xC0u) == 0x80u) st--;
    uint32_t c = (unsigned char)s[st];
    uint32_t len = (c < 0x80u) ? 1u : (c >= 0xF0u) ? 4u : (c >= 0xE0u) ? 3u : 2u;
    return (st + len <= n) ? n : st;
}

/* Append cp as UTF-8 at out[o] if it fits WHOLE with room left for the NUL.
 * Returns the new length, or o unchanged when it does not fit. */
static uint32_t u8_put(char *out, uint32_t o, uint32_t cap, uint32_t cp)
{
    static const unsigned char lead[5] = { 0, 0, 0xC0, 0xE0, 0xF0 };
    uint32_t k = (cp < 0x80u) ? 1u : (cp < 0x800u) ? 2u : (cp < 0x10000u) ? 3u : 4u;
    if (o + k >= cap) return o;
    if (k == 1u) { out[o] = (char)cp; return o + 1u; }
    uint32_t sh = 6u * (k - 1u);
    out[o++] = (char)(lead[k] | (cp >> sh));
    while (sh) { sh -= 6u; out[o++] = (char)(0x80u | ((cp >> sh) & 0x3Fu)); }
    return o;
}

#endif
