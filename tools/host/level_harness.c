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

/* Accumulators over a 1152-PAIR window.
 *
 * The window matters more than anything else here and the first attempt got it
 * wrong: measuring whole 104 ms FLAC frames says almost nothing about a bar
 * that is published every display frame. meters_feed() runs per emitted chunk
 * and peak_acc is published once per ~26 ms, so 1152 pairs is the window the
 * meter actually sees. A maximum over a longer window is always larger, which
 * would have flattered PEAK and hidden exactly the pegging being investigated. */
#define WIN_PAIRS 1152u

static uint32_t w_peak, w_pairs, w_rows;
static uint64_t w_abs, w_sq;

static void win_flush(void)
{
    if (!w_pairs) return;
    uint32_t n = w_pairs * 2u;
    hputu(++w_rows);
    hputc(' '); hputu(w_peak);
    hputc(' '); hputu((uint32_t)(w_abs >> 32));
    hputc(' '); hputu((uint32_t)(w_abs & 0xFFFFFFFFu));
    hputc(' '); hputu((uint32_t)(w_sq >> 32));
    hputc(' '); hputu((uint32_t)(w_sq & 0xFFFFFFFFu));
    hputc(' '); hputu(n);
    hnl();
    w_peak = 0; w_abs = 0; w_sq = 0; w_pairs = 0;
}

static void sink(void *ctx, const int16_t *pcm, uint32_t frames)
{
    (void)ctx;
    for (uint32_t p = 0; p < frames; p++) {
        for (uint32_t c = 0; c < 2u; c++) {
            int32_t v = pcm[p * 2u + c];
            if (v < 0) v = -v;
            if ((uint32_t)v > w_peak) w_peak = (uint32_t)v;
            w_abs += (uint32_t)v;
            w_sq  += (uint64_t)((uint32_t)v * (uint32_t)v);
        }
        if (++w_pairs >= WIN_PAIRS) win_flush();
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

    hputs("win peak abs_hi abs_lo sq_hi sq_lo n"); hnl();
    for (int i = 0; i < 14; i++)
        if (flac_decode_frame(&f, sink, 0) != FLAC_OK) break;
    win_flush();
    hputs("DONE"); hnl();
    return 0;
}
