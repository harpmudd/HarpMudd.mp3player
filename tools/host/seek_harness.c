/* Runs the REAL fw/flac.c through the real post-seek scan, under rv32sim.
 *
 * The question this exists to answer: after the player jumps to an
 * interpolated byte offset and calls flac_flush_input(), what does the decoder
 * actually report for the frames that follow? Two fixes were built on beliefs
 * about that and both were wrong, so nothing here is modelled -- the decoder
 * is the shipping one, the file is a real one, and the jump is the same
 * arithmetic the player performs.
 *
 * For each target it prints, per frame: the coded number, whether it is a
 * sample or frame number, the blocksize, the decode result, and the second the
 * number implies. That is the exact set the on-screen diagnostic would have
 * shown, obtained without a hardware round trip.
 */
#include "hostio.h"
#include "../../fw/flac.h"

/* ---- input: a cursor into the file the simulator was given ---------------- */
static uint32_t cur;          /* next byte to serve                          */

static int rd(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    uint32_t got = hread(cur, dst, (uint32_t)n);
    cur += got;
    return (int)got;
}

static void sink(void *ctx, const int16_t *pcm, uint32_t frames)
{
    (void)ctx; (void)pcm; (void)frames;      /* audio is not what is in doubt */
}

static int32_t ch0[8192];
static uint8_t mb[4];

/* Mirrors the metadata walk in player.c: the first audio byte is just past the
 * last metadata block. Done here rather than inferred from the decoder's
 * buffer state so it cannot disagree with the player. */
static uint32_t first_audio_byte(void)
{
    uint32_t off = 4;                        /* past "fLaC"                  */
    for (;;) {
        if (hread(off, mb, 4) != 4u) return 0;
        uint32_t len = ((uint32_t)mb[1] << 16) | ((uint32_t)mb[2] << 8) | mb[3];
        uint32_t last = mb[0] >> 7;
        off += 4u + len;
        if (last) return off;
    }
}

static uint64_t sample_of(const flac_t *f)
{
    return f->number_is_sample ? f->frame_number
                               : f->frame_number * (uint64_t)f->max_blocksize;
}

/* Transcribed from flac_seek_locate() in fw/player.c, line for line.
 *
 * The player itself cannot run here -- it is 8000 lines against hardware
 * registers -- so the SEARCH is duplicated while the decoder underneath it is
 * the real one. That is the part worth checking: whether refining a byte
 * offset against real frame headers converges, and whether the transport can
 * still wedge. The firmware differences are the seektable seed (which only
 * makes the starting bracket tighter) and reading through the card rather
 * than a file.
 *
 * No anchor on the current position: it was tried and helped only the
 * pathological case, while a stale anchor could exclude the true answer from
 * the bracket entirely. The seek-intent guard below covers the same ground
 * without that risk.
 */
static uint32_t seek_locate(flac_t *f, uint64_t want, uint32_t first,
                            uint32_t fsize, uint64_t total,
                            uint64_t *landed, int *iters, int *rejects)
{
    uint32_t lo_b = first, hi_b = fsize;
    uint64_t lo_s = 0, hi_s = total;
    if (want > total) want = total;

    uint32_t best_b = lo_b;
    uint64_t best_s = lo_s;
    uint64_t best_d = (want > lo_s) ? want - lo_s : lo_s - want;
    int it = 0, rej = 0;

    for (; it < 12; it++) {
        if (hi_s <= lo_s || hi_b <= lo_b + 1u) break;
        uint32_t at = lo_b + (uint32_t)(((uint64_t)(hi_b - lo_b) *
                                         (want - lo_s)) / (hi_s - lo_s));
        if (at <= lo_b) at = lo_b + 1u;
        if (at >= hi_b) at = hi_b - 1u;

        cur = at;
        flac_flush_input(f);
        if (flac_probe_frame(f) != FLAC_OK) { hi_b = at; continue; }

        uint64_t got = sample_of(f);
        if (got < lo_s || got > hi_s) { rej++; hi_b = at; continue; }

        uint64_t d = (got > want) ? got - want : want - got;
        if (d < best_d) { best_d = d; best_b = at; best_s = got; }
        if (d <= (uint64_t)f->max_blocksize) { it++; break; }

        if (got < want) { lo_b = at; lo_s = got; }
        else            { hi_b = at; hi_s = got; }
    }
    *landed = best_s;
    *iters = it;
    *rejects = rej;
    return best_b;
}

int main(void)
{
    uint32_t fsize = hfilesize();
    uint32_t first = first_audio_byte();

    flac_t f;
    memset(&f, 0, sizeof f);
    cur = 0;
    flac_err e = flac_open(&f, rd, 0, ch0, 8192u);
    if (e != FLAC_OK) { hputs("OPEN FAILED "); hputu(e); hnl(); return 1; }

    hputs("rate "); hputu(f.rate);
    hputs(" ch "); hputu(f.channels);
    hputs(" bps "); hputu(f.bps);
    hputs(" blk "); hputu(f.min_blocksize); hputc('/'); hputu(f.max_blocksize);
    hputs(" total "); hputu64(f.total_samples);
    hputs(" first "); hputu(first);
    hputs(" size "); hputu(fsize);
    hnl();

    uint32_t dur = f.rate ? (uint32_t)(f.total_samples / f.rate) : 0u;
    uint32_t span = fsize - first;

    static const uint32_t pcts[5] = { 10u, 25u, 50u, 75u, 90u };
    for (int k = 0; k < 5; k++) {
        uint32_t pct = pcts[k];
        uint32_t tgt = dur * pct / 100u;
        /* The same whole-file interpolation flac_seek_byte() falls back to.
         * Deliberately the LEAST accurate of the player's two paths: if the
         * decoder handles a sloppy landing correctly it handles a seektable
         * landing too, and the sloppy one is where the clock error lives. */
        uint32_t at = first + (uint32_t)(((uint64_t)span * pct) / 100u);

        hputs("SEEK pct "); hputu(pct);
        hputs(" tgt "); hputu(tgt);
        hputs(" byte "); hputu(at);
        hnl();

        cur = at;
        flac_flush_input(&f);

        for (int i = 0; i < 3; i++) {
            flac_err fe = flac_decode_frame(&f, sink, 0);
            uint64_t num = f.frame_number;
            uint64_t smp = f.number_is_sample
                         ? num : num * (uint64_t)f.max_blocksize;
            uint32_t pos = f.rate ? (uint32_t)(smp / f.rate) : 0u;

            hputs("  f"); hputu((uint32_t)i);
            hputs(" err "); hputu(fe);
            hputs(" num "); hputu64(num);
            hputs(" issamp "); hputu(f.number_is_sample);
            hputs(" bs "); hputu(f.blocksize);
            hputs(" pos "); hputu(pos);
            hnl();
            if (fe != FLAC_OK) break;
        }
    }

    /* ---- phase 2: repeated forward seeks, as the player performs them ----
     *
     * The player computes its next target from the clock: tgt = ui_sec + step.
     * That is fine while ui_sec is whatever the last seek ASKED for, because
     * then every press advances by exactly step. It stops being fine the
     * moment ui_sec is corrected to where the seek actually LANDED -- if the
     * landing undershoots by nearly a step, the next press starts from there
     * and the two nearly cancel. The clock becomes truthful and the transport
     * becomes unusable, which is a design fault in the correction and not a
     * coding error anywhere.
     *
     * Both are run side by side from the same landings: U is the clock with
     * the correction applied, T is the target the shipped code would have
     * used. Watch the ADVANCE column. */
    hnl();
    hputs("REPEATED FORWARD SEEKS, step 5s"); hnl();
    hputs("press  tgt  byte     landed  advance(corrected)  advance(shipped)");
    hnl();
    {
        uint32_t ui = 0, shipped = 0;
        for (int pr = 0; pr < 8; pr++) {
            uint32_t last = (dur > 3u) ? dur - 3u : 0u;
            uint32_t tgt = ui + 5u;
            if (tgt > last) tgt = last;
            uint32_t at = first + (uint32_t)(((uint64_t)span * tgt) / dur);

            cur = at;
            flac_flush_input(&f);
            flac_err fe = flac_decode_frame(&f, sink, 0);
            uint64_t num = f.frame_number;
            uint64_t smp = f.number_is_sample
                         ? num : num * (uint64_t)f.max_blocksize;
            uint32_t pos = f.rate ? (uint32_t)(smp / f.rate) : 0u;

            hputs("  "); hputu((uint32_t)pr);
            hputs("    "); hputu(tgt);
            hputs("   "); hputu(at);
            hputs("   "); hputu(pos);
            hputs("   err "); hputu(fe);
            hputs("   +"); hputu(pos > ui ? pos - ui : 0u);
            hputs("   +"); hputu(5u);
            hnl();

            ui = pos;                 /* the correction: trust the stream   */
            shipped = tgt;            /* what shipped: trust the request    */
            (void)shipped;
        }
    }

    /* ---- phase 3: the same presses, with the offset REFINED --------------
     *
     * Same transport, same clock rule (ui_sec follows the stream), the only
     * change being that the offset is corrected until it lands where it was
     * asked. If the landing is right, trusting it costs nothing and the
     * advance column reads +5 every press. */
    hnl();
    hputs("SHIPPED LOGIC: refine + seek intent, step 5s"); hnl();
    hputs("press  tgt  landed  advance  probes  rejected"); hnl();
    {
        uint32_t ui = 0, intent = 0;
        for (int pr = 0; pr < 10; pr++) {
            uint32_t last = (dur > 3u) ? dur - 3u : 0u;
            uint32_t base = ui;
            if (intent > ui && intent - ui < 30u) base = intent;
            uint32_t tgt = base + 5u;
            if (tgt > last) tgt = last;
            intent = tgt;

            uint64_t want = (uint64_t)tgt * (uint64_t)f.rate;
            uint64_t landed = 0;
            int iters = 0, rej = 0;
            seek_locate(&f, want, first, fsize, f.total_samples,
                        &landed, &iters, &rej);
            uint32_t pos = f.rate ? (uint32_t)(landed / f.rate) : 0u;

            hputs("  "); hputu((uint32_t)pr);
            hputs("    "); hputu(tgt);
            hputs("     "); hputu(pos);
            hputs("     +"); hputu(pos > ui ? pos - ui : 0u);
            hputs("     "); hputu((uint32_t)iters);
            hputs("     "); hputu((uint32_t)rej);
            hnl();
            ui = pos;
        }
    }

    hputs("DONE"); hnl();
    return 0;
}
