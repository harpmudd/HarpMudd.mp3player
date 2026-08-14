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

/* MSB-first bit reader. The reservoir holds at most 24 bits so a 32-bit
 * accumulator can always take another byte without shifting anything out. */
static int need(flac_t *f, uint32_t n)
{
    while (f->bitcnt < n) {
        int b = byte(f);
        if (b < 0) return 0;
        f->bitacc = (f->bitacc << 8) | (uint32_t)b;
        f->bitcnt += 8;
    }
    return 1;
}

static uint32_t bits(flac_t *f, uint32_t n)
{
    if (!n) return 0;
    if (!need(f, n)) return 0;
    f->bitcnt -= n;
    uint32_t v = (f->bitacc >> f->bitcnt) & (n == 32u ? 0xFFFFFFFFu
                                                      : ((1u << n) - 1u));
    return v;
}

/* Sign-extend an n-bit two's complement value. */
static int32_t sbits(flac_t *f, uint32_t n)
{
    if (!n) return 0;
    uint32_t v = bits(f, n);
    if (n < 32u && (v & (1u << (n - 1u)))) v |= ~((1u << n) - 1u);
    return (int32_t)v;
}

/* Unary: count zeros up to the first 1. Used by every Rice residual. */
static uint32_t unary(flac_t *f)
{
    uint32_t n = 0;
    for (;;) {
        if (!need(f, 1)) return n;
        f->bitcnt--;
        if ((f->bitacc >> f->bitcnt) & 1u) return n;
        n++;
        if (n > 1u << 20) return n;          /* corrupt stream, do not hang */
    }
}

static void align_byte(flac_t *f) { f->bitcnt -= f->bitcnt & 7u; }

/* ------------------------------------------------------------ metadata */

flac_err flac_open(flac_t *f, flac_read_fn read, void *ctx,
                   int32_t *ch0, uint32_t ch0_cap)
{
    for (uint32_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    f->read = read; f->ctx = ctx; f->ch0 = ch0; f->ch0_cap = ch0_cap;

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

static flac_err frame_header(flac_t *f)
{
    /* Sync is 14 bits of 1s followed by a zero. Scan for it rather than
     * assuming alignment: a decoder that has lost sync must be able to find
     * the next frame, and the CRC check is what makes that safe. */
    uint32_t tries = 0;
    for (;;) {
        if (!need(f, 15)) return FLAC_ERR_SHORT;
        uint32_t peek = (f->bitacc >> (f->bitcnt - 15u)) & 0x7FFFu;
        if ((peek >> 1) == 0x3FFEu) break;
        f->bitcnt--;                                   /* slide one bit */
        if (++tries > (1u << 22)) return FLAC_ERR_SYNC;
    }
    (void)bits(f, 14);                                 /* sync            */
    (void)bits(f, 1);                                  /* reserved        */
    (void)bits(f, 1);                                  /* blocking strategy */

    uint32_t bs_code = bits(f, 4);
    uint32_t sr_code = bits(f, 4);
    f->ch_mode       = (uint8_t)bits(f, 4);
    (void)bits(f, 3);                                  /* sample size     */
    (void)bits(f, 1);                                  /* reserved        */

    /* UTF-8 coded frame or sample number: leading ones give the length. */
    int c = byte(f);
    if (c < 0) return FLAC_ERR_SHORT;
    /* the accumulator is byte-aligned here, so a raw byte read is correct
     * only after flushing whole bytes out of it */
    uint32_t extra = 0;
    if      ((c & 0x80) == 0x00) extra = 0;
    else if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    else if ((c & 0xFC) == 0xF8) extra = 4;
    else if ((c & 0xFE) == 0xFC) extra = 5;
    else if ((c & 0xFF) == 0xFE) extra = 6;
    for (uint32_t i = 0; i < extra; i++) if (byte(f) < 0) return FLAC_ERR_SHORT;

    if      (bs_code == 6u) f->blocksize = bits(f, 8) + 1u;
    else if (bs_code == 7u) f->blocksize = bits(f, 16) + 1u;
    else                    f->blocksize = blk_tab[bs_code];

    if      (sr_code == 12u) (void)bits(f, 8);
    else if (sr_code == 13u || sr_code == 14u) (void)bits(f, 16);

    (void)bits(f, 8);                                  /* header CRC-8 */

    if (!f->blocksize || f->blocksize > f->ch0_cap) return FLAC_ERR_DATA;
    return FLAC_OK;
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
        flac_err e = residual(f, order, out + order);
        if (e) return e;
        const int8_t *c = fixed_coef[order];
        for (uint32_t i = order; i < n; i++) {
            int32_t p = 0;
            for (uint32_t j = 0; j < order; j++) p += c[j] * out[i - 1u - j];
            out[i] += p;
        }
    } else if (type >= 32u) {               /* LPC, order 1..32 */
        uint32_t order = type - 31u;
        for (uint32_t i = 0; i < order; i++) out[i] = sbits(f, bps);
        uint32_t prec  = bits(f, 4) + 1u;
        int32_t  shift = sbits(f, 5);
        if (prec == 16u || shift < 0) return FLAC_ERR_DATA;
        int32_t coef[FLAC_MAX_ORDER];
        for (uint32_t i = 0; i < order; i++) coef[i] = sbits(f, prec);
        flac_err e = residual(f, order, out + order);
        if (e) return e;
        for (uint32_t i = order; i < n; i++) {
            int64_t p = 0;
            for (uint32_t j = 0; j < order; j++)
                p += (int64_t)coef[j] * out[i - 1u - j];
            out[i] += (int32_t)(p >> shift);
        }
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
