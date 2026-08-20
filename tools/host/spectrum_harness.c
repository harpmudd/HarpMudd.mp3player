/* Measures what the octave cascade actually produces on real music.
 *
 * The first gain table was set by eye and only the bottom row of the meter
 * ever lit -- the values were more than an order of magnitude too small. This
 * runs the SAME cascade over real audio decoded by the real fw/flac.c, and
 * reports each band's mean magnitude, so the gains can be computed instead of
 * guessed. Guessing has already cost one hardware trip.
 *
 * Prints, per band, the mean |band| in sample units and what the meter would
 * currently show for it, so the two are comparable directly.
 */
#include "hostio.h"
#include "../../fw/flac.h"

#define SPEC_OCT   8u
#define SPEC_BANDS (SPEC_OCT * 2u)
#define SPEC_SH    1u
#define SPEC_SH2   2u
#define LED_ROWS   12u

static int32_t  lp[SPEC_OCT], slp[SPEC_OCT];
static uint32_t cnt[SPEC_OCT];
static uint64_t acc[SPEC_BANDS];
static uint32_t nsamp;

static uint32_t cur;

static int rd(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    uint32_t got = hread(cur, dst, (uint32_t)n);
    cur += got;
    return (int)got;
}

static void sink(void *ctx, const int16_t *pcm, uint32_t frames)
{
    (void)ctx;
    for (uint32_t p = 0; p < frames; p++) {
        int32_t x = ((int32_t)pcm[p * 2u] + (int32_t)pcm[p * 2u + 1u]) >> 1;
        for (uint32_t o = 0; o < SPEC_OCT; o++) {
            lp[o] += (x - lp[o]) >> SPEC_SH;
            int32_t hp = x - lp[o];
            slp[o] += (hp - slp[o]) >> SPEC_SH2;
            int32_t sl = slp[o], sh = hp - sl;
            acc[o * 2u]      += (uint32_t)(sh < 0 ? -sh : sh);
            acc[o * 2u + 1u] += (uint32_t)(sl < 0 ? -sl : sl);
            if (++cnt[o] & 1u) break;
            x = lp[o];
        }
    }
    nsamp += frames;
}

static int32_t ch0[8192];
static uint8_t mb[4];

static uint32_t first_audio_byte(void)
{
    uint32_t off = 4;
    for (;;) {
        if (hread(off, mb, 4) != 4u) return 0;
        uint32_t len = ((uint32_t)mb[1] << 16) | ((uint32_t)mb[2] << 8) | mb[3];
        uint32_t last = mb[0] >> 7;
        off += 4u + len;
        if (last) return off;
    }
}

int main(void)
{
    uint32_t fsize = hfilesize();
    uint32_t first = first_audio_byte();

    flac_t f;
    memset(&f, 0, sizeof f);
    cur = 0;
    if (flac_open(&f, rd, 0, ch0, 8192u) != FLAC_OK) {
        hputs("OPEN FAILED"); hnl(); return 1;
    }

    /* A third of the way in: intros are quiet and would understate every
     * band, which is the mistake being corrected. */
    cur = first + (fsize - first) / 3u;
    flac_flush_input(&f);

    for (int i = 0; i < 10; i++)
        if (flac_decode_frame(&f, sink, 0) != FLAC_OK) break;

    hputs("band mean_mag  cur_v  cur_rows"); hnl();
    static const unsigned short gain[SPEC_BANDS] = {
        345u, 345u, 221u, 221u, 183u, 183u, 196u, 196u,
        242u, 242u, 329u, 329u, 479u, 479u, 749u, 749u };
    for (uint32_t b = 0; b < SPEC_BANDS; b++) {
        uint32_t c = nsamp >> (b / 2u);
        uint32_t mean = c ? (uint32_t)(acc[b] / c) : 0u;
        uint32_t v = (mean * gain[SPEC_BANDS - 1u - b]) >> 4;
        v = (v * 255u) / 32768u;
        if (v > 255u) v = 255u;
        hputu(b);
        hputs("    "); hputu(mean);
        hputs("      "); hputu(v);
        hputs("      "); hputu((v * LED_ROWS) / 256u);
        hnl();
    }
    hputs("samples "); hputu(nsamp); hnl();
    hputs("DONE"); hnl();
    return 0;
}
