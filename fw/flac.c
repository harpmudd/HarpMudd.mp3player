/* Minimal streaming FLAC decoder -- see flac.h for why this exists rather
 * than libFLAC or dr_flac. */

#include "flac.h"

/* ------------------------------------------------------------------ input */

static int fill(flac_t *f)
{
    if (f->pos < f->have) return 1;
    if (f->eof) return 0;
    int n = f->read(f->ctx, f->buf, (int)sizeof(f->buf));
    if (n <= 0) { f->eof = 1; f->have = f->pos = 0; return 0; }
    f->have = (uint32_t)n;
    f->pos  = 0;
    return 1;
}

static int byte(flac_t *f)
{
    if (!fill(f)) return -1;
    return f->buf[f->pos++];
}

/* MSB-first bit reader over a 64-bit reservoir, refilled four bytes at a time.
 *
 * Aimed by measurement, not by guess: the Rice/bit-reader pass is 64-76% of
 * channel 0's decode (R on the diagnostic row), and its share RISES with
 * bitrate -- which is also why 24-bit files cost what they do. They carry
 * ~18.5 coded bits per sample against ~9.6 for a CD rip, so the reader does
 * roughly twice the work while the arithmetic barely changes.
 *
 * 64 bits keeps the refill guard at <=31, so the reservoir can never exceed 63
 * and (1 << bitcnt) below is always defined. */
static int need(flac_t *f, uint32_t n)
{
    while (f->bitcnt < n) {
        if (f->pos + 4u <= f->have && f->bitcnt <= 31u) {
            f->bitacc = (f->bitacc << 32)
                      | ((uint64_t)f->buf[f->pos] << 24)
                      | ((uint64_t)f->buf[f->pos + 1u] << 16)
                      | ((uint64_t)f->buf[f->pos + 2u] << 8)
                      |  (uint64_t)f->buf[f->pos + 3u];
            f->pos += 4u;
            f->bitcnt += 32u;
            continue;
        }
        int b = byte(f);
        if (b < 0) return 0;
        f->bitacc = (f->bitacc << 8) | (uint64_t)b;
        f->bitcnt += 8;
    }
    return 1;
}

static uint32_t bits(flac_t *f, uint32_t n)
{
    if (!n) return 0;
    if (f->bitcnt < n && !need(f, n)) return 0;
    f->bitcnt -= n;
    return (uint32_t)((f->bitacc >> f->bitcnt)
                      & (n == 32u ? 0xFFFFFFFFu : ((1u << n) - 1u)));
}

/* Sign-extend an n-bit two's complement value. */
static int32_t sbits(flac_t *f, uint32_t n)
{
    if (!n) return 0;
    uint32_t v = bits(f, n);
    if (n < 32u && (v & (1u << (n - 1u)))) v |= ~((1u << n) - 1u);
    return (int32_t)v;
}

/* Unary: count zeros up to the first 1. Every Rice residual calls this, so it
 * is the hottest function in the decoder.
 *
 * Scans the whole reservoir at once. __builtin_clzll is a few instructions;
 * the previous version cost a call to need() and a shift PER ZERO BIT. An
 * intermediate attempt that found the high bit by shifting was discarded
 * before it ever built -- that is one iteration per bit of the RESERVOIR,
 * which is slower than the loop it was meant to replace. */
static uint32_t unary(flac_t *f)
{
    uint32_t n = 0;
    for (;;) {
        if (!f->bitcnt && !need(f, 1)) return n;

        uint64_t win = f->bitacc & (((uint64_t)1 << f->bitcnt) - 1u);
        if (win) {
            uint32_t lead = (uint32_t)__builtin_clzll(win) - (64u - f->bitcnt);
            n += lead;
            f->bitcnt -= lead + 1u;          /* the zeros and the 1 */
            return n;
        }

        n += f->bitcnt;                      /* all zeros -- consume them all */
        f->bitcnt = 0;
        if (n > (1u << 20)) return n;        /* corrupt stream, do not hang */
    }
}

static void align_byte(flac_t *f) { f->bitcnt -= f->bitcnt & 7u; }

/* ------------------------------------------------------------ metadata */

/* ---- Vorbis comments (metadata block type 4) ---------------------------
 *
 * Structurally trivial next to ID3v2 -- no frame table, no unsynchronisation,
 * no per-frame encoding byte, no v2.2/v2.3/v2.4 divergence. Just a vendor
 * string, a count, and that many "KEY=value" entries, every length a 32-bit
 * LITTLE-endian field. (Little-endian: the one place FLAC departs from the
 * big-endian convention of everything else in the format, because the block
 * is inherited wholesale from Vorbis.)
 *
 * Values are UTF-8. They are copied as raw bytes and truncated to fit, which
 * matches what the ID3 path already does -- the font is Latin-1, so anything
 * outside it renders as substitutes either way, and the filename fallback is
 * the better answer for those files. */
static uint32_t le32(flac_t *f)
{
    uint32_t a = bits(f, 8), b = bits(f, 8), c = bits(f, 8), d = bits(f, 8);
    return a | (b << 8) | (c << 16) | (d << 24);
}

/* Case-insensitive match of "KEY=" at the head of the entry. */
static int key_is(const char *buf, uint32_t n, const char *key)
{
    uint32_t i = 0;
    for (; key[i]; i++) {
        if (i >= n) return 0;
        char c = buf[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != key[i]) return 0;
    }
    return i < n && buf[i] == '=';
}

static void tag_copy(char *dst, uint32_t cap, const char *src, uint32_t n)
{
    if (!dst || !cap) return;
    uint32_t j = 0;
    while (j < n && j + 1u < cap) { dst[j] = src[j]; j++; }
    dst[j] = 0;
}

static void vorbis_comments(flac_t *f, uint32_t length)
{
    uint32_t used = 0;
    uint32_t vlen = le32(f); used += 4u;
    for (uint32_t i = 0; i < vlen && used < length; i++, used++) (void)bits(f, 8);
    if (used + 4u > length) return;

    uint32_t count = le32(f); used += 4u;
    /* A corrupt count must not turn into a long spin over a short block. */
    if (count > 1024u) count = 1024u;

    for (uint32_t c = 0; c < count && used + 4u <= length; c++) {
        uint32_t n = le32(f); used += 4u;
        if (n > length - used) n = length - used;

        /* Only the head of an entry is worth holding: the longest key of
         * interest is ALBUMARTIST, and the values are truncated to the tag
         * buffers anyway. Anything past this is skipped in place. */
        char e[80];
        uint32_t keep = n < sizeof(e) ? n : (uint32_t)sizeof(e);
        for (uint32_t i = 0; i < keep; i++) e[i] = (char)bits(f, 8);
        for (uint32_t i = keep; i < n; i++) (void)bits(f, 8);
        used += n;

        const char *v = e;
        uint32_t    vn = keep;
        if      (key_is(e, keep, "TITLE"))       { v += 6; vn = keep - 6u;
                                                   tag_copy(f->tag_title,  f->tag_cap, v, vn); }
        else if (key_is(e, keep, "ARTIST"))      { v += 7; vn = keep - 7u;
                                                   tag_copy(f->tag_artist, f->tag_cap, v, vn); }
        else if (key_is(e, keep, "ALBUM"))       { v += 6; vn = keep - 6u;
                                                   tag_copy(f->tag_album,  f->tag_cap, v, vn); }
        else if (key_is(e, keep, "DATE"))        { v += 5; vn = keep - 5u;
                                                   tag_copy(f->tag_year,   8u, v, vn > 4u ? 4u : vn); }
        else if (key_is(e, keep, "TRACKNUMBER")) { v += 12; vn = keep - 12u;
                                                   tag_copy(f->tag_trk,    8u, v, vn); }
    }
    for (; used < length; used++) (void)bits(f, 8);
}

flac_err flac_open(flac_t *f, flac_read_fn read, void *ctx,
                   int32_t *ch0, uint32_t ch0_cap)
{
    /* The caller sets the tag pointers before this call; zeroing the struct
     * would throw them away, so they are carried across. */
    char *t_ti = f->tag_title,  *t_ar = f->tag_artist, *t_al = f->tag_album;
    char *t_yr = f->tag_year,   *t_tk = f->tag_trk;
    uint32_t t_cap = f->tag_cap;

    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    f->read = read; f->ctx = ctx; f->ch0 = ch0; f->ch0_cap = ch0_cap;
    f->tag_title = t_ti; f->tag_artist = t_ar; f->tag_album = t_al;
    f->tag_year  = t_yr; f->tag_trk    = t_tk; f->tag_cap   = t_cap;

    if (bits(f, 32) != 0x664C6143u) return FLAC_ERR_MAGIC;   /* "fLaC" */

    int last = 0;
    while (!last) {
        last            = (int)bits(f, 1);
        uint32_t type   = bits(f, 7);
        uint32_t length = bits(f, 24);

        if (type == 0) {                                     /* STREAMINFO */
            if (length < 34u) return FLAC_ERR_STREAMINFO;
            f->min_blocksize = bits(f, 16);
            f->max_blocksize = bits(f, 16);
            (void)bits(f, 24);                               /* min frame  */
            (void)bits(f, 24);                               /* max frame  */
            f->rate     = bits(f, 20);
            f->channels = (uint8_t)(bits(f, 3) + 1u);
            f->bps      = (uint8_t)(bits(f, 5) + 1u);
            /* 36-bit total sample count, in two reads. */
            uint32_t hi = bits(f, 4), lo = bits(f, 32);
            f->total_samples = ((uint64_t)hi << 32) | lo;
            for (uint32_t i = 0; i < 16u; i++) (void)bits(f, 8);  /* MD5 */
            for (uint32_t i = 34u; i < length; i++) (void)bits(f, 8);

            if (!f->rate || f->channels < 1u ||
                f->channels > FLAC_MAX_CHANNELS) return FLAC_ERR_UNSUPPORTED;
            if (f->bps != 16u && f->bps != 24u && f->bps != 8u &&
                f->bps != 20u) return FLAC_ERR_UNSUPPORTED;
            if (f->max_blocksize > ch0_cap) return FLAC_ERR_UNSUPPORTED;
        } else if (type == 4u) {                             /* VORBIS_COMMENT */
            vorbis_comments(f, length);
        } else {
            for (uint32_t i = 0; i < length; i++) (void)bits(f, 8);
        }
        if (f->eof) return FLAC_ERR_SHORT;
    }
    return f->rate ? FLAC_OK : FLAC_ERR_STREAMINFO;
}

/* --------------------------------------------------------------- frames */

static const uint16_t blk_tab[16] = {
    0, 192, 576, 1152, 2304, 4608, 0, 0,
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768
};

/* CRC-8 over the frame header, poly x^8+x^2+x^1+x^0 (0x07), init 0.
 *
 * Bitwise rather than a 256-byte table: a header is ~6 bytes, so this runs
 * about 48 iterations per frame against 256 bytes of rodata that would have to
 * come out of ~1.8 KB of remaining link slack. */
static uint8_t flac_crc8(const uint8_t *p, uint32_t n)
{
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (uint32_t b = 0; b < 8u; b++)
            c = (uint8_t)((c & 0x80u) ? ((c << 1) ^ 0x07u) : (c << 1));
    }
    return c;
}

/* Parses a header at the CURRENT byte-aligned position and VERIFIES its CRC.
 *
 * The bit reader cannot rewind, so a rejected candidate leaves the position
 * wherever parsing stopped -- which is fine: the caller simply keeps scanning,
 * and the next real frame is found from there.
 */
static flac_err parse_frame_header(flac_t *f)
{
    uint8_t  h[16];
    uint32_t n = 0;

#define HBYTE() do { if (!need(f, 8)) return FLAC_ERR_SHORT;                                        h[n++] = (uint8_t)bits(f, 8); } while (0)

    HBYTE(); HBYTE(); HBYTE(); HBYTE();          /* sync, bs/sr, ch/size */

    uint32_t bs_code = (uint32_t)(h[2] >> 4);
    uint32_t sr_code = (uint32_t)(h[2] & 0x0Fu);
    f->ch_mode       = (uint8_t)(h[3] >> 4);

    /* UTF-8 coded frame or sample number: leading ones give the length. */
    HBYTE();
    uint8_t  c = h[n - 1u];
    uint32_t extra = 0;
    if      ((c & 0x80u) == 0x00u) extra = 0;
    else if ((c & 0xE0u) == 0xC0u) extra = 1;
    else if ((c & 0xF0u) == 0xE0u) extra = 2;
    else if ((c & 0xF8u) == 0xF0u) extra = 3;
    else if ((c & 0xFCu) == 0xF8u) extra = 4;
    else if ((c & 0xFEu) == 0xFCu) extra = 5;
    else if ((c & 0xFFu) == 0xFEu) extra = 6;
    else return FLAC_ERR_DATA;                   /* 0xFF is not a valid lead */
    for (uint32_t i = 0; i < extra; i++) HBYTE();

    uint32_t bsi = n;
    if      (bs_code == 6u) { HBYTE(); }
    else if (bs_code == 7u) { HBYTE(); HBYTE(); }
    if      (sr_code == 12u) { HBYTE(); }
    else if (sr_code == 13u || sr_code == 14u) { HBYTE(); HBYTE(); }
#undef HBYTE

    if (!need(f, 8)) return FLAC_ERR_SHORT;
    uint8_t stored = (uint8_t)bits(f, 8);
    if (flac_crc8(h, n) != stored) return FLAC_ERR_DATA;

    if      (bs_code == 6u) f->blocksize = (uint32_t)h[bsi] + 1u;
    else if (bs_code == 7u) f->blocksize = (((uint32_t)h[bsi] << 8)
                                            | (uint32_t)h[bsi + 1u]) + 1u;
    else                    f->blocksize = blk_tab[bs_code];

    if (!f->blocksize || f->blocksize > f->ch0_cap) return FLAC_ERR_DATA;
    return FLAC_OK;
}

/* Finds the next frame header.
 *
 * During sequential playback frames abut, so this never actually searches --
 * which is why playback was clean while SEEKING was not. Seeking is the only
 * path that lands mid-stream and has to look, and the search had no real
 * defence: any 14-bit sync pattern at any bit offset was accepted, with only a
 * blocksize range check behind it.
 *
 * Measured on the test files: scanning forward for the bit pattern alone hits
 * a FALSE sync inside the audio data within a handful of frames, every time.
 * That is what a seek was locking onto, and the garbage that followed is what
 * "seek doesn't work well" was.
 *
 * Two defences, both cheap:
 *   - frames are byte-ALIGNED, so a candidate at any other offset is false by
 *     construction. Also lets the scan step a byte at a time instead of a bit.
 *   - the header carries a CRC-8, which was being read and discarded.
 */
static flac_err frame_header(flac_t *f)
{
    uint32_t tries = 0;
    for (;;) {
        if (!need(f, 15)) return FLAC_ERR_SHORT;

        if ((f->bitcnt & 7u) != 0u) {            /* re-align, never mid-byte */
            f->bitcnt -= (f->bitcnt & 7u);
            continue;
        }

        uint32_t peek = (uint32_t)((f->bitacc >> (f->bitcnt - 15u)) & 0x7FFFu);
        if ((peek >> 1) == 0x3FFEu) {
            flac_err e = parse_frame_header(f);
            if (e == FLAC_OK)        return FLAC_OK;
            if (e == FLAC_ERR_SHORT) return e;
            /* CRC or fields rejected it. Position is wherever parsing
             * stopped; carry on looking from there. */
        } else {
            f->bitcnt -= 8u;                     /* next byte */
        }

        if (++tries > (1u << 20)) return FLAC_ERR_SYNC;
    }
}

/* ------------------------------------------------------------- residual */

/* Rice residuals, one at a time.
 *
 * The array-filling version below is fine for the buffered channel, but the
 * STREAMED channel has to interleave residual decoding with reconstruction
 * and output -- so it needs to pull values singly. Partition state is small:
 * which partition, how many are left in it, and its parameter. */
typedef struct {
    uint32_t pbits, escape, parts, part, left, param, raw, order, per;
} rice_t;

static flac_err rice_init(flac_t *f, rice_t *r, uint32_t order)
{
    uint32_t method = bits(f, 2);
    if (method > 1u) return FLAC_ERR_DATA;
    r->pbits  = method ? 5u : 4u;
    r->escape = method ? 31u : 15u;
    uint32_t porder = bits(f, 4);
    r->parts = 1u << porder;
    if (f->blocksize % r->parts) return FLAC_ERR_DATA;
    r->per   = f->blocksize / r->parts;
    r->order = order;
    r->part  = 0;
    r->left  = 0;
    return FLAC_OK;
}

static int32_t rice_next(flac_t *f, rice_t *r)
{
    if (!r->left) {                                   /* open next partition */
        if (r->part >= r->parts) return 0;
        r->left  = r->per - (r->part == 0u ? r->order : 0u);
        r->param = bits(f, r->pbits);
        r->raw   = (r->param == r->escape) ? bits(f, 5) : 0u;
        r->part++;
        if (!r->left) return rice_next(f, r);          /* empty partition */
    }
    r->left--;
    if (r->param == r->escape) return sbits(f, r->raw);
    uint32_t q = unary(f);
    uint32_t v = (q << r->param) | bits(f, r->param);
    return (v & 1u) ? -(int32_t)((v >> 1) + 1u) : (int32_t)(v >> 1);
}


/* Decodes `n` residuals starting at out[0]. Rice partitions are flat in the
 * bitstream, so this can run straight into the caller's reconstruction. */
static flac_err residual(flac_t *f, uint32_t order, int32_t *out)
{
    uint32_t method = bits(f, 2);
    if (method > 1u) return FLAC_ERR_DATA;
    uint32_t pbits  = method ? 5u : 4u;
    uint32_t escape = method ? 31u : 15u;

    uint32_t porder = bits(f, 4);
    uint32_t parts  = 1u << porder;
    if (f->blocksize % parts) return FLAC_ERR_DATA;

    uint32_t idx = 0;
    for (uint32_t p = 0; p < parts; p++) {
        uint32_t count = f->blocksize / parts - (p == 0u ? order : 0u);
        uint32_t param = bits(f, pbits);
        if (param == escape) {
            uint32_t raw = bits(f, 5);
            for (uint32_t i = 0; i < count; i++) out[idx++] = sbits(f, raw);
        } else {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t q = unary(f);
                uint32_t r = bits(f, param);
                uint32_t v = (q << param) | r;
                /* zig-zag: LSB is the sign */
                out[idx++] = (v & 1u) ? -(int32_t)((v >> 1) + 1u)
                                      :  (int32_t)(v >> 1);
            }
        }
        if (f->eof) return FLAC_ERR_SHORT;
    }
    return FLAC_OK;
}

/* ------------------------------------------------------------- subframe */

static const int8_t fixed_coef[5][4] = {
    { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 2, -1, 0, 0 },
    { 3, -3, 1, 0 }, { 4, -6, 4, -1 }
};

/* Decodes one subframe into `out` (blocksize entries). `bps` is this
 * subframe's sample size, which is one bit wider than the frame's for the
 * side channel of a decorrelated pair. */

/* ---- optional profiling ------------------------------------------------
 *
 * D0 says the decoder is over budget but not by HOW MUCH -- it clips at zero,
 * and "needs 1.2x" and "needs 4x" are the difference between a fixable problem
 * and one to document instead. These split channel 0's decode into its two
 * passes so the effort goes where the cycles are.
 *
 * Channel 0 specifically, because subframe() runs residual and reconstruction
 * as SEPARATE passes and they can be timed apart. subframe_stream() interleaves
 * them by design and cannot be split; channel 1 does the same work, so the
 * ratio carries.
 *
 * The hook is a function pointer so the decoder keeps no firmware dependency
 * and the host tests leave it null. Two calls per pass per frame -- roughly
 * ten a second, against 88200 samples. FLAC_PROFILE 0 removes it entirely. */
#ifndef FLAC_PROFILE
#define FLAC_PROFILE 0     /* 1 to re-measure; must be 0 in a shipped build */
#endif

#if FLAC_PROFILE
uint32_t (*flac_tick)(void);
uint32_t flac_res_cyc, flac_lpc_cyc;
uint8_t  flac_order, flac_type;
#define PROF_T0()   uint32_t prof_t0 = flac_tick ? flac_tick() : 0u
#define PROF_ADD(A) do { if (flac_tick) (A) += flac_tick() - prof_t0; } while (0)
#else
#define PROF_T0()   do {} while (0)
#define PROF_ADD(A) do {} while (0)
#endif

static flac_err subframe(flac_t *f, int32_t *out, uint32_t bps)
{
    (void)bits(f, 1);                       /* zero bit */
    uint32_t type  = bits(f, 6);
    uint32_t wasted = 0;
    if (bits(f, 1)) wasted = unary(f) + 1u;
    bps -= wasted;

    uint32_t n = f->blocksize;

    if (type == 0u) {                       /* CONSTANT */
        int32_t v = sbits(f, bps);
        for (uint32_t i = 0; i < n; i++) out[i] = v;
    } else if (type == 1u) {                /* VERBATIM */
        for (uint32_t i = 0; i < n; i++) out[i] = sbits(f, bps);
    } else if (type >= 8u && type <= 12u) { /* FIXED, order 0..4 */
        uint32_t order = type - 8u;
        for (uint32_t i = 0; i < order; i++) out[i] = sbits(f, bps);
#if FLAC_PROFILE
        flac_order = (uint8_t)order; flac_type = 1u;
#endif
        { PROF_T0();
        flac_err e = residual(f, order, out + order);
        PROF_ADD(flac_res_cyc);
        if (e) return e; }
        const int8_t *c = fixed_coef[order];
        PROF_T0();
        for (uint32_t i = order; i < n; i++) {
            int32_t p = 0;
            for (uint32_t j = 0; j < order; j++) p += c[j] * out[i - 1u - j];
            out[i] += p;
        }
        PROF_ADD(flac_lpc_cyc);
    } else if (type >= 32u) {               /* LPC, order 1..32 */
        uint32_t order = type - 31u;
        for (uint32_t i = 0; i < order; i++) out[i] = sbits(f, bps);
        uint32_t prec  = bits(f, 4) + 1u;
        int32_t  shift = sbits(f, 5);
        if (prec == 16u || shift < 0) return FLAC_ERR_DATA;
        int32_t coef[FLAC_MAX_ORDER];
        for (uint32_t i = 0; i < order; i++) coef[i] = sbits(f, prec);
#if FLAC_PROFILE
        flac_order = (uint8_t)order; flac_type = 2u;
#endif
        { PROF_T0();
        flac_err e = residual(f, order, out + order);
        PROF_ADD(flac_res_cyc);
        if (e) return e; }
        PROF_T0();
        for (uint32_t i = order; i < n; i++) {
            int64_t p = 0;
            for (uint32_t j = 0; j < order; j++)
                p += (int64_t)coef[j] * out[i - 1u - j];
            out[i] += (int32_t)(p >> shift);
        }
        PROF_ADD(flac_lpc_cyc);
    } else {
        return FLAC_ERR_DATA;
    }

    if (wasted) for (uint32_t i = 0; i < n; i++) out[i] <<= wasted;
    return FLAC_OK;
}

/* Scales a decoded sample of `bps` bits down to 16 for the DAC. The output
 * is 16/48 whatever the source, so this is not a quality choice -- it is the
 * interface. */
static int16_t to16(int32_t v, uint32_t bps)
{
    if (bps > 16u) v >>= (int)(bps - 16u);
    else if (bps < 16u) v <<= (int)(16u - bps);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* Decodes the SECOND channel while consuming the first from the same array,
 * emitting decorrelated pairs as it goes.
 *
 * This is what makes the decoder fit. buf[i] holds channel 0 until the instant
 * channel 1's sample i is reconstructed -- so reading it and then writing
 * channel 1 over it costs nothing, and channel 1's own LPC history is exactly
 * those overwritten entries. One blocksize buffer instead of two: 18432 bytes
 * for the 4608-sample blocks on the test card, not 36864.
 */
static flac_err subframe_stream(flac_t *f, uint32_t bps, uint32_t out_bps,
                                uint8_t m, flac_sink_fn sink, void *sctx)
{
    int32_t *buf = f->ch0;
    uint32_t n = f->blocksize;

    (void)bits(f, 1);
    uint32_t type = bits(f, 6);
    uint32_t wasted = 0;
    if (bits(f, 1)) wasted = unary(f) + 1u;
    bps -= wasted;

    int16_t pcm[128];
    uint32_t k = 0;
    uint32_t i = 0;

    /* Emits one pair and hands the buffer slot over to channel 1. */
    #define EMIT(S) do {                                                           int32_t a_ = buf[i], s_ = (S), l_, r_;                                     if      (m == 8u)  { l_ = a_;             r_ = a_ - s_; }                  else if (m == 9u)  { r_ = s_;             l_ = s_ + a_; }                  else if (m == 10u) { int32_t mid_ = (a_ << 1) | (s_ & 1);                                       l_ = (mid_ + s_) >> 1;                                                     r_ = (mid_ - s_) >> 1; }                              else               { l_ = a_;             r_ = s_; }                       pcm[k * 2] = to16(l_, out_bps); pcm[k * 2 + 1] = to16(r_, out_bps);        buf[i] = s_;                                                               if (++k == 64u) { sink(sctx, pcm, k); k = 0; }                             i++;                                                                   } while (0)

    if (type == 0u) {                              /* CONSTANT */
        int32_t v = sbits(f, bps) << wasted;
        while (i < n) EMIT(v);
    } else if (type == 1u) {                       /* VERBATIM */
        while (i < n) EMIT(sbits(f, bps) << wasted);
    } else if (type >= 8u && type <= 12u) {        /* FIXED */
        uint32_t order = type - 8u;
        int32_t warm[4];
        for (uint32_t j = 0; j < order; j++) warm[j] = sbits(f, bps);
        rice_t r; flac_err e = rice_init(f, &r, order);
        if (e) return e;
        for (uint32_t j = 0; j < order; j++) EMIT(warm[j] << wasted);
        const int8_t *c = fixed_coef[order];
        while (i < n) {
            int32_t p = 0;
            for (uint32_t j = 0; j < order; j++) p += c[j] * (buf[i - 1u - j] >> wasted);
            EMIT((rice_next(f, &r) + p) << wasted);
        }
    } else if (type >= 32u) {                      /* LPC */
        uint32_t order = type - 31u;
        int32_t warm[FLAC_MAX_ORDER];
        for (uint32_t j = 0; j < order; j++) warm[j] = sbits(f, bps);
        uint32_t prec  = bits(f, 4) + 1u;
        int32_t  shift = sbits(f, 5);
        if (prec == 16u || shift < 0) return FLAC_ERR_DATA;
        int32_t coef[FLAC_MAX_ORDER];
        for (uint32_t j = 0; j < order; j++) coef[j] = sbits(f, prec);
        rice_t r; flac_err e = rice_init(f, &r, order);
        if (e) return e;
        for (uint32_t j = 0; j < order; j++) EMIT(warm[j] << wasted);
        while (i < n) {
            int64_t p = 0;
            for (uint32_t j = 0; j < order; j++)
                p += (int64_t)coef[j] * (buf[i - 1u - j] >> wasted);
            EMIT((rice_next(f, &r) + (int32_t)(p >> shift)) << wasted);
        }
    } else {
        return FLAC_ERR_DATA;
    }
    #undef EMIT

    if (k) sink(sctx, pcm, k);
    return f->eof ? FLAC_ERR_SHORT : FLAC_OK;
}

/* ---------------------------------------------------------------- frame */


/* Drops buffered input and all bit state, keeping STREAMINFO and the caller's
 * block buffer.
 *
 * For seeking. The caller moves the underlying stream, then calls this; the
 * next frame_header() sync scan starts clean at the new position. Without it
 * the decoder carries up to 512 buffered bytes and a partial bit reservoir
 * from the OLD offset, decodes them as though they belonged at the new one,
 * and never recovers -- which presented as a hang.
 *
 * Not flac_restart(): that reopens from byte 0 and re-reads the metadata,
 * which is right for a track restart and wrong for a seek. */
void flac_flush_input(flac_t *f)
{
    f->have = 0; f->pos = 0;
    f->bitacc = 0; f->bitcnt = 0;
    f->eof = 0;
}

flac_err flac_decode_frame(flac_t *f, flac_sink_fn sink, void *sink_ctx)
{
    if (f->eof && f->pos >= f->have) return FLAC_END;

    flac_err e = frame_header(f);
    if (e) return (f->eof && e == FLAC_ERR_SHORT) ? FLAC_END : e;

    uint32_t n = f->blocksize, bps = f->bps;
    uint8_t  m = f->ch_mode;

    /* Channel 0 is buffered because decorrelation needs it alongside channel
     * 1, which FLAC stores afterwards rather than interleaved. */
    uint32_t bps0 = bps + ((m == 9u) ? 1u : 0u);   /* right/side: side first */
    e = subframe(f, f->ch0, bps0);
    if (e) return e;

    if (f->channels == 1u) {
        int16_t pcm[128];
        for (uint32_t i = 0; i < n; ) {
            uint32_t k = 0;
            while (k < 64u && i < n) { int16_t s = to16(f->ch0[i++], bps);
                                       pcm[k * 2] = s; pcm[k * 2 + 1] = s; k++; }
            sink(sink_ctx, pcm, k);
        }
        align_byte(f);
        (void)bits(f, 16);                          /* frame CRC-16 */
        return FLAC_OK;
    }

    /* Channel 1 never gets a buffer of its own -- see subframe_stream(). */
    uint32_t bps1 = bps + ((m == 8u || m == 10u) ? 1u : 0u);
    e = subframe_stream(f, bps1, bps, m, sink, sink_ctx);
    if (e) return e;

    align_byte(f);
    (void)bits(f, 16);                              /* frame CRC-16 */
    return FLAC_OK;
}

const char *flac_strerror(flac_err e)
{
    switch (e) {
    case FLAC_OK:              return "ok";
    case FLAC_END:             return "end of stream";
    case FLAC_ERR_MAGIC:       return "not a FLAC file";
    case FLAC_ERR_STREAMINFO:  return "bad STREAMINFO";
    case FLAC_ERR_UNSUPPORTED: return "unsupported format";
    case FLAC_ERR_SYNC:        return "lost frame sync";
    case FLAC_ERR_DATA:        return "malformed subframe";
    case FLAC_ERR_SHORT:       return "input ended mid-frame";
    }
    return "?";
}
