/* Runs the SHIPPING FLAC decoder over a whole file under tools/rv32sim.py.
 *
 * Two jobs, and the first one is what makes the second safe:
 *
 *   PROOF   A FLAC file carries an MD5 of its original PCM. This harness
 *           CRC32s the decoder's output and tools/host/flac_bench.py compares
 *           that against a Python decode already verified against the MD5. So
 *           an optimisation either reproduces the audio exactly or it fails
 *           here, over whole tracks, rather than being judged by ear.
 *
 *   COST    Instructions retired, total and split between the residual pass
 *           and the reconstruction pass, via fw/flac.c's own FLAC_PROFILE
 *           hooks. That is the measurement that says whether making the
 *           predictor loop faster is worth anything before it is written.
 *
 * Built by flac_bench.py, which compiles fw/flac.c itself -- there is no copy
 * of the decoder here to drift.
 */
#include "hostio.h"
#include "../../fw/flac.h"

/* ---- file as a stream ---------------------------------------------------- */
static uint32_t rd_pos;

static int rd(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    uint32_t got = hread(rd_pos, dst, (uint32_t)n);
    rd_pos += got;
    return (int)got;
}

static int sk(void *ctx, uint32_t n)
{
    (void)ctx;
    rd_pos += n;
    return 1;
}

/* ---- CRC32 over the PCM, byte for byte as Python sees it ----------------- */
static uint32_t crc_tab[256];
static uint32_t crc = 0xFFFFFFFFu;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}

static void crc_add(const uint8_t *b, uint32_t n)
{
    uint32_t c = crc;
    while (n--) c = crc_tab[(c ^ *b++) & 0xFFu] ^ (c >> 8);
    crc = c;
}

static uint32_t total_frames, total_samples;

static void sink(void *ctx, const int16_t *pcm, uint32_t frames)
{
    (void)ctx;
    /* little-endian int16 pairs -- array('h').tobytes() on the Python side */
    crc_add((const uint8_t *)pcm, frames * 2u * sizeof(int16_t));
    total_samples += frames;
}

/* How much of the track to run. A whole album track is billions of simulated
 * instructions; a few hundred frames is ~1M samples, exercises every subframe
 * type the file uses, and finishes in a minute. Raise it for a long soak. */
#ifndef MAX_FRAMES
#define MAX_FRAMES 200u
#endif

static int32_t ch0[4608 * 2];        /* max blocksize this harness accepts */
static flac_t  f;

int main(void)
{
    crc_init();
    rd_pos = 0;
#if FLAC_PROFILE
    flac_tick = hticks;
#endif

    f.skip = sk;
    flac_err e = flac_open(&f, rd, 0, ch0, (uint32_t)(sizeof(ch0) / sizeof(ch0[0])));
    if (e != FLAC_OK) { hputs("OPEN_ERR "); hputu((uint32_t)e); hnl(); hexit(1); }

    hputs("rate ");  hputu(f.rate);
    hputs(" ch ");   hputu(f.channels);
    hputs(" bps ");  hputu(f.bps);
    hputs(" blk ");  hputu(f.max_blocksize);
    hnl();

    uint32_t t0 = hticks();
    while (total_frames < MAX_FRAMES) {
        e = flac_decode_frame(&f, sink, 0);
        if (e != FLAC_OK) break;
        total_frames++;
    }
    uint32_t spent = hticks() - t0;

    hputs("frames ");   hputu(total_frames);
    hputs(" samples "); hputu(total_samples);
    hputs(" last_err "); hputu((uint32_t)e);
    hnl();
    hputs("crc32 ");    hputx(crc ^ 0xFFFFFFFFu);
    hnl();
    hputs("instr_total ");  hputu(spent);
#if FLAC_PROFILE
    hputs(" instr_residual "); hputu(flac_res_cyc);
    hputs(" instr_predict ");  hputu(flac_lpc_cyc);
#endif
    hnl();
    hexit(0);
    return 0;
}
