/* Proves the metadata skip in fw/flac.c is byte-exact.
 *
 * flac_open() can now ask the caller to move the stream past a block it does
 * not want, instead of reading it a byte at a time. The risk is not that it
 * saves too little -- it is the accounting. At the moment it asks, the reader
 * is holding bytes in `buf` and bits in its reservoir, all of them stream
 * content already fetched, and only what lies beyond that may be skipped
 * remotely. One byte wrong and the stream position is wrong, which does not
 * degrade anything: STREAMINFO or the first frame lands in the wrong place and
 * the file does not play at all.
 *
 * So this opens the SAME file twice through the real decoder -- once with the
 * skip callback and once without -- and compares everything flac_open()
 * produces, plus the first frames decoded afterwards, which is where a lost or
 * duplicated byte would actually show.
 */
#include "hostio.h"
#include "../../fw/flac.h"

static uint32_t cur;
static uint32_t reads, skips, skipped_bytes;

static int rd(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    uint32_t got = hread(cur, dst, (uint32_t)n);
    cur += got;
    reads++;
    return (int)got;
}

/* What the firmware does: move the file cursor, nothing else to keep. */
static int sk(void *ctx, uint32_t n)
{
    (void)ctx;
    cur += n;
    skips++;
    skipped_bytes += n;
    return 1;
}

static int32_t ch0[8192];
static uint32_t sig, pcm_n;

static void sink(void *ctx, const int16_t *pcm, uint32_t frames)
{
    (void)ctx;
    for (uint32_t i = 0; i < frames * 2u; i++) {
        sig ^= (uint32_t)(uint16_t)pcm[i];
        sig *= 16777619u;
        pcm_n++;
    }
}

static char ti[64], ar[64], al[64], yr[8], tk[8];

static void tags_clear(void)
{
    for (int i = 0; i < 64; i++) ti[i] = ar[i] = al[i] = 0;
    for (int i = 0; i < 8; i++) yr[i] = tk[i] = 0;
}

static uint32_t str_sig(const char *p)
{
    uint32_t h = 2166136261u;
    while (*p) { h ^= (uint8_t)*p++; h *= 16777619u; }
    return h;
}

/* Returns 0 on success, filling the out-params. */
static int run(int use_skip, uint32_t frames_to_decode,
               flac_t *out, uint32_t *tagsig)
{
    flac_t f;
    memset(&f, 0, sizeof f);
    tags_clear();
    f.tag_title = ti; f.tag_artist = ar; f.tag_album = al;
    f.tag_year = yr;  f.tag_trk = tk;    f.tag_cap = 64u;
    if (use_skip) f.skip = sk;

    cur = 0; sig = 2166136261u; pcm_n = 0;
    reads = skips = skipped_bytes = 0;

    if (flac_open(&f, rd, 0, ch0, 8192u) != FLAC_OK) return 1;
    for (uint32_t i = 0; i < frames_to_decode; i++)
        if (flac_decode_frame(&f, sink, 0) != FLAC_OK) break;

    *out = f;
    *tagsig = str_sig(ti) ^ str_sig(ar) ^ str_sig(al) ^ str_sig(yr) ^ str_sig(tk);
    return 0;
}

static void show(const char *label, flac_t *f, uint32_t tagsig)
{
    hputs(label);
    hputs(" rate ");   hputu(f->rate);
    hputs(" ch ");     hputu(f->channels);
    hputs(" bps ");    hputu(f->bps);
    hputs(" blk ");    hputu(f->max_blocksize);
    hputs(" total ");  hputu64(f->total_samples);
    hputs(" tags ");   hputu(tagsig);
    hputs(" pcm ");    hputu(pcm_n);
    hputs(" sig ");    hputu(sig);
    hnl();
}

int main(void)
{
    flac_t a, b;
    uint32_t ta, tb, siga, pcma;

    if (run(0, 3u, &a, &ta)) { hputs("PLAIN OPEN FAILED"); hnl(); return 1; }
    siga = sig; pcma = pcm_n;
    show("without skip:", &a, ta);
    hputs("   reads "); hputu(reads); hnl();

    if (run(1, 3u, &b, &tb)) { hputs("SKIP OPEN FAILED"); hnl(); return 1; }
    show("with skip:   ", &b, tb);
    hputs("   reads "); hputu(reads);
    hputs("  skips "); hputu(skips);
    hputs("  bytes skipped "); hputu(skipped_bytes);
    hnl();

    int bad = 0;
    if (a.rate != b.rate || a.channels != b.channels || a.bps != b.bps ||
        a.max_blocksize != b.max_blocksize ||
        a.total_samples != b.total_samples) { hputs("STREAMINFO DIFFERS"); hnl(); bad = 1; }
    if (ta != tb) { hputs("TAGS DIFFER"); hnl(); bad = 1; }
    if (pcma != pcm_n) { hputs("SAMPLE COUNT DIFFERS"); hnl(); bad = 1; }
    if (siga != sig) { hputs("AUDIO DIFFERS"); hnl(); bad = 1; }

    hputs(bad ? "FAILED" : "IDENTICAL: same header, same tags, same audio");
    hnl();
    return bad;
}
