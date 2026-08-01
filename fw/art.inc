/* ======================================================================= ART
 * Embedded cover art: locate the ID3 APIC frame, decode the JPEG inside it,
 * and scale the result into the off-screen SDRAM stash the panel blits from.
 *
 * Runs ONCE per track, before prefill -- so the blocking reads it needs cannot
 * starve the decoder, because playback has not started yet. Decoding later,
 * during playback, would put hundreds of milliseconds of SD reads directly in
 * the path that keeps the PCM FIFO fed.
 *
 * picojpeg is used in REDUCE mode: it decodes one pixel per 8x8 block, skipping
 * AC dequantisation, IDCT and chroma upsampling entirely. A 600x600 cover
 * collapses to ~75x75 -- already about panel size -- for a fraction of the
 * work. Full-resolution decode would be pointless at 96 px and far too slow.
 */
#include "picojpeg.h"

#define ART_MAX_SRC_W 320u              /* covers art up to ~2560 px wide */
#define ART_MAX_BYTES (384u * 1024u)

static uint16_t art_row[2][ART_MAX_SRC_W];
static uint32_t art_jpg_pos, art_jpg_end;   /* file offsets of the image data */
static uint32_t art_win_len, art_win_pos;   /* window currently held in tagbuf */

/* picojpeg asks for at most 255 bytes at a time. Serving each of those with its
 * own target read would be hundreds of SD commands per image, so a 4 KB window
 * is refilled instead and the callback served from that. */
static unsigned char art_need_bytes(unsigned char *pBuf, unsigned char buf_size,
                                    unsigned char *pBytes, void *d)
{
    (void)d;
    uint32_t n = 0;
    while (n < buf_size) {
        if (art_win_pos >= art_win_len) {
            if (art_jpg_pos >= art_jpg_end) break;
            uint32_t len = art_jpg_end - art_jpg_pos;
            if (len > TAG_SIZE) len = TAG_SIZE;
            if (!target_read_slot(MP3_SLOT_ID, art_jpg_pos, TAG_OFF, len)) break;
            art_jpg_pos += len;
            art_win_len = len;
            art_win_pos = 0;
        }
        pBuf[n++] = tagbuf[art_win_pos++];
    }
    *pBytes = (unsigned char)n;
    return n ? 0 : PJPG_STREAM_READ_ERROR;
}

/* Walk the tag's frame list looking for APIC. Reads each 10-byte frame header
 * on its own and skips by the declared size rather than scanning a buffer:
 * cover art is often hundreds of KB, so the frame we want usually sits well
 * past anything worth holding in RAM. */
static int art_find_apic(uint32_t tag_len)
{
    if (tag_len < 20u) return 0;
    if (!target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, 16u)) return 0;
    if (tagbuf[0] != 'I' || tagbuf[1] != 'D' || tagbuf[2] != '3') return 0;
    uint8_t major = tagbuf[3];

    uint32_t p = 10;
    while (p + 10u <= tag_len) {
        if (!target_read_slot(MP3_SLOT_ID, p, TAG_OFF, 16u)) return 0;
        if (tagbuf[0] == 0) return 0;                     /* padding reached */

        uint32_t fsize = (major >= 4)
            ? (((uint32_t)(tagbuf[4] & 0x7Fu) << 21) | ((uint32_t)(tagbuf[5] & 0x7Fu) << 14) |
               ((uint32_t)(tagbuf[6] & 0x7Fu) << 7)  |  (uint32_t)(tagbuf[7] & 0x7Fu))
            : (((uint32_t)tagbuf[4] << 24) | ((uint32_t)tagbuf[5] << 16) |
               ((uint32_t)tagbuf[6] << 8)  |  (uint32_t)tagbuf[7]);
        if (!fsize || p + 10u + fsize > tag_len) return 0;

        if (tagbuf[0] == 'A' && tagbuf[1] == 'P' &&
            tagbuf[2] == 'I' && tagbuf[3] == 'C') {
            if (fsize > ART_MAX_BYTES) return 0;          /* refuse the absurd */

            /* Payload: encoding, MIME (NUL-terminated), picture type,
             * description (NUL-terminated; two NULs when UTF-16), then image. */
            uint32_t body = p + 10u;
            uint32_t win  = fsize < 256u ? fsize : 256u;
            if (!target_read_slot(MP3_SLOT_ID, body, TAG_OFF, win)) return 0;

            uint8_t  enc  = tagbuf[0];
            uint32_t i    = 1;
            uint32_t mime = i;
            while (i < win && tagbuf[i]) i++;

            /* picojpeg is baseline JPEG only, and PNG cover art is common
             * enough that checking the MIME beats feeding a PNG to it. */
            int jpg = 0;
            for (uint32_t k = mime; k + 2u < i; k++) {
                uint8_t a = tagbuf[k] | 0x20u, b2 = tagbuf[k+1] | 0x20u,
                        c = tagbuf[k+2] | 0x20u;
                if (a == 'j' && b2 == 'p' && (c == 'e' || c == 'g')) { jpg = 1; break; }
            }
            if (!jpg) return 0;

            i++;                                     /* past the MIME NUL     */
            i++;                                     /* picture type byte     */
            if (enc == 1u || enc == 2u) {            /* UTF-16 description    */
                while (i + 1u < win && (tagbuf[i] || tagbuf[i+1])) i += 2;
                i += 2;
            } else {
                while (i < win && tagbuf[i]) i++;
                i++;
            }
            if (i >= win) return 0;

            art_jpg_pos = body + i;
            art_jpg_end = p + 10u + fsize;
            art_win_len = 0;
            art_win_pos = 0;
            return 1;
        }
        p += 10u + fsize;
    }
    return 0;
}

/* One destination row, run-length encoded: adjacent pixels of downscaled cover
 * art are very often identical, and a run costs one engine command whatever
 * its length. */
static void art_emit_row(const uint16_t *src, uint32_t dy, const uint16_t *xmap)
{
    uint32_t run_x = 0;
    uint16_t run_c = src[xmap[0]];
    for (uint32_t dx = 1; dx <= ART_IMG; dx++) {
        uint16_t c = (dx < ART_IMG) ? src[xmap[dx]] : (uint16_t)(run_c ^ 1u);
        if (c != run_c) {
            fb_rect(ART_PAD + run_x, ART_STASH_Y + ART_PAD + dy,
                    dx - run_x, 1, run_c);
            run_x = dx;
            run_c = c;
        }
    }
}

static int art_decode(uint32_t tag_len)
{
    if (!tag_len || !art_find_apic(tag_len)) return 0;

    pjpeg_image_info_t info;
    if (pjpeg_decode_init(&info, art_need_bytes, 0, 1 /* reduce */)) return 0;

    uint32_t bxn = (uint32_t)info.m_MCUWidth / 8u;
    uint32_t byn = (uint32_t)info.m_MCUHeight / 8u;
    uint32_t sw  = (uint32_t)info.m_MCUSPerRow * bxn;
    uint32_t sh  = (uint32_t)info.m_MCUSPerCol * byn;
    if (!sw || !sh || sw > ART_MAX_SRC_W) return 0;

    static uint16_t xmap[ART_IMG];
    for (uint32_t dx = 0; dx < ART_IMG; dx++) xmap[dx] = (uint16_t)(dx * sw / ART_IMG);

    for (uint32_t my = 0; my < (uint32_t)info.m_MCUSPerCol; my++) {
        for (uint32_t mx = 0; mx < (uint32_t)info.m_MCUSPerRow; mx++) {
            if (pjpeg_decode_mcu()) return 0;
            for (uint32_t by = 0; by < byn; by++) {
                for (uint32_t bx = 0; bx < bxn; bx++) {
                    uint32_t blk = (by * bxn + bx) * 64u;
                    uint32_t sx  = mx * bxn + bx;
                    if (sx >= ART_MAX_SRC_W) continue;
                    uint8_t r = info.m_pMCUBufR[blk];
                    uint8_t g = (info.m_comps == 1) ? r : info.m_pMCUBufG[blk];
                    uint8_t b = (info.m_comps == 1) ? r : info.m_pMCUBufB[blk];
                    art_row[by][sx] = (uint16_t)(((r & 0xF8u) << 8) |
                                                 ((g & 0xFCu) << 3) | (b >> 3));
                }
            }
        }
        /* Emit the destination rows this MCU row covers. Source arrives in
         * raster order, so this streams -- no full image is ever buffered. */
        for (uint32_t by = 0; by < byn; by++) {
            uint32_t gsy = my * byn + by;
            uint32_t d0  = gsy * ART_IMG / sh;
            uint32_t d1  = (gsy + 1u) * ART_IMG / sh;
            for (uint32_t dy = d0; dy < d1 && dy < ART_IMG; dy++)
                art_emit_row(art_row[by], dy, xmap);
        }
    }

    /* Deliberately does NOT derive an accent from the cover. That was tried:
     * the accent then changed on every track, which read as inconsistency
     * rather than as a feature. One fixed accent everywhere. */
    return 1;
}
