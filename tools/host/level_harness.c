/* Measures peak against average level, using the real decoder on real music.
 *
 * The meters are driven by the PEAK of each frame. On modern masters that sits
 * near full scale almost continuously, so the bars pin near the top and stop
 * saying anything about the music. An average moves properly -- but only if
 * the gain is right, and that gain is a property of real audio rather than
 * something to pick by eye and then discover on hardware.
 *
 * So this decodes frames from the middle of an actual track through fw/flac.c
 * and reports, per frame, the three numbers the meter could be built on.
 */
#include "hostio.h"
#include "../../fw/flac.h"

static uint32_t cur;

static int rd(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    uint32_t got = hread(cur, dst, (uint32_t)n);
    cur += got;
    return (int)got;
}

/* Per-frame accumulators, reset by the caller between frames. */
static uint32_t f_peak;
static uint64_t f_abs, f_sq;
static uint32_t f_n;

static void sink(void *ctx, const int16_t *pcm, uint32_t frames)
{
    (void)ctx;
    for (uint32_t i = 0; i < frames * 2u; i++) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;
        if ((uint32_t)v > f_peak) f_peak = (uint32_t)v;
        f_abs += (uint32_t)v;
        f_sq  += (uint64_t)((uint32_t)v * (uint32_t)v);
        f_n++;
    }
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
        hputs("OPEN FAILED"); hnl();
        return 1;
    }

    /* Start a third of the way in: intros are quiet and would flatter an
     * average-based meter for the wrong reason. */
    cur = first + (fsize - first) / 3u;
    flac_flush_input(&f);

    hputs("frame peak abs_hi abs_lo sq_hi sq_lo n"); hnl();
    int good = 0;
    for (int i = 0; i < 20 && good < 6; i++) {
        f_peak = 0; f_abs = 0; f_sq = 0; f_n = 0;
        if (flac_decode_frame(&f, sink, 0) != FLAC_OK) break;
        if (!f_n) continue;
        good++;
        hputu((uint32_t)good);
        hputc(' '); hputu(f_peak);
        hputc(' '); hputu((uint32_t)(f_abs >> 32));
        hputc(' '); hputu((uint32_t)(f_abs & 0xFFFFFFFFu));
        hputc(' '); hputu((uint32_t)(f_sq >> 32));
        hputc(' '); hputu((uint32_t)(f_sq & 0xFFFFFFFFu));
        hputc(' '); hputu(f_n);
        hnl();
    }
    hputs("DONE"); hnl();
    return 0;
}
