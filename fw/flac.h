/* Minimal streaming FLAC decoder.
 *
 * Portable C99 with no firmware dependencies, so it can be built and tested on
 * a host against real files before it ever reaches hardware. That matters:
 * every hardware round trip on this project costs a card swap, and a decoder
 * is the kind of code where a host test finds in seconds what hardware would
 * take an evening to localise.
 *
 * WHY NOT libFLAC OR dr_flac: both allocate working buffers per BLOCK --
 * blocksize x channels x 4 bytes, which is 36864 for the 4608-sample blocks
 * every file on the test card uses, against a 26624-byte arena. They do not
 * fit and cannot be made to.
 *
 * This one buffers ONE channel and streams the other, and the streamed one
 * decodes into the SAME array it is consuming -- ch0[i] is read at exactly the
 * instant ch1[i] is produced, and ch1's LPC history is the entries just
 * overwritten. Total working set: blocksize x 4 bytes. Stereo decorrelation
 * needs both channels at the same sample index, and FLAC stores subframes
 * consecutively (all of channel 0, then all of channel 1) -- so channel 0 has
 * to be held, but channel 1 can be reconstructed a sample at a time and
 * emitted immediately. LPC needs only `order` samples of history, 32 at most.
 * That halves the requirement to blocksize x 4.
 */
#ifndef FLAC_H
#define FLAC_H

#include <stdint.h>

#define FLAC_MAX_ORDER   32u
#define FLAC_MAX_CHANNELS 2u

/* Pulls compressed bytes. Returns how many it supplied; 0 means end of
 * stream. Firmware backs this with the ring buffer, the host test with a
 * file. */
typedef int (*flac_read_fn)(void *ctx, uint8_t *dst, int n);

typedef enum {
    FLAC_OK = 0,
    FLAC_END,                 /* clean end of stream                        */
    FLAC_ERR_MAGIC,           /* not a FLAC file                            */
    FLAC_ERR_STREAMINFO,
    FLAC_ERR_UNSUPPORTED,     /* channels, bit depth or blocksize we refuse */
    FLAC_ERR_SYNC,            /* lost the frame sync                        */
    FLAC_ERR_DATA,            /* malformed subframe                         */
    FLAC_ERR_SHORT            /* input ran out mid-frame                    */
} flac_err;

typedef struct {
    /* ---- input ---- */
    flac_read_fn  read;
    void         *ctx;
    uint8_t       buf[512];
    uint32_t      have, pos;      /* bytes in buf, and read cursor          */
    uint64_t      bitacc;         /* bit reservoir, refilled 4 bytes at a time */
    uint32_t      bitcnt;
    int           eof;

    /* ---- tag destinations, set by the caller BEFORE flac_open ----
     * Left null to skip tag parsing entirely. tag_cap sizes title/artist/album;
     * year and track are fixed 8-byte fields, matching the ID3 path. */
    char         *tag_title, *tag_artist, *tag_album, *tag_year, *tag_trk;
    uint32_t      tag_cap;

    /* ---- from STREAMINFO ---- */
    uint32_t      rate;
    uint8_t       channels;
    uint8_t       bps;
    uint32_t      min_blocksize, max_blocksize;
    uint64_t      total_samples;

    /* ---- per frame ---- */
    uint32_t      blocksize;
    uint8_t       ch_mode;        /* 0 independent, 8 L/S, 9 R/S, 10 M/S    */

    /* ---- the one buffered channel ---- */
    int32_t      *ch0;            /* max_blocksize entries, caller-provided */
    uint32_t      ch0_cap;

    /* LPC history for the streaming channel */
    int32_t       hist[FLAC_MAX_ORDER];
} flac_t;

/* Reads the magic and every metadata block, stopping at the first audio
 * frame. Fills the STREAMINFO fields. `ch0` must have room for
 * max_blocksize int32s -- which is only known after this call, so callers
 * that cannot grow a buffer should pass the largest they have and check
 * max_blocksize afterwards. */
flac_err flac_open(flac_t *f, flac_read_fn read, void *ctx,
                   int32_t *ch0, uint32_t ch0_cap);

/* Decodes one frame. Emits interleaved 16-bit samples through `sink`, which
 * is called with however many frames are ready -- possibly several times per
 * FLAC frame, which is the point of streaming. Returns FLAC_END at the end of
 * the stream. */
typedef void (*flac_sink_fn)(void *ctx, const int16_t *pcm, uint32_t frames);
flac_err flac_decode_frame(flac_t *f, flac_sink_fn sink, void *sink_ctx);

/* Profiling, present when flac.c is built with FLAC_PROFILE (the default).
 * Firmware points flac_tick at its cycle counter; leave it null to disable. */
extern uint32_t (*flac_tick)(void);
extern uint32_t flac_res_cyc;    /* channel 0, Rice/bit-reader pass */
extern uint32_t flac_lpc_cyc;    /* channel 0, reconstruction pass  */
extern uint8_t  flac_order, flac_type;   /* type: 1 FIXED, 2 LPC    */

/* Drops buffered input and bit state after the caller has repositioned the
 * stream. Keeps STREAMINFO and the block buffer. */
void flac_flush_input(flac_t *f);


#endif
