// =============================================================================
// Stage 3 -- real MP3 playback.
//
// Pipeline:
//   SD card --(APF 0180, arbitrary offset)--> MP3 ring buffer in RAM
//           --(Helix fixed-point decode)-----> PCM frame (1152 samples)
//           --(burst push)-------------------> pcm_fifo, drained in HARDWARE
//                                              at the file's true sample rate
//
// The hardware FIFO is what makes this possible at all: a decode call blocks
// for ~1.1M cycles (~22 ms) while the DAC needs a sample every ~21 us, so
// firmware can never feed the DAC directly. It pushes a frame at a time and
// hardware handles timing.
//
// Refill policy: top the MP3 ring up whenever it drops below half, and only
// while the PCM FIFO still has a cushion. Analogue's docs warn the first access
// to a slot walks the FAT cluster chain and that the seek cache is dropped when
// switching slots, so reads can occasionally be slow -- the PCM FIFO is what
// absorbs that jitter.
// =============================================================================

#include <stdint.h>
#include "mp3dec.h"
#include "font_metrics.h"

#define REG(a)      (*(volatile uint32_t *)(uintptr_t)(a))

#define R_AUDIO     0x80000008u
#define R_CYCLES    0x8000000Cu
#define R_STAT0     0x80000010u
#define R_STAT1     0x80000014u
#define R_STAT2     0x80000018u
#define R_STAT3     0x8000001Cu
#define R_TGT_ID    0x80000020u
#define R_TGT_OFF   0x80000024u
#define R_TGT_ADR   0x80000028u
#define R_TGT_LEN   0x8000002Cu
#define R_TGT_GO    0x80000030u
#define R_PCM_ST    0x80000034u
#define R_PCM_RATE  0x80000038u
#define R_INPUT     0x8000003Cu
#define R_VERSION   0x80000040u
#define R_RELOAD    0x80000044u
#define R_FB_ADDR   0x80000048u
#define R_FB_SIZE   0x8000004Cu   /* {h[17:9], w[8:0]} */
#define R_FB_COLOR  0x80000050u   /* {bg[31:16], fg[15:0]} */
#define R_FB_GO     0x80000054u   /* W: {sy[13:12],sx[11:10],glyph[9:3],op[1:0]} */
#define R_FB_STALL  0x80000058u   /* R: clk cycles spent with the draw FIFO full */
#define R_SLOT_SZ   0x8000005Cu   /* R: size of the file APF reported at reload */
#define R_DT_ADDR   0x80000060u   /* W: datatable word address                  */
#define R_DT_DATA   0x80000064u   /* R/W: datatable word at that address        */
#define R_EQ        0x80000068u   /* R/W: EQ preset index, 0 = FLAT (bypass)    */
#define R_SET_IDX   0x8000006Cu   /* W:   persistent settings word index, 0..7  */
#define R_SET_DAT   0x80000070u   /* R: value APF wrote  W: value we publish    */

/* Target command selector, written to R_TGT_GO bits [1:0]. */
#define TGT_READ     0u   /* 0180 */
#define TGT_OPENFILE 1u   /* 0192 */
#define TGT_GETFILE  2u   /* 0190 */
#define TGT_WRITE    3u   /* 0184 */

/* R_RELOAD bits -- see mp3_soc.v */
#define RL_PENDING   1u
#define RL_PL_RELOAD 0x10u   /* the PLAYLIST slot was reloaded (bit 4) */
#define RL_READY     2u   /* allcomplete rose since the reload notification   */
#define RL_AC_NOW    4u   /* allcomplete level right now                      */
#define RL_AC_FELL   8u   /* allcomplete fell since the reload notification   */

#define FB_OP_RUN   0u
#define FB_OP_RECT  1u
#define FB_OP_CHAR  2u
#define FB_OP_COPY  3u

/* Album-art panel. The image is decoded ONCE into an off-screen SDRAM stash
 * (row 400+, past the 360 visible rows) and then blitted into place with a
 * single COPY per animation step -- which is what makes sliding affordable at
 * all. Re-sending ~9000 pixels from the CPU every step would have starved the
 * decoder outright. */
/* The panel is a MOUNT, not a bare image: a rounded grey plate carrying the
 * card's tone, with the cover inset inside it and a hairline between the two.
 * Baked into the stash as one picture, so sliding it is still a single COPY --
 * the frame costs nothing per animation step.
 *
 *   ART_W/ART_H  the whole mount (what gets blitted)
 *   ART_PAD      grey border around the cover
 *   ART_IMG      the cover itself
 */
#define ART_PAD  6u
#define ART_IMG  92u               /* cover; 4px smaller buys the card 4px back */
#define ART_W    (ART_IMG + 2u * ART_PAD)
#define ART_H    (ART_IMG + 2u * ART_PAD)
#define ART_X    276u              /* rests flush with the shared right margin */

/* Vertical relationship to the meter, in ONE place.
 *
 * The two share a baseline by design, but that made them impossible to tune
 * separately -- nudging the meter dragged the art with it. ART_NUDGE breaks
 * that without abandoning the relationship: the art still tracks the meter if
 * the meter moves or changes height, and this is the only number to touch when
 * the art alone needs to shift. Negative is up. */
#define ART_NUDGE  4        /* holds the art while the meter moved up */
/* Bottom-aligned with the waveform (180 + 72 = 252) so the two read as one
 * band on a common baseline rather than two overlapping objects. */
#define ART_Y    (UI_WAVE_Y + UI_WAVE_H - ART_H + ART_NUDGE)
#define ART_STASH_Y 400u           /* off-screen: below the visible frame */

/* R_PCM_ST layout -- MUST match mp3_soc.v:
 *   {13'd0, underrun, full, empty, 4'd0, level[11:0]}
 * i.e. level = bits 11:0, empty = 16, full = 17, underrun = 18.
 * These were originally coded as bits 13/14, which silently read as constant 0:
 * the "wait while full" check never waited (pcm_fifo drops pushes when full, so
 * samples vanished) and underruns were never reported. Keep in sync with RTL.
 * Moved up here (was down near their first original use) because the UI code
 * below now calls pcm_underrun() earlier in the file than that was -- C needs
 * the declaration, and macros need their #define, visible before first use. */
#define PCM_LEVEL(s)  ((s) & 0xFFFu)
#define PCM_EMPTY(s)  (((s) >> 16) & 1u)
#define PCM_FULL(s)   (((s) >> 17) & 1u)
#define PCM_UNDER(s)  (((s) >> 18) & 1u)

/* sysio.c -- heap high-water mark. Displayed so "is it leaking?" is answered
 * by a number instead of an argument: load_track() frees the old Helix
 * instance before allocating the new one, so newlib should hand back the same
 * ~34 KB block every time and this should sit flat no matter how many tracks
 * are loaded. If it climbs per reload, something genuinely is not being freed. */
extern unsigned int heap_used(void);

#define CLK_HZ      60000000u   /* clk_sys; UI timing needs it before playback does */

/* Free-running cycle counter. Up here because the UI uses it for its own
 * timing (marquee, paused-state throttle) well before the playback code does. */
static inline uint32_t cycles(void) { return REG(R_CYCLES); }

static inline uint32_t pcm_level(void)    { return PCM_LEVEL(REG(R_PCM_ST)); }
static inline int      pcm_underrun(void) { return PCM_UNDER(REG(R_PCM_ST)); }

/* Must match CORE_VERSION in mp3_soc.v. The .rom reloads in seconds but the
 * bitstream needs a ~6 min compile, so flashing firmware onto stale RTL is easy
 * and its symptoms (dead peripheral, silent audio, unresponsive buttons) look
 * exactly like logic bugs. Checking here turns that into an obvious signal. */
#define EXPECT_VERSION 0x4D503315u   /* rev 21: 16 setting slots          */

/* Shown on the splash. This is the PRODUCT version, not the RTL/firmware
 * contract above -- they answer different questions and must not be conflated.
 * Keep it in step with the status line in README.md; nothing enforces that. */
#define APP_VER "1.2.0"

/* Developer diagnostics, OFF in a release build. Flip to 1 to bring back
 * Select+A (APF slot table, boot vs live), Select+B (the framework's file
 * descriptor) and Select+Start (load-phase timings in ms).
 *
 * These earned their place -- the slot table is what proved the fix for the
 * bug that was corrupting .mp3 files, and the load timings are how the next
 * question about a slow load gets answered with a measurement instead of a
 * guess. They are compiled out rather than deleted so that turning them back
 * on is a one-line change, not an archaeology exercise. */
#ifndef DEBUG_DIAG
#define DEBUG_DIAG 0
#endif

/* Framebuffer: 400x360 RGB565, one word/pixel, 512-word (page-aligned) stride.
 * See mp3_fb.sv for the full rationale. */
#define FB_W       400u
#define FB_H       360u
#define FB_STRIDE  512u

/* Glyph cell: the engine upscales the 8x8 source font 2x via EPX, so one
 * character occupies 16*scale pixels each way. The source font already leaves
 * its two right-hand columns clear, which becomes the inter-character gap --
 * cells tile edge-to-edge with no extra advance needed. */
/* Type scale. The engine scales fractionally (Bresenham), so there is a real
 * step between 16 and 32 px -- integer-only scaling forced a doubling, which
 * is far too coarse for setting a title against an artist line. */
enum { TS_1X = 0, TS_15X = 1, TS_2X = 2, TS_3X = 3 };
static const unsigned char ts_half[4] = { 2, 3, 4, 6 };   /* size = 16*n/2 */

#define FB_CELL(s)  ((FONT_CELL_H * ts_half[s]) / 2u)   /* 16 / 24 / 32 / 48 */

/* The one place a byte becomes a glyph index. The atlas holds 0x20..0x7E and
 * nothing else, so everything outside that is a space.
 *
 * This exists because the width path and the DRAW path disagreed. fb_adv()
 * substituted a space for an out-of-range byte while fb_char() masked with
 * 0x7F and sent the result to the engine -- so the first UTF-8 byte of an
 * accented or symbol character (0xE2, say) was drawn as 'b' while being
 * measured as a space. Wrong glyphs AND overlapping spacing, from one title
 * containing a character the font cannot show. Both now ask this. */
static uint32_t fb_glyph(char ch)
{
    uint32_t c = (unsigned char)ch;
    return (c < FONT_FIRST || c > FONT_LAST) ? (uint32_t)' ' : c;
}

/* Proportional advance. The engine paints the full 16-px cell, and glyphs are
 * left-aligned within it, so stepping by the ink width overwrites only the
 * previous glyph's blank padding -- proportional spacing without needing a
 * transparent blit. */
static uint32_t fb_adv(char ch, uint32_t sx)
{
    unsigned char c = (unsigned char)ch;
    return ((uint32_t)font_adv[fb_glyph(c) - FONT_FIRST] * ts_half[sx]) / 2u;
}

/* Shadow of the engine's colour register. The parameter registers persist
 * between pushes (each push snapshots them into the FIFO), so a whole string
 * in one colour costs ONE colour write plus two writes per character. */
static uint32_t fb_color_shadow = 0xFFFFFFFFu;

static inline void fb_wait(void) { while (REG(R_FB_GO) & 1u) { } }

static void fb_set_color(uint16_t fg, uint16_t bg)
{
    uint32_t v = ((uint32_t)bg << 16) | (uint32_t)fg;
    if (v != fb_color_shadow) { fb_color_shadow = v; REG(R_FB_COLOR) = v; }
}

/* One command for the whole block -- the engine walks the rows itself. This
 * used to be a firmware loop issuing one command per row, which is what made
 * drawing expensive enough to disturb the decode budget.
 *
 * CLOBBERS the colour registers (it sets fg = bg = its fill). Any text drawn
 * afterwards needs its own fb_set_color() -- and calling fb_set_color() BEFORE
 * a clear rect leaves fg == bg, so the glyphs paint invisibly. */
static void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color)
{
    if (!w || !h) return;
    fb_wait();
    REG(R_FB_ADDR) = y * FB_STRIDE + x;
    REG(R_FB_SIZE) = (h << 9) | w;
    fb_set_color(color, color);
    REG(R_FB_GO)   = FB_OP_RECT;
}

/* Draws the full cell -- foreground AND background -- so text overwrites
 * whatever was there. Nothing needs erasing before a redraw, which removes
 * both the erase-rect and the "did I erase enough" class of bug. Call
 * fb_set_color() once for the run of characters. */
/* SDRAM -> SDRAM block move. Source rides in the colour registers because a
 * copy has no use for them. */
/* The engine stages each row in glyphbuf, which is 128 entries addressed by
 * wsrc_addr[6:0]. A copy wider than that wraps the read address while still
 * writing the full width, so it emits garbage AND runs past where the caller
 * thinks it stopped. A 246 px scroll wrote straight through into the album art,
 * which shares those scanlines. Splitting here rather than at the call sites
 * means nothing else can hit it either.
 *
 * 127, not 128: the engine latches the width as char_w <= q_w[6:0], seven bits,
 * so a span of exactly 128 truncates to ZERO and copies nothing at all. That
 * left the first span dead and only the remainder scrolling -- half the strip
 * moving and half sitting still. */
#define FB_COPY_MAX 127u

static void fb_copy_span(uint32_t sx_, uint32_t sy_, uint32_t dx, uint32_t dy,
                         uint32_t w, uint32_t h)
{
    uint32_t src = sy_ * FB_STRIDE + sx_;
    fb_wait();
    REG(R_FB_ADDR)  = dy * FB_STRIDE + dx;
    REG(R_FB_SIZE)  = (h << 9) | w;
    REG(R_FB_COLOR) = ((src >> 16) & 0x7u) | ((src & 0xFFFFu) << 16);
    fb_color_shadow = 0xFFFFFFFFu;      /* colour regs clobbered -- invalidate */
    REG(R_FB_GO)    = FB_OP_COPY;
}

static void fb_copy(uint32_t sx_, uint32_t sy_, uint32_t dx, uint32_t dy,
                    uint32_t w, uint32_t h)
{
    if (!w || !h) return;

    /* Overlapping copies must run away from the direction of travel, or a
     * later chunk reads source pixels an earlier one already overwrote. */
    if (dx > sx_) {
        uint32_t done = 0;
        while (done < w) {
            uint32_t n = w - done;
            if (n > FB_COPY_MAX) n = FB_COPY_MAX;
            uint32_t off = w - done - n;
            fb_copy_span(sx_ + off, sy_, dx + off, dy, n, h);
            done += n;
        }
    } else {
        uint32_t done = 0;
        while (done < w) {
            uint32_t n = w - done;
            if (n > FB_COPY_MAX) n = FB_COPY_MAX;
            fb_copy_span(sx_ + done, sy_, dx + done, dy, n, h);
            done += n;
        }
    }
}

static void fb_char(uint32_t x, uint32_t y, char ch, uint32_t sx, uint32_t sy)
{
    fb_wait();
    REG(R_FB_ADDR) = y * FB_STRIDE + x;
    REG(R_FB_GO)   = FB_OP_CHAR
                   | ((fb_glyph(ch) & 0x7Fu) << 3)
                   | (sx << 10)
                   | (sy << 12);
}

/* Stage 4b bring-up proof, isolated from playback deliberately: this is the
 * FIRST use of SDRAM, a new PLL clock, and a new scanout engine in this
 * project, all introduced together. Draw something simple and unmistakable
 * BEFORE building real UI art on top, so a problem points at "the new
 * subsystem" rather than being tangled up with the working playback code. */
static void fb_test_pattern(void)
{
    /* Five horizontal bands: primaries + white, easy to eyeball for correct
     * colour channel mapping (a mixed-up RGB565 packing shows up immediately
     * as e.g. "red" rendering blue) and for scanout corruption (tearing,
     * wrong-line artifacts, the "pink edge" class of scaler bug documented
     * in mp3_fb.sv). RGB565: bits [15:11]=R [10:5]=G [4:0]=B. */
    uint16_t bands[5] = {
        0xF800u,  /* red   */
        0x07E0u,  /* green */
        0x001Fu,  /* blue  */
        0xFFFFu,  /* white */
        0x0000u,  /* black */
    };
    uint32_t band_h = FB_H / 5u;
    for (int i = 0; i < 5; i++)
        fb_rect(0, (uint32_t)i * band_h, FB_W, band_h, bands[i]);
}

/* The font itself now lives in the FPGA (src/fpga/core/font_rom.v, generated
 * from the Inter typeface by tools/gen_font_rom.py) -- the CPU sends a
 * character code, not pixels. Firmware keeps only the geometry, since it still
 * has to decide what fits where. */

static uint32_t fb_text_width(const char *s, uint32_t sx)
{
    uint32_t w = 0;
    while (*s) w += fb_adv(*s++, sx);
    return w;
}

/* Clips to whole characters. This MUST be the only way text gets drawn: the
 * engine does not bounds-check, and stride=512 > FB_W=400 means a cell running
 * past column 400 lands in the memory the NEXT scanline reads -- corrupt
 * pixels elsewhere on screen, not a clean cutoff.
 *
 * The screen edge is enforced here rather than trusted to max_w, because
 * max_w is a budget measured from x and every caller would otherwise have to
 * subtract its own x correctly to stay safe. Returns where the text ended. */
static uint32_t fb_text_clipped(uint32_t x, uint32_t y, const char *s,
                                uint32_t sx, uint32_t sy, uint32_t max_w)
{
    /* Two DIFFERENT limits, which is what the old single test got wrong.
     * max_w budgets ADVANCES (how much room the text may occupy), while the
     * screen edge bounds the painted CELL. Testing the cell against max_w
     * meant the final glyph needed a whole cell of budget when it only
     * advances by its ink width -- so the last character of every
     * tightly-measured string was silently dropped. */
    uint32_t cell  = (FONT_CELL_W * ts_half[sx]) / 2u;
    uint32_t limit = x + max_w;
    while (*s) {
        uint32_t a = fb_adv(*s, sx);
        if (x + a > limit) break;          /* out of layout budget */
        if (x + cell > FB_W) break;        /* would paint off-screen */
        fb_char(x, y, *s, sx, sy);
        x += a;
        s++;
    }
    return x;
}

/* Picks the largest scale whose whole string still fits, so a short title gets
 * the big treatment and a long one stays legible instead of being chopped. */
static uint32_t fb_text_fit(const char *s, uint32_t max_w, uint32_t max_scale)
{
    for (uint32_t k = max_scale; k > 0u; k--)
        if (fb_text_width(s, k) <= max_w) return k;
    return TS_1X;
}

/* Declared here (rather than down with the rest of the playback state) so the
 * UI functions just below, which reference them, don't need forward decls --
 * C requires a declaration to be visible before first use. */
static HMP3Decoder dec;
static uint32_t frames, errs, rate_set, min_level;
static uint32_t samprate;         /* set once rate_set; needed for elapsed-time display */
/* Samples per frame, taken from the decoder rather than assumed. MPEG-1 Layer
 * III is 1152; MPEG-2 and 2.5 are 576. Hardcoding 1152 ran the elapsed clock at
 * double speed on any low-sample-rate file -- which is the only thing that ever
 * made this core MPEG-1-only. Helix decodes all three, and everything else here
 * already derives from what it reports. */
static uint32_t samp_per_frame = 1152u;
/* Up here with the other UI-visible state: the progress bar needs both, and C
 * needs them declared before ui_draw_dynamic(). */
static uint32_t audio_start;             /* first byte after any ID3 tag */
static uint32_t bytes_per_sec = 16000u;  /* refined once decoding */
/* Set the moment a decoded frame disagrees with the first frame's bitrate.
 * Until then the file is CBR as far as we have seen, and for CBR the frame
 * bitrate IS the byte rate -- exactly, with nothing to measure. */
static uint8_t  vbr_seen;
static uint32_t track_kbps, track_hz;

/* Playback speed. 1.2x is the whole range: playing at N x needs N x the decode
 * throughput, and against the Stage 0 figures 1.5x already breaks on 320 kbps
 * while 2x cannot work at any bitrate. 6/5 rather than a float -- there is no
 * FPU, and this lands in the same expression as a 64-bit shift.
 *
 * Deliberately NOT persisted, so it costs no settings slot (all eight are used
 * and a ninth would mean an RTL change). Resetting to normal each launch is
 * also the right default for something engaged per-listen. See ROADMAP. */
#define SPEED_NUM 6u
#define SPEED_DEN 5u
static uint8_t speed_fast;               /* 0 = normal, 1 = 1.2x */

/* Drain the PCM FIFO at the file's sample rate, scaled by the current speed;
 * sound_i2s zero-order-holds up to its fixed 48 kHz. Pitch rises with speed --
 * there is no room in the decode budget for time-stretching on top.
 *
 * ONE place computes this. It was duplicated at the two points that learn the
 * sample rate, and a speed toggle has to reproduce it exactly or the pitch goes
 * wrong on the next track only.
 *
 * Nothing else needs adjusting for speed: ui_sec, the duration and the seek
 * distances are all derived from FILE POSITION, not wall clock, so they stay
 * correct by construction. The FIFO drain is the only wall-clock-domain thing
 * here. */
static void pcm_rate_apply(uint32_t hz)
{
    if (!hz) return;
    uint64_t inc = ((uint64_t)hz << 32) / CLK_HZ;
    if (speed_fast) inc = inc * SPEED_NUM / SPEED_DEN;
    REG(R_PCM_RATE) = (uint32_t)inc;
}
static uint32_t track_bytes;      /* audio length the FILE declares (Xing/VBRI) */
static uint8_t  size_suspect;     /* directory disagrees with the file itself   */
static uint8_t  ui_size_warned;
/* Load timing, in milliseconds, for the phases between pcm_flush() and
 * prefill(). Everything in that window runs with the FIFO empty, so whatever
 * dominates it is the hiccup. Measured rather than reasoned about: four
 * attempts at this were aimed by theory and three of them made it worse. */
static uint16_t ld_head, ld_size, ld_art, ld_pre, ld_total;
static uint8_t  ui_ld_shown;
#define LD_MS(c) ((uint16_t)((c) / (CLK_HZ / 1000u)))
/* Total frames declared by a Xing/Info/VBRI header, 0 when the file has none.
 * Duration from size/bitrate is only right for CBR -- on a VBR file the first
 * frame's bitrate is not the file's average, which is why both the total time
 * and the progress bar were off. */
static uint32_t track_frames, track_secs;
/* How far the pending seek moves, in seconds. Was a hardcoded 5 at the point of
 * use, which made both a fine seek and an accelerating scrub impossible to
 * express -- the request said which way but never how far. */
static uint32_t seek_secs = 5u;
/* Average byte rate measured from real playback. Converges on the truth for
 * VBR files that carry no Xing header, which the first-frame bitrate cannot. */
static uint32_t meas_rate;
/* Where the current measurement window started. meas_rate has to be throughput
 * ACTUALLY DECODED, and file_pos alone is not that -- a seek moves it without
 * any time passing. Measuring from a baseline that a seek resets keeps the two
 * quantities describing the same interval. */
static uint32_t meas_pos0, meas_sec0;

/* Ring cursors live up here with the UI state: the measured-rate calculation
 * needs them, and C wants them declared before ui_draw_dynamic(). */
static uint32_t file_pos;       /* next byte offset to fetch from the file   */
static uint32_t ring_fill;      /* valid bytes currently in the ring         */
static uint32_t ring_rd;        /* read cursor within the ring               */ /* stream format, known only once decoding starts */
static uint32_t ui_info_y;            /* where chrome left room for the format line */
static uint32_t ui_last_info, ui_last_prog, ui_pause_next, ui_breath, ui_icon_next;
static uint32_t ui_arr_t, ui_wave_force, ui_accent_changed;

/* ---- preset EQ ----
 * The curve table and the names are GENERATED from the same quantised
 * coefficients the RTL filters with (tools/gen_eq_coeffs.py --curves), so
 * the shape on screen cannot drift from the shape being applied. */
#include "eq_curve.h"
static uint8_t  eq_idx;              /* 0 = FLAT = RTL bypass          */
static uint32_t ui_sec, ui_sec_acc, ui_last_frames, ui_prog_sec;
static int      ui_was_paused;
static uint32_t peak_amp;         /* max |sample| in the most recently decoded frame, 0..32767 */

enum { ID3_OK, ID3_NO_TAG, ID3_NO_FRAME, ID3_UNSUPPORTED_ENCODING };
static int title_status;

static char track_title[48];
static char track_artist[48];
static char track_album[48];
static char track_year[8];
static char track_trk[8];

/* Encoder identification from the LAME/Info tag extension.
 *
 * The tag sits immediately after the Xing header this core already parses, so
 * the bytes are free -- we were reading past them and throwing them away. Its
 * first nine bytes are the encoder string ("LAME3.100", "Lavf58.29"), and the
 * byte after that carries the VBR method in its low nibble.
 *
 * Empty when the file has no LAME extension, which is most non-LAME encoders
 * and every file with a bare Xing header. */
static char    track_encoder[12];
static uint8_t track_vbr_method;      /* 0 = unknown / absent */

/* Transport state. Declared up here with the other UI-visible globals rather
 * than down with poll_input(): ui_draw_dynamic() shows the play/pause state,
 * and C needs the declaration before that use. */
/* Volume as a straight percentage, 0..100, where 100 is unity gain -- the
 * loudest the stream goes without clipping. Applied as a Q8 fixed-point gain
 * recomputed once per change, because a divide per sample would be 2304
 * software divides every frame inside the decode budget. */
#define VOL_MAX   100u
#define VOL_STEP  5u
static uint32_t paused, volume = 65u;    /* overridden by the saved setting     */

/* Fade-in after ANY audio discontinuity, in samples (~46 ms at 44.1 kHz).
 *
 * Paired with pcm_fifo's glide-to-zero on underrun: the FIFO brings the held
 * output down to silence during a gap, and this ramps the new audio up from
 * silence after it. BOTH halves are required -- a fade alone starts at zero
 * while the DAC still holds the old level (the step just moves), and the glide
 * alone ends at zero and then steps up to the first full-scale sample.
 *
 * This includes RESUME from pause. Resume used to be click-free by accident:
 * the held sample was waveform-continuous with the next pushed one. The glide
 * breaks that bargain, so resume must ramp like everything else. */
#define FADE_SAMPLES 2048u
static uint32_t fade_left;
static uint8_t  under_shadow;   /* underrun already faded this flush epoch */
static int32_t  vol_gain = 256;          /* Q8: 256 == unity */

static void vol_apply(void)
{
    vol_gain = (int32_t)(volume * 256u / 100u);
}
/* ------------------------------------------------------------- playlist ----
 * State only. The logic is in playlist.inc, which has to be included further
 * down (it needs the target-command helpers), but the UI drawn above that point
 * reads these -- so they are declared here where both can see them.
 *
 * pl_order[] is the PLAY order, pl_off[] the file order. Shuffle permutes
 * pl_order and leaves pl_off alone, so "track 4 of 12" always means the same
 * track whether shuffled or not, and turning shuffle off resumes the file
 * order without reloading anything. */
#define PL_MAX       128u        /* tracks; the index costs 4 bytes each */
/* The .m3u text. 8 KB made PL_MAX unreachable in practice and therefore a lie:
 * a real playlist here averages 110 bytes a line, so the buffer ran out at ~74
 * tracks while the documentation promised 128. 16 KB covers 128 lines of up to
 * 128 bytes, which is longer than any sane "Artist - Title.mp3". Costs 8 KB of
 * BSS. If this ever needs to grow again, raise PL_MAX with it or the same
 * mismatch comes back the other way round. */
#define PL_TEXT_MAX  16384u

static char     pl_text[PL_TEXT_MAX];
/* Set when the .m3u did not fit -- either the text buffer filled or PL_MAX was
 * reached with lines still to read. Without this a clipped playlist is
 * indistinguishable from a short one: the screen just shows a smaller number. */
static uint8_t  pl_truncated;
static uint16_t pl_off[PL_MAX];          /* byte offset of each name in pl_text */
static uint16_t pl_order[PL_MAX];        /* play order -> file index            */
static uint16_t pl_count;                /* 0 = no playlist loaded              */
static uint16_t pl_pos;                  /* index INTO pl_order                 */

enum { REP_OFF = 0, REP_ALL, REP_ONE };
static uint8_t  rep_mode;                /* cycles off -> all -> one -> off */
static uint8_t  shuffle_on;
static uint32_t pl_rng = 1u;             /* shuffle RNG, seeded from cycles() */

/* Screen blanking. blank_min is minutes with no button press before the screen
 * goes black; 0 disables it. Playback is untouched -- only drawing stops.
 *
 * Be honest about what this buys, because an earlier version of this comment
 * was not. The Pocket is a 1600x1440 LTPS LCD, NOT an OLED, so there is no
 * burn-in to protect against -- an LCD gets temporary image persistence at
 * worst. Nor is it a power saving: a core cannot reach the backlight (the APF
 * interface carries pixel data and sync and nothing else), so the panel stays
 * lit and the draw calls saved are a rounding error next to it.
 *
 * What it actually gives you is a dark screen in a dark room. That is a real
 * want, and it is the whole of the case for this feature.
 *
 * Set by Select+Down, not persisted -- it costs a settings slot that resume
 * needs more. */
/* ---------------------------------------------------------------- RESUME
 * Where playback was when the core was last closed, in ONE 32-bit word,
 * because one settings slot is all there was to spare:
 *
 *   bits  0..6   track index   0-127, matching PL_MAX
 *   bits  7..23  seconds       0-131071, about 36 hours -- audiobook country
 *   bit  24      FROM PLAYLIST -- is the track index above meaningful at all
 *   bits 25..30  reserved, always zero
 *   bit  31      ALWAYS ZERO
 *
 * Bit 31 is reserved unset, and the tag is seven bits rather than eight, for a
 * reason paid for on hardware: APF stores these values as SIGNED 32-bit. The
 * first attempt used all 32 bits and declared the slider max as 4294967295,
 * which APF read as -1 -- so the range became [0, -1], max below min, and
 * every value collapsed to -1. The persist file said `"val": -1` while every
 * other setting stored correctly. Keeping the word inside positive int32
 * territory means no value can ever be mistaken for negative.
 *
 * The tag is the guard. A saved index means nothing if the .m3u has been
 * edited since, so the byte is compared against the file that actually opens
 * and the position is discarded when it disagrees. Eight bits is a 1-in-256
 * chance of agreeing by accident, and the cost of that is resuming at the
 * wrong timestamp in a real track -- recoverable with B, and bounded because
 * the offset is range-checked against the file anyway.
 *
 * Seconds, not bytes: bytes would need 22 bits for a long file and leave no
 * room for the guard, and seconds survive a re-encode of the same track. */
#define RS_TRACK(w)   ((w) & 0x7Fu)
#define RS_SECS(w)    (((w) >> 7) & 0x1FFFFu)
#define RS_PL(w)      (((w) >> 24) & 1u)

/* Set when the playlist started the playing track, cleared when the user
 * picked a file themselves. Without it resume_pump() stamped pl_pos onto every
 * save merely because a playlist EXISTED, so a standalone mp3 was stored as
 * "playlist track 5" and came back as Phish at 128 seconds. */
#define RS_PACK(t, s, p) (((uint32_t)((t) & 0x7Fu)) \
                        | ((uint32_t)((s) & 0x1FFFFu) << 7) \
                        | ((uint32_t)((p) ? 1u : 0u) << 24))

static uint8_t  track_from_pl;    /* playlist started this track, not the user */
static uint32_t resume_word;      /* the packed point, as published to APF   */
static uint8_t  resume_on;        /* Core Settings check; default OFF, opt in */
static uint8_t  resume_armed;     /* a saved point is waiting to be applied  */
static uint32_t resume_at;        /* seconds to seek to once the track opens */
static uint8_t  resume_seek_req;  /* apply resume_at at the next safe point   */
static uint32_t resume_deadline;  /* stop waiting for a rate/size after this   */

/* Which branch the boot-time restore took. Latched, so the answer survives on
 * screen instead of having to be inferred from whether music started in the
 * right place:
 *   0 not reached      1 armed            2 position past a known duration
 *   3 disabled/nothing  4 nothing opened    6 repositioned
 *   7 idle at the seek  8 no byte rate      9 no file size
 *  10 target past the end of the file
 *  11 gave up waiting for a reload/stop to finish */
static uint8_t  resume_dbg;
static uint16_t resume_saves;     /* times resume_pump has published a point  */

static uint32_t blank_min;            /* 0 = never; set by Select+Down        */
static uint32_t blank_sec;            /* whole seconds since the last button   */
static uint32_t blank_tick;           /* cycles() deadline for the next second */
static uint8_t  screen_blank;         /* the screen is currently black         */
static uint32_t ui_mode_dirty = 1u;      /* repaint the mode icons / N-of-M   */
static uint32_t idle;                    /* nothing loaded: waiting on the user */
static uint8_t  stopped;                 /* Start: at 0:00, not merely paused  */
static uint8_t  hold_paused;             /* stay paused across a track change  */
static uint32_t stop_req;

#define PL_HOLD_MS 400u                  /* Left/Right held this long = skip  */

/* Defined in playlist.inc / settings.inc, called from the input handler that
 * sits above both includes. */
static void pl_reorder(void);
static void pl_resync(uint16_t file_idx);
static uint16_t pl_live_count(void);
static uint16_t pl_live_ordinal(uint16_t pos);
/* The loaded playlist's own filename, last component, uppercased. Filled by
 * pl_name_read() in playlist.inc, which is included below -- declared here
 * because the splash summary above it draws the name. Empty when APF will not
 * say what is in the slot. */
static char pl_name[24];
static char pl_name_raw[24];      /* same, in the card's own spelling */
/* The remembered list is reopened at BOOT only. pl_load() also runs whenever
 * the user picks a playlist, and restoring there would override the pick. */
static uint8_t pl_restore_pending = 1u;

/* Gate for a playlist-slot reload, mirroring the one the MP3 slot already has.
 * 008A means the user PICKED a file, not that the slot is readable yet. */
static uint16_t pl_notify_n;      /* playlist notifications seen from the RTL */
static uint16_t pl_load_n;        /* times pl_load() actually ran             */
static uint32_t pl_reload_seen;   /* OR of every R_RELOAD word observed       */
static uint8_t  pl_reload_armed;
static uint32_t pl_reload_at;        /* hard deadline: act regardless */
static uint32_t pl_probe_at;         /* next slot-changed check */
static char     pl_leaving[24];      /* the name we are switching AWAY from */

/* The playlist to reopen at boot, packed four characters per settings word.
 *
 * Storing the NAME is unavoidable. APF cannot enumerate a directory, so 0192
 * can only open something we already hold, and the slot itself remembers
 * nothing: with a filename declared in data.json APF resets it to that file at
 * every core load, and without one the slot comes up empty and prompts. Both
 * were measured. There is no arrangement of data.json that remembers a pick.
 *
 * Twelve characters of STEM, with ".m3u" implied rather than stored -- that
 * covers "Shenanigans", "audiobook" and most real names for three words
 * instead of four. Truncation is possible and harmless: a name that does not
 * round-trip simply fails to reopen and the default loads. */
#define PL_STEM_MAX 12u
static char pl_saved_stem[PL_STEM_MAX + 1u];

static void settings_mark_dirty(void);
static uint8_t set_flush_now;
/* Set by settings_load() once APF's stored values have been taken. Nothing may
 * publish before that: settings_store() writes all eight words, so an earlier
 * write would push firmware defaults over what the user had saved. Declared
 * here rather than in settings.inc, which is included further down. */
static uint8_t settings_adopted;
static uint8_t settings_load_ok;   /* what the BOOT settings_load() returned */

/* Defined with the rest of the blanking code, below the error paths that have
 * to wake the screen before drawing to it directly. */
static void ui_blank_wake(void);

static uint8_t eq_apply;             /* preset changed: tell the RTL */

static uint32_t seek_req;                /* +1 forward, -1 back (as unsigned) */
static uint32_t soft_restart_req;  /* probe: reposition only, keeps the known-good tag */
static uint32_t reload_pending;
static int      reload_armed;      /* reload seen, waiting out the settle delay */
static uint32_t reload_at;         /* hard deadline: act even if never confirmed */
static uint32_t reload_probe_at;   /* next slot-changed check                    */
static uint32_t reload_settle;     /* blind wait when identity is unavailable     */

/* First four bytes of the file as they actually landed in RAM, captured the
 * instant the head read returns. Surfaced on screen when no tag is found,
 * because "(No ID3 Tag)" alone cannot distinguish the three real possibilities:
 *   49 44 33 ..  tag IS there  -> the parser is at fault
 *   FF Fx ..     bare MP3 sync -> file genuinely has no ID3v2 tag (not a bug)
 *   00 00 00 00  nothing       -> the read didn't land; an APF/DMA problem
 * Guessing between those is what burned two rounds on the reload bug earlier;
 * four bytes on screen settles it in one hardware test. */
static uint8_t head_bytes[4];

/* R_RELOAD snapshot taken at the moment the reload was acted on, and the file
 * size APF reported with the 008A notification. Both shown in the diagnostic
 * line so a failing reload reports its own circumstances. */
static uint32_t reload_status;
static uint32_t slot_size;

/* Previous track's head bytes and APF-reported size, for the stale-slot check
 * in read_track_head(). reload_retries counts how often that check actually
 * fired -- shown on screen, so the mitigation is observable instead of taken
 * on faith. */
/* Identity of the track we are LEAVING, captured at the moment a reload is
 * noticed and then held for the whole probe window. This is what makes
 * staleness detectable: `prev_head`/`prev_slot_size` used to be overwritten by
 * the (possibly stale) load itself, which destroyed the very reference the
 * later probes needed to compare against. */
static char     stale_ref_title[48];   /* title of the track being left */
static uint32_t stale_ref_size;        /* and the size APF reported for it */
static char     last_title[48];        /* title the current load settled on */
static char     track_file[64];        /* filename APF reports for the slot  */

/* A file identity that SURVIVES A POWER CYCLE, which cur_file_id does not.
 *
 * cur_file_id hashes the whole 0190 response struct -- 64 words -- and was
 * built to answer "has the slot changed yet?" by comparing two hashes taken
 * minutes apart in the same session. Nothing ever established it is stable
 * across a relaunch, and it is not: resume was rejecting every saved point
 * with a tag mismatch because the same file hashed differently on the next
 * boot. Anything in that 256-byte window that is a handle, padding, or
 * residue from an earlier command moves, and the hash moves with it.
 *
 * The filename is a property of the FILE, so it is the same on every boot.
 * slot_filename() extracts it as the longest printable-ASCII run rather than
 * trusting an offset, which is also why it is the sturdier thing to hash. */
/* (track_name_id() lived here: a filename hash tried as a cross-boot file
 * identity, measured unstable, and its gate removed. Deleted, not left to
 * rot.) */
static uint32_t cur_file_id;           /* 0190 identity of the loaded file   */
static uint32_t force_size_probe;      /* R_SLOT_SZ is stale: measure instead */
static uint32_t stale_ref_file_id;     /* ...and of the one being left       */
static uint32_t reload_retries;
static uint32_t tag_corrections;   /* periodic probe found a wrong tag */

/* ------------------------------------------------------------ UI layout ---
 * No album art (would need a JPEG decoder for ID3 APIC frames -- out of
 * scope), so this is a plain dark-mode layout: title/artist, a real
 * amplitude-driven level meter, elapsed time. Colours are RGB565.
 */
/* Vertical gradient endpoints. Drawn as horizontal bands rather than a true
 * per-pixel ramp: a rect is ONE engine command, so 40 bands cost 40 commands
 * where a per-row ramp would cost 360 -- and in RGB565 a 40-step ramp across
 * this small a colour range is visually smooth. No RTL change needed. */
#define UI_GRAD_TOP 0x2124u   /* neutral graphite -- the blue cast read as murky
                               * behind the card, and a grey ramp lets the teal
                               * accent be the only colour on screen */
#define UI_GRAD_BOT 0x0000u   /* black          */
#define UI_BANDS    40u

#define UI_PANEL   0x2945u   /* card: neutral, one step above the ramp */
#define UI_BG      0x0862u   /* near-black navy */
#define UI_WHITE   0xFFFFu
#define UI_DIM     0x94B2u   /* mid-gray, for the artist line */
/* Accent palette, cycled with the L/R shoulder triggers. Deriving the accent
 * from the cover art was tried and dropped -- it changed on every track and
 * read as inconsistency. A colour the USER picks is stable, which is the
 * difference. Chosen to sit well on the graphite background: nothing so dark
 * it disappears, nothing so pale it competes with the white type. RGB565. */
static const uint16_t ui_palette[] = {
    0xFC65u,   /* amber   */
    0xBF08u,   /* lime    */
    0xAF13u,   /* mint    */
    0x7E55u,   /* seafoam */
    0x5D5Cu,   /* sky     */
    0x7C5Au,   /* indigo  */
    0xAC5Eu,   /* lilac   */
    0xF534u,   /* blush   */
    0xF32Du,   /* coral   */
    0xB9E9u,   /* crimson */
    0xEE07u,   /* gold    */
    0xEF1Au,   /* cream   */
};
#define UI_PALETTE_N (sizeof(ui_palette) / sizeof(ui_palette[0]))

/* Shown in the toast when L/R changes the accent -- the colour was the only
 * control that changed something without saying what it changed to.
 *
 * A parallel array is a correspondence a later edit can break in silence: add a
 * colour, forget the name, and the label reads off the end. The assert makes that
 * a build error instead of a bug someone finds on hardware. Keep the order. */
static const char *const ui_palette_name[] = {
    "AMBER", "LIME", "MINT", "SEAFOAM", "SKY", "INDIGO",
    "LILAC", "BLUSH", "CORAL", "CRIMSON", "GOLD", "CREAM",
};
_Static_assert(sizeof(ui_palette_name) / sizeof(ui_palette_name[0]) == UI_PALETTE_N,
               "ui_palette_name[] must name every color in ui_palette[]");
#define UI_ACCENT  0xFC65u
static uint16_t ui_accent = UI_ACCENT;
static uint32_t ui_pal_idx;

/* Transient status line. Volume and seek had no on-screen confirmation at all
 * -- the only functional control feedback missing. Shown briefly, then wiped
 * back to the gradient. */
#define UI_TOAST_Y  (UI_PROG_Y - 20u)
#define UI_TOAST_H  16u
#define UI_TOAST_HOLD  (CLK_HZ)            /* full brightness ~1 s   */
#define UI_TOAST_FADE  (CLK_HZ * 3u / 4u)  /* then dissolve over ~.75 s */
#define UI_TOAST_STEPS 10u
static char     ui_toast[24];
static uint32_t ui_toast_t0;               /* 0 = inactive */
static uint32_t ui_toast_step;             /* 0 = solid, UI_TOAST_STEPS = gone */
static uint32_t ui_toast_end;              /* x the last toast draw reached    */
#define UI_TRACK   0x18E3u   /* unfilled part of the meter -- a visible track
                              * rather than bare background, so the meter reads
                              * as one object at any level */
#define UI_RED     0xF800u

#define UI_FAINT   0x6B4Du   /* filename line -- present but recessive */
#define UI_CARD_H  120u
#define UI_SHOW_DIAG 0        /* 1 = show A/S/T/F reload diagnostics */

/* Speed-branch instrumentation. ON by default here and NOT behind a button
 * combo, deliberately: the last diagnostic on this project never appeared
 * because its trigger was written down wrong (Select+L, when the code wanted
 * Select and L held plus Start), and the hardware round trip was wasted. A
 * readout with no way to fail to summon it cannot repeat that.
 *
 * OFF now that resume works. The rows and the resume_dbg latching stay in the
 * source: flipping this to 1 brings back the speed row and the resume row,
 * which between them found four separate faults here, and the 1.2x seek defect
 * is still open. Cheaper to keep than to rewrite. */
#define UI_SHOW_SPEED_DIAG 0

#define UI_MARGIN   20u
#define UI_TITLE_Y  30u
/* Scrolling amplitude history, drawn as bars -- the "waveform" element from
 * the reference art. Bars are cheap (one rect each) now that the engine owns
 * the row loop, and a rolling history reads as motion in a way a single
 * left-to-right level bar never does. */
/* ---- vertical layout knobs -------------------------------------------------
 * Every band's position lives here. ART_Y is derived from the meter (see
 * ART_NUDGE) so the two keep their shared baseline; everything else is
 * independent, so moving one band cannot silently drag another.
 *
 *   card      16 .. 16+UI_CARD_H
 *   art       tracks the meter, offset by ART_NUDGE
 *   meter     UI_WAVE_Y .. +UI_WAVE_H
 *   transport UI_TRANSPORT_Y
 *   clock     UI_TIME_Y
 *   toast     UI_TOAST_Y
 *   progress  UI_PROG_Y
 */
#define UI_WAVE_N   36u
#define UI_WAVE_Y   173u
#define UI_WAVE_H   72u
#define UI_WAVE_GAP 2u
#define UI_TRANSPORT_Y 262u
#define UI_TIME_Y   288u
#define UI_PROG_Y   334u
#define UI_PROG_H   5u
#define UI_INNER_W  (FB_W - 2u * UI_MARGIN)

#define UI_SHOW_UNDERRUN 0   /* red square, top-right: audio FIFO ran dry */
#define UI_UNDERRUN_SZ 10u
#define UI_UNDERRUN_X  (FB_W - UI_MARGIN - UI_UNDERRUN_SZ)
#define UI_UNDERRUN_Y  UI_MARGIN

static uint32_t ui_last_sec   = 0xFFFFFFFFu;
static uint32_t ui_last_vu    = 0xFFFFFFFFu;
static uint32_t ui_last_pause = 0xFFFFFFFFu;
static uint32_t ui_last_stall;
static uint32_t ui_last_spd = 0xFFFFFFFFu;   /* speed-branch diag row */
/* One marquee per scrollable line. Title and artist can both overflow, and
 * they scroll independently -- a shared position would drag the shorter one
 * around for no reason. */
typedef struct {
    char     text[64];
    uint32_t y, scale, on, pos, next;
} ui_marquee_t;
static ui_marquee_t ui_mq_title, ui_mq_artist;
/* Visualisations, cycled with X. The choice persists via interact.json.
 *
 * All three run off what the decoder already produces -- there are no frequency
 * bins here, so none of these is a spectrum: BARS and WATER show loudness over
 * TIME, LEVELS shows the two channels right now. */
enum { VIZ_BARS = 0, VIZ_WATER, VIZ_LEVELS, VIZ_SCOPE, VIZ_WAVE, VIZ_VU,
       VIZ_SCROLL, VIZ_MIRROR, VIZ_DOTS, VIZ_COUNT };

/* Stereo phase scope. Left against right, rotated 45 degrees so mono lands on
 * the vertical -- the standard goniometer orientation, and the reason it reads
 * at a glance: a vertical line is mono, a wide cloud is a wide mix, a
 * horizontal spread is out of phase.
 *
 * Points are captured during decode rather than read from pcm[] at draw time:
 * the buffer is refilled every frame and the UI runs on its own schedule, so
 * drawing from it directly would sample whatever happened to be there. */
#define SCOPE_N 48u
/* Frames of persistence. A vectorscope drawn as isolated dots, cleared every
 * pass, never builds into a shape -- what makes a real one readable is the
 * phosphor holding the trace while it moves. Keeping a few frames and drawing
 * the older ones dimmer costs a little more per pass but multiplies what is on
 * screen: 48 points x 4 frames reads as a continuous figure where 96 fresh dots
 * read as static. */
#define SCOPE_HIST 4u

/* Oscilloscope: a short TRIGGERED window, not the whole frame's envelope.
 *
 * Min/max across each column's slice filled almost the whole box, because a
 * column covering ~18 samples spans several cycles of anything above a few
 * hundred Hz -- the envelope of a frame is nearly always full scale. Showing a
 * brief window instead means the columns follow the wave itself, so the trace
 * is thin and you can see the shape moving.
 *
 * The window starts at a rising zero crossing so successive frames line up
 * instead of sliding; without that the trace skates sideways and reads as
 * noise. */
/* VU: two analogue meters, left and right.
 *
 * Ballistics matter more than the drawing. A real VU integrates over ~300 ms;
 * a needle tracking instantaneous peaks twitches and reads as an artefact
 * rather than a meter. Fast attack, slow decay, in a Q8 accumulator so the
 * movement is smooth at any frame rate.
 *
 * Deflection is by SIN TABLE rather than a divide per pixel: the needle is
 * drawn as a run of short segments along the angle, and 16 entries at Q12 is
 * both cheaper and smaller than the trigonometry. */
#define VU_ATT   180u             /* Q8 rise per frame toward the target */
#define VU_DEC    40u             /* ~370 ms full-scale fall: VU ballistics */
#define VU_STEPS  28u             /* segments: 1.9 px apart, so the needle
                                   * is solid. At 10 they sat 5.4 px apart and
                                   * the needle read as a dotted line. */
static uint32_t vu_l, vu_r;       /* Q8 deflection, 0..255               */
static uint8_t  vu_face;          /* the static face is on screen        */
static uint16_t vu_face_w;        /* ...and the width it was drawn for   */
static uint8_t  vu_shown_l, vu_shown_r;   /* deflection currently drawn   */


/* Needle angle, -50 to +50 degrees from vertical in 16 steps: a 100 degree
 * sweep, which is what a real VU movement travels. The first attempt built one
 * table and tried to derive both components from it by index arithmetic; the
 * values ran past Q12 unity, so the sweep came out narrow and lopsided -- the
 * 45-degree stub. Two honest tables, generated rather than hand-written. */
static const int16_t vu_sn[17] = {
    -3138, -2832, -2493, -2125, -1731, -1317,  -887,  -446,     0,
      446,   887,  1317,  1731,  2125,  2493,  2832,  3138
};
static const int16_t vu_cs[17] = {
     2633,  2959,  3250,  3502,  3712,  3879,  3999,  4072,  4096,
     4072,  3999,  3879,  3712,  3502,  3250,  2959,  2633
};

/* Interpolate between table entries. Indexing the table directly gave the
 * needle 17 positions across the sweep -- the tip jumping ~5 px at a time,
 * which reads as stepping rather than sweeping. Interpolating gives it 256, so
 * the movement is as smooth as the ballistics driving it. */
static void vu_angle(uint32_t v255, int32_t *sn, int32_t *cs)
{
    uint32_t pos = v255 * 16u;                 /* 0..4080 */
    uint32_t q   = pos / 255u;                 /* 0..16   */
    uint32_t f   = pos % 255u;
    uint32_t q1  = (q < 16u) ? q + 1u : 16u;
    *sn = vu_sn[q] + (int32_t)((vu_sn[q1] - vu_sn[q]) * (int32_t)f) / 255;
    *cs = vu_cs[q] + (int32_t)((vu_cs[q1] - vu_cs[q]) * (int32_t)f) / 255;
}


#define WAVE_COLS 64u
#define WAVE_SPAN 4u              /* samples per column -- 256 sample window */
static signed char wav_v[WAVE_COLS];
/* Capture is normalised to a fixed +-SCOPE_UNIT; the DRAW scales that onto the
 * box. Splitting it that way is what lets x and y have different extents: the
 * meter area is 246x72, so an isotropic trace can only ever be 72 px across and
 * sits as a small blob in a wide empty rectangle. Stretching x fills the space
 * and exaggerates stereo width, while mono still collapses to x = 0 and
 * out-of-phase still lies flat -- the readings that matter are preserved. */
#define SCOPE_UNIT 100
static signed char scope_x[SCOPE_HIST][SCOPE_N], scope_y[SCOPE_HIST][SCOPE_N];
static uint8_t     scope_head;   /* newest frame */
static uint8_t  viz_mode;
static uint32_t peak_l, peak_r;          /* per-channel, for LEVELS */
static unsigned char lvl_l, lvl_r, lvl_pl, lvl_pr;

static unsigned char wave[UI_WAVE_N], wave_drawn[UI_WAVE_N];
static unsigned char wave_pk[UI_WAVE_N], wave_pk_drawn[UI_WAVE_N];

/* Art panel: art_x is where it currently sits, animated toward its target.
 * FB_W means fully off the right edge. */
/* Shown by default; SELECT hides it. art_x is where the panel currently sits,
 * and FB_W means fully off the right edge. */
static uint32_t art_x = ART_X, art_shown = 1, art_ready, ui_text_w;
/* art_shown is what is on screen; art_pref is what the USER asked for.
 * Keeping them apart is what lets the choice survive a track change: a
 * track with no artwork hides the panel without forgetting that the panel
 * is wanted, so the next track that has some brings it back. */
static uint8_t  art_pref = 1, art_have;
static uint32_t art_file_id;      /* 0190 identity the stash was decoded for */
static uint32_t art_toggle, art_next;   /* rolling amplitude history, 0..UI_WAVE_H */
static int      ui_underrun_shown;

/* Linear blend of two RGB565s, t in 0..UI_BANDS. Per channel so the ramp keeps
 * its hue instead of sliding through grey. */
static uint16_t ui_mix(uint16_t a, uint16_t b, uint32_t t, uint32_t n)
{
    uint32_t r = (((a >> 11) & 0x1Fu) * (n - t) + ((b >> 11) & 0x1Fu) * t) / n;
    uint32_t g = (((a >> 5)  & 0x3Fu) * (n - t) + ((b >> 5)  & 0x3Fu) * t) / n;
    uint32_t bl = ((a & 0x1Fu) * (n - t) + (b & 0x1Fu) * t) / n;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Top of the background ramp, tinted toward the user's accent. Recomputed
 * whenever the accent changes; the bottom stays black. */
static uint16_t ui_grad_top_c = UI_GRAD_TOP;

/* Luma the tinted top is normalised to. The ramp used to be a fixed neutral
 * graphite at luma 35, and at that brightness RGB565 has almost no room for
 * hue: measured over the twelve accents, CREAM quantises to exactly the old
 * neutral and several others land within one level of it, so a tint would have
 * been invisible on half the palette. 45 buys enough levels to tell them apart
 * while keeping the background far below the white type it sits under. */
#define UI_GRAD_LUMA 45u

/* Accent -> a dark tinted ramp top. Normalising to a FIXED luma rather than
 * scaling the accent directly is what keeps this safe: every colour lands at
 * the same brightness, so the contrast the text was tuned against does not move
 * when the user cycles the palette, and no accent can produce a background that
 * competes with the type. Then pulled halfway to neutral, because a full
 * saturation cast is what made an earlier coloured ramp read as murky behind
 * the card -- this should say "tinted", not "coloured". */
static void ui_grad_set(uint16_t accent)
{
    uint32_t r = ((accent >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g = ((accent >> 5)  & 0x3Fu) * 255u / 63u;
    uint32_t b = (accent & 0x1Fu) * 255u / 31u;

    uint32_t l = (2126u * r + 7152u * g + 722u * b) / 10000u;
    if (!l) l = 1u;
    /* Rounded, not truncated. Flooring here cost up to a level of green on half
     * the palette, which at this brightness is a visible fraction of the whole
     * tint -- and made the built colours differ from the ones that were
     * reviewed in tools/gradient_preview.py. */
    r = (r * UI_GRAD_LUMA + l / 2u) / l;
    g = (g * UI_GRAD_LUMA + l / 2u) / l;
    b = (b * UI_GRAD_LUMA + l / 2u) / l;
    r = (r + UI_GRAD_LUMA + 1u) / 2u;     /* half-saturated */
    g = (g + UI_GRAD_LUMA + 1u) / 2u;
    b = (b + UI_GRAD_LUMA + 1u) / 2u;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;

    ui_grad_top_c = (uint16_t)(((r * 31u + 127u) / 255u) << 11 |
                               ((g * 63u + 127u) / 255u) << 5  |
                               ((b * 31u + 127u) / 255u));
}

/* THE colour of the background at row y. Every consumer must come through here
 * -- the ramp is dithered, so anything that reconstructs a background colour
 * by its own arithmetic will disagree with what was actually drawn and leave a
 * patch. There were nine such call sites before this existed.
 *
 * Why dither at all: 40 bands between the old top and black collapse to
 * THIRTEEN distinct RGB565 colours, each holding for ~28 rows -- ~111 panel
 * rows once the Pocket's 4x integer scale is applied, which is the visible
 * banding. Drawing more bands changes nothing, because the colour space has no
 * values in between. Alternating adjacent levels row to row lands the eye on
 * intermediate colours RGB565 cannot name. The cost is fine horizontal texture
 * at one framebuffer row -- 4 panel rows, ~2 arcminutes at arm's length against
 * a band's ~110 -- so it should fuse where a band edge cannot.
 *
 * Text is the one place this is imperfect: CHAR paints its own background in a
 * single colour, so a glyph cell gets one row's colour for all 16 of its rows.
 * The mismatch is at most one level, which is the same error the old band
 * boundaries already made when a glyph straddled one. */
static uint16_t ui_grad_at(uint32_t y)
{
    static const uint8_t thr[4] = { 1u, 5u, 3u, 7u };   /* eighths, ordered */
    if (y >= FB_H) y = FB_H - 1u;
    uint32_t den = FB_H - 1u;
    uint32_t rem = den - y;                 /* bottom is black: ideal = top*rem/den */
    uint32_t t   = thr[y & 3u];
    uint32_t lv[3] = { (uint32_t)((ui_grad_top_c >> 11) & 0x1Fu),
                       (uint32_t)((ui_grad_top_c >> 5)  & 0x3Fu),
                       (uint32_t)(ui_grad_top_c & 0x1Fu) };
    for (uint32_t k = 0; k < 3u; k++) {
        uint32_t num  = lv[k] * rem;
        uint32_t base = num / den;
        if ((num - base * den) * 8u > t * den) base++;
        lv[k] = base;
    }
    return (uint16_t)((lv[0] << 11) | (lv[1] << 5) | lv[2]);
}

/* Rounded rect. The engine has no corner primitive, so the corners are cut
 * back out afterwards with the GRADIENT's local colour at each row -- a flat
 * background colour would leave four visible notches against the ramp. r rows
 * of 2 small rects each; trivial next to a full-screen fill. */
static void fb_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t r, uint16_t color)
{
    fb_rect(x, y, w, h, color);
    for (uint32_t i = 0; i < r; i++) {
        /* Quarter-circle by integer search: for this row's distance from the
         * corner centre, find the largest x still inside radius r. */
        uint32_t dy = r - i;
        uint32_t inner = 0;
        while ((inner + 1u) * (inner + 1u) + dy * dy <= r * r) inner++;
        uint32_t cut = r - inner;
        if (!cut) continue;
        uint16_t top = ui_grad_at(y + i);
        uint16_t bot = ui_grad_at(y + h - 1u - i);
        fb_rect(x, y + i, cut, 1, top);
        fb_rect(x + w - cut, y + i, cut, 1, top);
        fb_rect(x, y + h - 1u - i, cut, 1, bot);
        fb_rect(x + w - cut, y + h - 1u - i, cut, 1, bot);
    }
}

/* The waveform gives up its right-hand end to the art panel, which occupies
 * the same rows. */
static uint32_t ui_wave_w(void)
{
    return art_shown ? (ART_X - UI_MARGIN - 4u) : UI_INNER_W;
}

/* Repaint the strip the art travels through, so a slide leaves the background
 * behind it rather than a smear. Only the bands crossing the panel's rows. */
static void ui_art_bg_range(uint32_t x, uint32_t w)
{
    if (!w || x >= FB_W) return;
    if (x + w > FB_W) w = FB_W - x;
    for (uint32_t y = ART_Y; y < ART_Y + ART_H && y < FB_H; y++)
        fb_rect(x, y, w, 1, ui_grad_at(y));
}

/* Blit the stash to the current position, clipped at the right edge. The panel
 * enters from the right, so only its leftmost columns are on screen at first --
 * and an unclipped copy would run past column 400 into the memory the NEXT
 * scanline displays, i.e. corruption elsewhere rather than a clean cut. */
static void ui_art_draw(void)
{
    /* The panel overlaps the meter area while it slides, so it can paint
     * over a cached VU face -- which is how hiding the art left the right
     * meter's arc erased for good: the width changed once, the face was
     * redrawn once, and then the slide wiped it again. */
    vu_face = 0;

    if (!art_ready || art_x >= FB_W) return;
    uint32_t w = FB_W - art_x;
    if (w > ART_W) w = ART_W;
    fb_copy(0, ART_STASH_Y, art_x, ART_Y, w, ART_H);
}

/* One rect per ROW, not per band. 360 commands against 40 -- affordable
 * because this is a repaint, never a per-frame path, and the SDRAM work is
 * identical either way since the pixel count does not change. Per-row is what
 * lets ui_grad_at() dither at all. */
static void ui_gradient(void)
{
    for (uint32_t y = 0; y < FB_H; y++)
        fb_rect(0, y, FB_W, 1, ui_grad_at(y));
}

static void ui_toast_set(const char *msg, uint32_t n, const char *suffix)
{
    uint32_t i = 0;
    while (msg[i] && i < sizeof(ui_toast) - 1u) { ui_toast[i] = msg[i]; i++; }
    if (n != 0xFFFFFFFFu) {
        ui_toast[i++] = ' ';
        char t[8]; int k = 0;
        do { t[k++] = (char)('0' + n % 10u); n /= 10u; } while (n && k < 7);
        while (k && i < sizeof(ui_toast) - 1u) ui_toast[i++] = t[--k];
    }
    if (suffix) while (*suffix && i < sizeof(ui_toast) - 1u) ui_toast[i++] = *suffix++;
    ui_toast[i] = 0;
    ui_toast_t0   = cycles() | 1u;         /* never 0 -- that means inactive */
    ui_toast_step = 0xFFFFFFFFu;           /* force the first draw */
}

/* Plain text, no trailing number. */
static void ui_toast_msg(const char *msg) { ui_toast_set(msg, 0xFFFFFFFFu, 0); }


/* Bytes per second of AUDIO, which is what both the duration and the seek
 * distance depend on.
 *
 * `bytes_per_sec` is the first decoded frame's bitrate. On a VBR file that is
 * not the file's average, so it made the total length wrong AND made a "5
 * second" seek move by some other amount -- the same error, surfacing twice.
 *
 * Preference order: the rate implied by a declared frame count (exact), then
 * the rate actually measured during playback (self-correcting, needs a few
 * seconds), then the first frame. */
static uint32_t ui_byte_rate(void)
{
    if (track_secs && slot_size > audio_start)
        return (slot_size - audio_start) / track_secs;

    /* CBR with no Xing/Info header: the frame bitrate IS the byte rate, and it
     * is exact. Measuring throughput instead was the whole defect.
     *
     * Three files on the test card are exactly this shape -- no frame count,
     * constant bitrate -- and they were the ONLY three where seeking went
     * wrong, at 1.2x and latently at 1.0x. meas_rate is a self-correcting
     * estimate; preferring it over a number that cannot be wrong let the seek
     * distance drift for no reason at all.
     *
     * meas_rate now serves only what it was ever for: a headerless VBR file,
     * where no constant exists to read. */
    if (!vbr_seen && bytes_per_sec) return bytes_per_sec;
    if (meas_rate) return meas_rate;
    return bytes_per_sec;
}

/* Byte rate for the SEEK LIMIT specifically. Deliberately not ui_byte_rate():
 * that falls back to meas_rate, which converges all through playback, so a limit
 * derived from it drifts a little on every repeat. The parked position then
 * never equals the newly computed one, the movement guard never fires, and the
 * flush-and-reprefill loop comes straight back -- but only on files with no
 * Xing header, since those are the ones that reach the meas_rate branch. That
 * is exactly "works on some songs and not others".
 *
 * Both inputs here are fixed at load, so this figure never moves. Same reasoning
 * as ui_total_secs() below, which was corrected for the same fault earlier. */
static uint32_t ui_seek_rate(void)
{
    if (track_secs && slot_size > audio_start)
        return (slot_size - audio_start) / track_secs;
    return bytes_per_sec;
}

static uint32_t ui_total_secs(void)
{
    if (track_secs) return track_secs;          /* Xing/VBRI: exact */
    /* Headerless: size / first-frame bitrate. BOTH inputs are fixed at load,
     * so the figure appears with the first painted clock and never moves --
     * exact for CBR, approximate for the rare headerless VBR. The earlier
     * version derived this from meas_rate, which updates all through playback;
     * that was the wandering total. */
    if (slot_size > audio_start && bytes_per_sec)
        return (slot_size - audio_start) / bytes_per_sec;
    return 0;
}

static char *ui_mmss(char *p, uint32_t sec)
{
    uint32_t m = sec / 60u, s = sec % 60u;
    if (m > 99u) m = 99u;
    *p++ = (char)('0' + (m / 10u) % 10u);
    *p++ = (char)('0' + m % 10u);
    *p++ = ':';
    *p++ = (char)('0' + s / 10u);
    *p++ = (char)('0' + s % 10u);
    return p;
}

static char *ui_dec(char *p, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static char *ui_hex2(char *p, uint8_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    *p++ = hx[v >> 4]; *p++ = hx[v & 15u];
    return p;
}

/* Drawn ONCE per track load (from load_track(), after title/artist are
 * parsed) -- static chrome, never touched again until the next track. */
/* Fill the stash with a placeholder until real cover art is decoded into it.
 * Deliberately drawn the same way real art will be delivered -- into the same
 * off-screen rows -- so the panel, the slide and the clipping are all exercised
 * now and the decoder simply replaces the contents later. */
static int  art_decode(uint32_t tag_len);

/* Round the art's corners by masking the STASH rather than the blit: baked in
 * once, so every slide step then draws an already-rounded image for free. The
 * corner colour is the card grey, which ties the panel to the text block --
 * and because the gradient runs vertically, a fixed colour stays correct at
 * every x the panel slides through. */
static void ui_art_round(void)
{
    /* Corners are cut with the colour of the GRADIENT at the row the mount
     * will occupy, not a flat colour -- otherwise the rounding shows up as four
     * pale wedges instead of disappearing. The gradient runs vertically, so a
     * per-row colour stays correct at every x the panel slides through. */
    const uint32_t r = 8u;
    for (uint32_t i = 0; i < r; i++) {
        uint32_t dy = r - i, inner = 0;
        while ((inner + 1u) * (inner + 1u) + dy * dy <= r * r) inner++;
        uint32_t cut = r - inner;
        if (!cut) continue;
        uint16_t top = ui_grad_at(ART_Y + i);
        uint16_t bot = ui_grad_at(ART_Y + ART_H - 1u - i);
        fb_rect(0,           ART_STASH_Y + i,              cut, 1, top);
        fb_rect(ART_W - cut, ART_STASH_Y + i,              cut, 1, top);
        fb_rect(0,           ART_STASH_Y + ART_H - 1u - i, cut, 1, bot);
        fb_rect(ART_W - cut, ART_STASH_Y + ART_H - 1u - i, cut, 1, bot);
    }
}

/* The grey plate the cover sits on, plus a hairline just inside the padding so
 * the artwork reads as mounted rather than as a hole cut in the panel. Drawn
 * BEFORE the cover, which then lands inside it. */
static void ui_art_mount(void)
{
    fb_rect(0, ART_STASH_Y, ART_W, ART_H, UI_PANEL);
}

static void ui_art_placeholder(void)
{
    /* Only the cover area -- the mount around it is already drawn. A slightly
     * darker fill than the plate so an artless track reads as an empty frame
     * rather than a solid grey slab. */
    fb_rect(ART_PAD, ART_STASH_Y + ART_PAD, ART_IMG, ART_IMG, UI_TRACK);
    art_ready = 1;
}

/* Clear the waveform band across the FULL inner width and force every bar to
 * redraw. Used on a panel toggle: the bars change width, so leftovers from the
 * previous width would otherwise stay on screen. */
static void ui_wave_clear(void)
{
    /* The VU face is cached on screen rather than redrawn each pass, so
     * whatever erases this region has to say so -- otherwise the arc,
     * ticks and labels are wiped and never come back, while the needle
     * carries on repainting itself. Hiding the album art did exactly
     * that. */
    vu_face = 0;

    for (uint32_t y = UI_WAVE_Y; y < UI_WAVE_Y + UI_WAVE_H && y < FB_H; y++)
        fb_rect(UI_MARGIN, y, UI_INNER_W, 1, ui_grad_at(y));
    for (uint32_t i = 0; i < UI_WAVE_N; i++) { wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu; }
}

/* Transport glyphs drawn as shapes, not characters: the font atlas is ASCII
 * 0x20-0x7E, so there is no play or pause symbol in it, and adding two glyphs
 * to a 97%-full BRAM for this would be a poor trade. Both are a handful of
 * rects, which the engine draws in one command each. */
#define UI_ICON_H 12u
#define UI_ICON_W 11u

/* Three chasing arrows rather than one pulsing arrow: the lit one advances and
 * the one behind it fades out across the step, so the motion reads as
 * direction of travel instead of just "something is on". */
#define UI_ARR_W     9u
#define UI_ARR_H     14u      /* 13 visible rows: a triangle tapers, so it has
                               * to run slightly taller than the 12px pause bars
                               * to carry the same visual weight */
#define UI_ARR_GAP   3u
#define UI_ARR_N     3u
#define UI_ARR_STEPS 10u       /* refreshes per arrow; 30 Hz -> ~330 ms each */
#define UI_ARR_TICKS (UI_ARR_N * UI_ARR_STEPS)
#define UI_ARR_TAIL  (UI_ARR_STEPS * 2u)   /* how far the glow trails behind */
#define UI_ARR_SPAN  (UI_ARR_N * (UI_ARR_W + UI_ARR_GAP))

/* One clear box big enough for EITHER symbol. The pause branch used to clear
 * only its own 12 rows while the arrows are 14, so switching to paused left
 * the bottom two rows of the arrows on screen. Sizing the wipe to the larger
 * of the two removes the whole class of leftover. */
#define UI_ICONBOX_H ((UI_ARR_H > UI_ICON_H) ? UI_ARR_H : UI_ICON_H)

static void ui_icon_arrow(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint16_t c)
{
    /* Right-pointing triangle: vertical edge on the left, apex centred on the
     * right -- the standard media-player form. */
    uint32_t half = h / 2u;
    for (uint32_t i = 0; i < h; i++) {
        uint32_t d  = (i < half) ? (half - i) : (i - half);
        uint32_t ww = ((half - d) * w) / half;
        if (ww) fb_rect(x, y + i, ww, 1, c);
    }
}

/* ---- mode indicators -------------------------------------------------------
 * Drawn from rects rather than added to the font atlas: BRAM is at 97% and a
 * glyph costs a whole cell, while these are four rects each. They also need to
 * dim to "mode off" rather than disappear -- an icon that vanishes gives no
 * hint the mode exists, which is the usual complaint about hidden controls. */
#define UI_MODE_W  11u
#define UI_MODE_H  9u

/* Repeat: a rounded rectangle loop with an arrowhead on the top-right. With
 * `one` set, the middle is notched to read as "just this track". */
static void ui_icon_repeat(uint32_t x, uint32_t y, uint16_t c, int one)
{
    fb_rect(x,               y,               UI_MODE_W,     1u,        c);
    fb_rect(x,               y + UI_MODE_H-1, UI_MODE_W,     1u,        c);
    fb_rect(x,               y + 1u,          1u,            UI_MODE_H-2, c);
    fb_rect(x + UI_MODE_W-1, y + 1u,          1u,            UI_MODE_H-2, c);
    /* Arrowhead, top-right, pointing along the loop. */
    fb_rect(x + UI_MODE_W-4, y - 1u,          1u,            3u,        c);
    fb_rect(x + UI_MODE_W-3, y - 2u,          1u,            5u,        c);
    if (one) {
        /* Break the bottom edge and drop a tick in the gap: unmistakably a
         * different state at 1x, without needing a digit glyph. */
        fb_rect(x + UI_MODE_W/2u - 1u, y + UI_MODE_H-1, 3u, 1u, UI_PANEL);
        fb_rect(x + UI_MODE_W/2u,      y + UI_MODE_H-3, 1u, 3u, c);
    }
}

/* Shuffle: two crossing paths with arrowheads, the conventional form. */
static void ui_icon_shuffle(uint32_t x, uint32_t y, uint16_t c)
{
    for (uint32_t i = 0; i < UI_MODE_H; i++) {
        uint32_t t = (i * (UI_MODE_W - 3u)) / (UI_MODE_H - 1u);
        fb_rect(x + t,                    y + i,               1u, 1u, c);
        fb_rect(x + (UI_MODE_W - 3u) - t, y + i,               1u, 1u, c);
    }
    fb_rect(x + UI_MODE_W - 3u, y,               3u, 1u, c);
    fb_rect(x + UI_MODE_W - 3u, y + UI_MODE_H-1, 3u, 1u, c);
    fb_rect(x + UI_MODE_W - 1u, y,               1u, 3u, c);
    fb_rect(x + UI_MODE_W - 1u, y + UI_MODE_H-3, 1u, 3u, c);
}

/* Stop: a filled square, the universal counterpart to the pause bars. */
static void ui_icon_stop(uint32_t x, uint32_t y, uint16_t c)
{
    fb_rect(x, y, UI_ICON_W, UI_ICON_H, c);
}

static void ui_icon_pause(uint32_t x, uint32_t y, uint16_t c)
{
    uint32_t bar = UI_ICON_W / 3u;
    fb_rect(x, y, bar, UI_ICON_H, c);
    fb_rect(x + UI_ICON_W - bar, y, bar, UI_ICON_H, c);
}

static void ui_marq_init(ui_marquee_t *m, const char *text,
                         uint32_t y, uint32_t scale)
{
    uint32_t i = 0;
    while (text[i] && i < sizeof(m->text) - 1u) { m->text[i] = text[i]; i++; }
    m->text[i] = 0;
    m->y     = y;
    m->scale = scale;
    m->pos   = 0;
    m->next  = cycles() + CLK_HZ;          /* hold at the start first */
    m->on    = (fb_text_width(m->text, scale) > ui_text_w);
}

/* One step. Repaints the whole row first, because the window that follows may
 * be shorter than what was there. */
static void ui_marq_step(ui_marquee_t *m, uint16_t fg)
{
    if (!m->on || (int32_t)(cycles() - m->next) < 0) return;
    m->next = cycles() + CLK_HZ / 3u;

    uint32_t len = 0;
    while (m->text[len]) len++;
    if (++m->pos > len) m->pos = 0;
    if (m->pos == 0) m->next = cycles() + CLK_HZ;   /* pause at the start */

    fb_rect(UI_MARGIN, m->y, ui_text_w, FB_CELL(m->scale), UI_PANEL);
    fb_set_color(fg, UI_PANEL);
    fb_text_clipped(UI_MARGIN, m->y, m->text + m->pos,
                    m->scale, m->scale, ui_text_w);
}

static void ui_draw_chrome(void)
{
    /* Draw nothing at all while blanked -- a track change must not light the
     * screen back up. ui_blank_wake() calls this again on the way out, so the
     * skipped work is simply deferred rather than lost. */
    if (screen_blank) return;
    ui_gradient();

    /* When there's no usable title, say WHICH failure it was and show the
     * first bytes of the file -- see head_bytes' comment. */
    char fallback[24];
    const char *title = track_title;
    if (!track_title[0]) {
        if (title_status == ID3_UNSUPPORTED_ENCODING) {
            title = "UNICODE TAG";
        } else {
            char *p = fallback;
            /* NORD == the sentinel survived, i.e. the read delivered nothing;
             * NOTAG/NOTIT == real bytes arrived but weren't a tag. */
            int nothing_landed = (head_bytes[0] == 0xA5u && head_bytes[1] == 0xA5u &&
                                  head_bytes[2] == 0xA5u && head_bytes[3] == 0xA5u);
            const char *tag = nothing_landed ? "NORD "
                            : (title_status == ID3_NO_TAG) ? "NOTAG " : "NOTIT ";
            while (*tag) *p++ = *tag++;
            for (int i = 0; i < 4; i++) p = ui_hex2(p, head_bytes[i]);
            *p++ = ' '; *p++ = 'R';
            p = ui_hex2(p, (uint8_t)reload_status);
            *p = 0;
            title = fallback;
        }
    }

    /* Fixed, deliberately. Reflowing the text when the panel slides made the
     * whole layout jump on a button press; the waveform yields the space
     * instead, since it shares the panel's rows.
     *
     * MUST be set before the card below, which is sized from it. It used to be
     * assigned afterwards, so on the FIRST call it was still 0 and the card was
     * drawn 16 px wide -- invisible. Every later call inherited the previous
     * call's value and looked correct, which is why it only ever went wrong at
     * boot and looked fine after a reload. */
    /* One right edge for everything. The card used to overhang it by 8 px,
     * so the card, art panel and waveform all ended at different x -- the
     * single biggest reason the layout read as unaligned. Text bounds + 8 px
     * padding, with that padding landing exactly on the shared margin. */
    ui_text_w = UI_INNER_W - 8u;

    /* Card behind the type. Text paints its own background, so the panel
     * colour has to be what the glyphs blend against or every character sits
     * in a little rectangle of the wrong shade. */
    fb_round_rect(UI_MARGIN - 8u, UI_TITLE_Y - 14u,
                  ui_text_w + 16u, UI_CARD_H, 8u, UI_PANEL);

    /* Keep each line's text + scale so its marquee can repaint that row. */
    /* Capped at 2x rather than 3x: with album and format lines below it, a 48px
     * title cannot fit inside the card without colliding with the art row. A
     * predictable layout is worth more than the largest possible type. */
    /* Never below 1.5x. Auto-fit alone could drop a long title to 1x, which is
     * a two-step fall while every other track sits at 1.5x or 2x -- measured on
     * a real library, only the two longest titles ever got there, so they read
     * as a glitch rather than as a layout rule. The marquee already exists for
     * text that will not fit; shrinking was being tried first and never leaving
     * it anything to do. One step of size variation, then it scrolls. */
    /* Fixed at 2x. Auto-fitting meant the title changed size with its LENGTH:
     * measured across a real library, nine of eleven fitted at 2x and the two
     * longest dropped to 1.5x, so those two looked wrong rather than looking
     * fitted -- and one of them missed by eight pixels. The text column is a
     * constant 352 px (it deliberately does not narrow when the art panel
     * slides in), so which side of the line a title falls on is pure chance.
     *
     * One size for every track, and ui_marq_init below turns the scroll on for
     * anything that overflows -- which is what the marquee was already for, and
     * why it almost never ran. */
    uint32_t ts = TS_2X;
    ui_marq_init(&ui_mq_title, title, UI_TITLE_Y, ts);
    fb_set_color(UI_WHITE, UI_PANEL);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y, ui_mq_title.text, ts, ts, ui_text_w);

    /* Artist one step down from the title, never below 1.5x -- that step only
     * exists because the engine can scale fractionally now. */
    uint32_t as = (ts > TS_15X) ? (ts - 1u) : TS_15X;
    ui_mq_artist.on = 0;      /* no artist -> no leftover scroll from the last track */
    uint32_t y = UI_TITLE_Y + FB_CELL(ts) + 6u;
    if (track_artist[0]) {
        fb_set_color(UI_DIM, UI_PANEL);
        ui_marq_init(&ui_mq_artist, track_artist, y, as);
        fb_text_clipped(UI_MARGIN, y, ui_mq_artist.text, as, as, ui_text_w);
        y += FB_CELL(as) + 3u;
    }

    /* Format line (bitrate / sample rate). Nothing to draw yet -- neither is
     * known until the first frame decodes -- so chrome only reserves the row
     * and ui_draw_dynamic() fills it in once. It replaced the slot filename,
     * which existed to prove 0190 getfile returns real data. It does, so that
     * reconnaissance is finished; the capability it demonstrated is what
     * in-core track selection will be built on. */
    if (track_album[0] || track_year[0] || track_trk[0]) {
        char b[64], *q = b;
        if (track_trk[0]) {
            const char *t = track_trk;
            while (*t) *q++ = *t++;
            *q++ = ' '; *q++ = '-'; *q++ = ' ';
        }
        const char *a = track_album;
        while (*a && q < b + sizeof(b) - 10) *q++ = *a++;
        if (track_album[0] && track_year[0]) { *q++ = ' '; *q++ = '-'; *q++ = ' '; }
        const char *yr = track_year;
        while (*yr && q < b + sizeof(b) - 1) *q++ = *yr++;
        *q = 0;
        fb_set_color(UI_DIM, UI_PANEL);
        fb_text_clipped(UI_MARGIN, y, b, TS_1X, TS_1X, ui_text_w);
        y += FB_CELL(TS_1X) + 2u;
    }

    ui_info_y    = y;
    ui_last_info = 0xFFFFFFFFu;

    /* Wave bed. Bars grow upward from the baseline, so clear the whole band
     * once here and let ui_draw_dynamic() repaint only the bars. */
    ui_wave_clear();

    /* Every one of these must be invalidated: this function repaints the whole
     * screen, so anything ui_draw_dynamic() only redraws "when it changes" has
     * just been erased and has to be considered absent. Forgetting
     * ui_last_stall is why the diagnostic line vanished after the first track
     * load and never came back. */
    /* Just re-blit whatever the stash holds. Decoding happens in load_track,
     * NOT here: ui_draw_chrome() is also called by the tag probe during
     * playback, and a decode there would fire hundreds of blocking SD reads
     * straight into the budget that keeps the PCM FIFO fed. */
    ui_art_draw();

    ui_last_sec   = 0xFFFFFFFFu;   /* force the first time-display redraw */
    ui_last_vu    = 0xFFFFFFFFu;
    ui_last_pause = 0xFFFFFFFFu;
    ui_last_stall = 0xFFFFFFFFu;
    ui_last_spd   = 0xFFFFFFFFu;   /* or the diag row dies on the first reload */
    /* THIRD entry to be forgotten from this list, after ui_last_stall and the
     * mode row. A toast is drawn only when its fade step CHANGES, so once
     * chrome has painted over one, ui_toast_step still says "already drawn"
     * and it never comes back.
     *
     * Every toast set around a track change was being erased: the LOADING
     * PLAYLIST and LOADING TRACK indicators, and pl_report()'s "N TRACKS"
     * summary too -- which is why picking from the menu showed nothing at all
     * while the same toasts work fine from a button press. */
    ui_toast_step = 0xFFFFFFFFu;
    ui_last_prog  = 0xFFFFFFFFu;
    /* The mode row -- repeat, shuffle, the EQ name, N-of-M. Missing from this
     * list until now, and the second entry to be forgotten from it after
     * ui_last_stall. It survived because the flag is statically initialised to
     * 1, so the FIRST track drew fine and every reload after that came up
     * blank: the row had been erased and nothing said so. That is why the
     * indicators only appeared once one of them was pressed.
     *
     * The accent-change path knew to set this and ui_draw_chrome did not, which
     * is the real fault -- two lists of the same thing, and only one of them
     * correct. The duplicates there are gone now; this is the one place that
     * knows what a full repaint destroys. */
    ui_mode_dirty = 1u;
    ui_icon_next  = cycles();      /* transport arrows, on the next tick   */
    ui_sec = 0; ui_sec_acc = 0; ui_last_frames = 0xFFFFFFFFu;
    ui_prog_sec   = 0xFFFFFFFFu;
    /* track_kbps is NOT cleared here. It describes the STREAM, not the screen,
     * and ui_draw_chrome() is also called mid-track by the tag probe. Clearing
     * it there blanked the format line permanently: rate_set is latched after
     * the first decoded frame, so nothing ever recomputed the value, and the
     * line's draw is gated on track_kbps being non-zero. Whoever resets
     * rate_set clears it instead. */
    ui_underrun_shown = 0;
    ui_size_warned    = 0;
    ui_ld_shown       = 0;
}

/* A load failure used to spin in `for(;;){}`, which is the worst possible
 * outcome: an I/O problem became a frozen screen with no information, and the
 * user could not even pick another file. Say what happened, keep the status
 * bytes on screen, and stay responsive so the Core menu can load a new track. */
static void poll_input(void);

/* Nothing loaded. The core no longer forces a file to be chosen before it
 * starts, so this is the first thing seen on a card with no playlist -- it has
 * to say what to do rather than look like a failure. */
/* Gradient and title only. Shown while the first track loads, so the wait is a
 * deliberate-looking screen rather than a flash of instructions that is then
 * replaced -- which read as a glitch. */
/* Splash composition: the PLAYER, with nothing loaded into it.
 *
 * A centred title on an empty field was tried and rejected, and the reason is
 * worth keeping: the player screen is a left-aligned card with a meter and a
 * transport row, so a centred splash is a second visual language for the same
 * product. Reusing the player's own skeleton -- same card geometry at the same
 * UI_TITLE_Y, the meter where the meter always is, an info row where the
 * transport sits, the progress bar on its own line -- means the boot screen and
 * the player are one design rather than two that happen to ship together.
 *
 * It also fills the frame honestly. The old splash left 209 of 360 rows
 * carrying nothing; here every band of the screen has the same job it has
 * during playback. */
/* Anchored to the BOTTOM of the card, not tucked under the title. The card is
 * UI_CARD_H to match the player's, which carries four lines; the splash has
 * two, so placing the version directly under the title leaves the lower half
 * visibly empty and the card looks unfinished. Top and bottom anchored, the
 * same space reads as deliberate. 14px inset mirrors the card's top inset. */
#define UI_SPL_VER_Y    (UI_TITLE_Y - 14u + UI_CARD_H - 14u - 16u)
#define UI_SPL_INFO_Y  262u    /* the transport row's line */

/* Card, title and version. Shared by the static splash and the animated one so
 * they cannot drift -- they are the same screen, and previously each drew the
 * title itself. `f/den` is the title's fade position; the card and version do
 * not fade, because animating the frame draws the eye to the furniture. */
static void ui_splash_card(uint32_t f, uint32_t den)
{
    /* Identical geometry to ui_draw_chrome()'s card, at full width since the
     * splash has no art panel to make room for. */
    fb_round_rect(UI_MARGIN - 8u, UI_TITLE_Y - 14u,
                  UI_INNER_W + 16u, UI_CARD_H, 8u, UI_PANEL);

    uint32_t sc = fb_text_fit("MP3 PLAYER", UI_INNER_W, TS_2X);
    fb_set_color(ui_mix(UI_PANEL, ui_accent, f, den), UI_PANEL);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y, "MP3 PLAYER", sc, sc, UI_INNER_W);

    fb_set_color(UI_DIM, UI_PANEL);
    fb_text_clipped(UI_MARGIN, UI_SPL_VER_Y, "v" APP_VER, TS_1X, TS_1X,
                    UI_INNER_W);
}

static void ui_splash(void)
{
    ui_gradient();
    ui_splash_card(1u, 1u);
}

/* Boot animation: a pulse sweeps the meter while the title fades up.
 *
 * Runs BEFORE the track is opened, so it has the whole CPU -- nothing is
 * decoding yet and there is no audio to protect. That is the one moment in this
 * core where drawing cost genuinely does not matter, which is why the meter can
 * be redrawn in full every frame here and nowhere else.
 *
 * Fixed length rather than "until the track loads": tying it to load time means
 * it is a different animation on every card, and a stutter if the load is
 * quick. ~0.8 s, then the splash stays put until playback is ready. */
/* Boot meter: the playback bar meter, running on nothing.
 *
 * Two earlier attempts missed for opposite reasons. The first decayed to
 * silence in 0.83 s, finishing before the playlist had loaded. The second was a
 * travelling sine -- smooth, but too regular to read as a meter.
 *
 * This one is the SAME MOTION as VIZ_BARS during playback: the history scrolls
 * one bar left per frame and a new sample enters at the right, so the boot
 * screen moves the way the player does. Mirroring it was the point; a boot
 * animation that moves differently from the thing it boots into is a second
 * visual language again.
 *
 * The incoming level is a random WALK rather than a fresh random number. White
 * noise scrolling sideways looks like static; a walk wanders, holds a level for
 * a while and then moves off it, which is what a loudness history actually
 * looks like.
 *
 * The walk is MEAN-REVERTING. A plain one clamped at both ends drifts into a
 * corner and sits there -- simulated, it spent most of its time near the floor
 * and the meter read flat and low. Biasing it upward instead just pinned it to
 * the ceiling. Pulling it gently back toward a centre gives a mean around half
 * height with excursions either way, which held across several seeds. */
#define WV_FPS    18u    /* 36 bars at 18 fps: ~2.0 s for one to cross */

/* The level is a slow BODY plus a decaying TRANSIENT, not one walk between a
 * floor and a ceiling. A single clamped walk gave a mean of 73% but a standard
 * deviation of only 5.7 px -- everything sat in a narrow band and the contour
 * read as texture rather than as music. Music is a sustained level with hits
 * punching above it and dropping back, so that is what this generates: the body
 * wanders gently, and every seventh frame or so a transient is struck somewhere
 * in the headroom left above it and then decays away.
 *
 * Simulated over 900 frames: mean 69% of height, sd 10.4, range 18..72, and the
 * ceiling is touched under 1% of the time so peaks land rather than flatten. */
#define WV_BODY   53u    /* percent of full height the body settles around */
#define WV_PULL   18u    /* body is pulled back toward it by /this per frame */
#define WV_DRIFT   5u    /* most the body may wander between samples */
#define WV_HIT     7u    /* a transient is struck about 1 frame in this many */
#define WV_DECAY   2u    /* transient keeps DECAY/4 of itself each frame */
#define WV_FLOOR   2u    /* a bar at zero reads as broken rather than as quiet */

static uint8_t  wv_on, wv_level, wv_tr;
static uint32_t wv_next, wv_rng;
static unsigned char wv_h[UI_WAVE_N];

/* One frame at env/100 of full height. The same code draws the run and the
 * settle: winding env down pulls the whole meter with it. */
static void ui_wave_frame(void)
{
    uint16_t bed = ui_grad_at(UI_WAVE_Y);
    /* Full width, NOT ui_wave_w(). That reports the narrow meter whenever
     * art_shown is set, and art_shown is initialised to 1 -- so the boot meter
     * was leaving a gap for an album-art panel that does not exist yet and
     * cannot, since no track has been opened. The panel appears when the first
     * track turns out to have artwork, and the player narrows the meter then. */
    uint32_t ww  = UI_INNER_W;

    /* Scroll left and insert at the right -- the playback meter's own shift. */
    for (uint32_t i = 0; i < UI_WAVE_N - 1u; i++) wv_h[i] = wv_h[i + 1u];

    /* Body. One RNG draw, mean-reverting toward WV_BODY. */
    wv_rng ^= wv_rng << 13; wv_rng ^= wv_rng >> 17; wv_rng ^= wv_rng << 5;
    int32_t body = (int32_t)((UI_WAVE_H * WV_BODY) / 100u);
    int32_t lv   = (int32_t)wv_level
                 + (int32_t)(wv_rng % (2u * WV_DRIFT + 1u)) - (int32_t)WV_DRIFT
                 - ((int32_t)wv_level - body) / (int32_t)WV_PULL;
    if (lv < (int32_t)WV_FLOOR)   lv = (int32_t)WV_FLOOR;
    if (lv > (int32_t)UI_WAVE_H)  lv = (int32_t)UI_WAVE_H;
    wv_level = (uint8_t)lv;

    /* Transient. Decays first, then may be re-struck anywhere in the headroom
     * ABOVE the body -- which is what keeps a hit from simply saturating when
     * the body is already high. */
    wv_tr = (uint8_t)(((uint32_t)wv_tr * WV_DECAY) / 4u);
    wv_rng ^= wv_rng << 13; wv_rng ^= wv_rng >> 17; wv_rng ^= wv_rng << 5;
    if (wv_rng % WV_HIT == 0u) {
        uint32_t head = (uint32_t)(UI_WAVE_H - wv_level);
        if (head) {
            wv_rng ^= wv_rng << 13; wv_rng ^= wv_rng >> 17; wv_rng ^= wv_rng << 5;
            uint32_t hit = wv_rng % (head + 1u);
            if (hit > wv_tr) wv_tr = (uint8_t)hit;
        }
    }

    uint32_t smp = (uint32_t)wv_level + wv_tr;
    if (smp > UI_WAVE_H) smp = UI_WAVE_H;
    wv_h[UI_WAVE_N - 1u] = (unsigned char)smp;

    for (uint32_t i = 0; i < UI_WAVE_N; i++) {
        uint32_t h   = wv_h[i];
        uint32_t x   = UI_MARGIN + (i * ww) / UI_WAVE_N;
        uint32_t xn  = UI_MARGIN + ((i + 1u) * ww) / UI_WAVE_N;
        uint32_t lit = (xn - x > UI_WAVE_GAP) ? (xn - x - UI_WAVE_GAP) : 1u;

        fb_rect(x, UI_WAVE_Y + UI_WAVE_H - h, lit, h,
                ui_mix(UI_TRACK, ui_accent, i + 1u, UI_WAVE_N));
        if (UI_WAVE_H > h) fb_rect(x, UI_WAVE_Y, lit, UI_WAVE_H - h, bed);
    }
}

static void ui_wave_anim_start(void)
{
    wv_on = 1u; wv_next = 0; wv_rng = cycles() | 1u;
    wv_level = (uint8_t)((UI_WAVE_H * WV_BODY) / 100u);
    wv_tr    = 0;
    for (uint32_t i = 0; i < UI_WAVE_N; i++) wv_h[i] = 0;
}

/* Called from the read spin. A single compare and return unless armed, so
 * every other caller of target_read_slot() is unaffected. */
static void ui_wave_anim_tick(void)
{
    if (!wv_on) return;
    if ((int32_t)(cycles() - wv_next) < 0) return;
    wv_next = cycles() + CLK_HZ / WV_FPS;
    ui_wave_frame();
}

/* Just stop. A ~0.5 s drain was tried and removed: it is half a second added to
 * every launch to watch bars fall, and the player repaints the whole screen a
 * moment later anyway. Stopping dead costs nothing and nobody sees the frozen
 * frame for long. */
static void ui_wave_anim_stop(void)
{
    wv_on = 0;
}

static void ui_splash_anim(void)
{
    ui_gradient();

    const uint32_t STEP_DEN = 32u;   /* title fade resolution */

    /* Minimum time on screen, then the wave carries on from the read spin for
     * as long as loading takes. Fixed length here rather than "until loaded"
     * so a fast card still gets a boot animation instead of a flicker. */
    ui_wave_anim_start();
    const uint32_t INTRO_MS = 1600u;
    uint32_t t0 = cycles(), fade_end = CLK_HZ / 1000u * (INTRO_MS / 2u);
    /* Redraw the card only when the fade STEP changes -- 33 times, not once per
     * spin of an unpaced loop. Repainting the title thousands of times a second
     * is what made it flash: each repaint erases its own cell before writing
     * the glyph, and scanout catches the gap. */
    uint32_t last_f = 0xFFFFFFFFu;
    for (;;) {
        uint32_t el = cycles() - t0;
        if (el >= CLK_HZ / 1000u * INTRO_MS) break;
        uint32_t f = (el < fade_end) ? (el * STEP_DEN / fade_end) : STEP_DEN;
        if (f != last_f) { last_f = f; ui_splash_card(f, STEP_DEN); }
        ui_wave_anim_tick();
    }

    /* Nothing flattens the meter here any more. That belonged to the original
     * one-shot animation, which had finished by this point; now the meter keeps
     * running from the read spin until loading is done, and flattening it would
     * blank one frame and then be immediately overdrawn. */
}

/* `reason` explains why there is nothing playing, or is NULL when the answer is
 * simply "you have not picked anything yet".
 *
 * It has to be painted HERE rather than raised as a toast. This screen is
 * ui_splash() plus three lines of instructions -- the same gradient and the same
 * title as the boot screen -- so a user whose playlist failed sees a screen they
 * cannot distinguish from the one that was already up, and reads it as a hang.
 * The toast that would have explained it never appears either: the main loop
 * does `if (idle) continue;` before it reaches ui_draw_dynamic(), so in idle
 * mode nothing draws toasts at all. A static line is the only thing that
 * survives here. */
/* ---- boot progress ---------------------------------------------------------
 * Reading the .m3u is ~19 APF commands and the CPU spends nearly all of that
 * spinning in target_read_slot()'s completion poll. Dead time, with a static
 * splash on screen and no way to tell a slow card from a hung one.
 *
 * So the wait gets a voice: a label plus three dots whose brightness sweeps
 * across them. The falloff is deliberately the SAME arithmetic the transport
 * arrows use -- rotating peak, trailing glow, 30 Hz tick -- so it reads as this
 * UI's existing language rather than a second idiom bolted on.
 *
 * Driven from inside the read poll, not from a timer. That matters: the dots
 * animate against the REAL load, so a slow card keeps them moving and they stop
 * because the work they were reporting actually finished. A fixed-duration
 * animation would be a lie that happens to look similar.
 *
 * Costs nothing. There is no audio at boot, which is the same reason
 * ui_splash_anim() can redraw a whole meter every frame here and nowhere else.
 */
/* The SAME row the playlist summary lands on, deliberately. The two are
 * sequential, never simultaneous -- the indicator runs while the .m3u is being
 * read and ui_boot_clear() wipes the row the moment it is done, which is
 * exactly when PLAYLIST / N TRACKS replaces it. One status line that changes
 * what it says, rather than two lines where one is always blank. */
#define UI_BOOT_Y     UI_SPL_INFO_Y
#define UI_DOT_N      3u
#define UI_DOT_W      7u
#define UI_DOT_GAP    6u
#define UI_DOT_STEPS  10u
#define UI_DOT_TICKS  (UI_DOT_N * UI_DOT_STEPS)
#define UI_DOT_TAIL   (UI_DOT_STEPS * 2u)   /* how far the glow trails behind */
#define UI_DOT_FLOOR  7u       /* out of 31: unlit dots dim, never vanish */

static const char *ui_boot_msg;        /* NULL = nothing in progress */
static uint32_t    ui_boot_next, ui_boot_t, ui_boot_x;

static uint16_t ui_boot_bg(void)
{
    return ui_grad_at(UI_BOOT_Y);
}

/* A 7x7 disc, one rect per row -- the way every other icon here is built,
 * because BRAM is at 97% and a glyph would cost a whole atlas cell. */
static void ui_icon_dot(uint32_t x, uint32_t y, uint16_t c)
{
    static const unsigned char inset[7] = { 2, 1, 0, 0, 0, 1, 2 };
    for (uint32_t i = 0; i < 7u; i++)
        fb_rect(x + inset[i], y + i, 7u - 2u * inset[i], 1u, c);
}

static void ui_boot_note(const char *msg)
{
    ui_boot_msg  = msg;
    ui_boot_t    = 0;
    ui_boot_next = 0;                       /* first tick paints immediately */
    /* WIPE FIRST. On the splash this row is empty so it never mattered, but in
     * the player it is the transport row -- PLAYING, the repeat and shuffle
     * arrows, the EQ name -- and writing over it left the old glyphs showing
     * around the shorter new text. */
    fb_rect(UI_MARGIN, UI_BOOT_Y, UI_INNER_W, FB_CELL(TS_1X), ui_boot_bg());

    /* Retire any live toast. This row and the toast band are both "what is
     * happening", and they must not disagree.
     *
     * Concretely: ui_draw_chrome() invalidates ui_toast_step so a toast
     * survives a repaint, which also means the PREVIOUS load's toast gets
     * redrawn for a frame as the next load completes -- seen as a quick flash
     * of stale text on the toast line. Killing it here means the newer message
     * is the only one making a claim. */
    ui_toast_t0   = 0;
    ui_toast_step = 0;
    fb_rect(UI_MARGIN, UI_TOAST_Y, UI_INNER_W, UI_TOAST_H, ui_grad_at(UI_TOAST_Y));

    fb_set_color(UI_WHITE, ui_boot_bg());
    /* Left-aligned at UI_MARGIN, like every other row on this screen and on
     * the player it is imitating. An earlier version centred the label and its
     * dots; centring reads as a different screen, which is the whole thing
     * this layout exists to avoid.
     *
     * fb_text_clipped returns where it stopped, so the dots follow the label
     * instead of sitting at a hardcoded offset that a longer word would run
     * into. */
    ui_boot_x = fb_text_clipped(UI_MARGIN, UI_BOOT_Y, msg,
                                TS_1X, TS_1X, UI_INNER_W) + 8u;
}

static void ui_boot_tick(void)
{
    if (!ui_boot_msg) return;
    if ((int32_t)(cycles() - ui_boot_next) < 0) return;
    ui_boot_next = cycles() + CLK_HZ / 30u;

    uint16_t bg = ui_boot_bg();
    uint32_t dy = UI_BOOT_Y + (FB_CELL(TS_1X) - 7u) / 2u;   /* centred on the text */
    uint32_t t  = ui_boot_t++ % UI_DOT_TICKS;

    /* No erase pass: every dot is redrawn every tick at a fixed position, so
     * each one covers its own footprint. Erase-then-draw on a single-buffered
     * framebuffer is what makes things flicker when scanout catches the gap. */
    for (uint32_t i = 0; i < UI_DOT_N; i++) {
        uint32_t dist = (t + UI_DOT_TICKS - i * UI_DOT_STEPS) % UI_DOT_TICKS;
        uint32_t l    = (dist < UI_DOT_TAIL) ? (31u - (dist * 31u) / UI_DOT_TAIL)
                                             : 0u;
        /* A floor rather than skipping the dim ones: three dots that are always
         * present read as a three-dot indicator, where two-plus-a-gap reads as
         * something missing. The arrows can afford to vanish; a 7 px disc
         * cannot. */
        if (l < UI_DOT_FLOOR) l = UI_DOT_FLOOR;
        ui_icon_dot(ui_boot_x + i * (UI_DOT_W + UI_DOT_GAP), dy,
                    ui_mix(bg, ui_accent, l, 31u));
    }
}

static void ui_boot_clear(void)
{
    if (!ui_boot_msg) return;
    ui_boot_msg = 0;
    fb_rect(UI_MARGIN, UI_BOOT_Y, UI_INNER_W, FB_CELL(TS_1X), ui_boot_bg());
}

/* Disarm WITHOUT repainting the row -- for callers whose next step redraws the
 * screen anyway.
 *
 * load_track() calls ui_draw_chrome() partway through, so by the time it
 * returns the player is already on screen and ui_boot_clear()'s wipe would
 * punch a gradient-coloured rectangle straight through the transport row.
 * Disarming still matters: ui_boot_tick() runs inside every blocking read, so
 * a note left armed would keep painting dots over the player forever. */
static void ui_boot_cancel(void) { ui_boot_msg = 0; }

/* What the card turned out to hold, on the row the transport occupies during
 * playback. A labelled pair rather than a bare number: "10 TRACKS" alone in the
 * middle of a screen reads as a caption for nothing, where PLAYLIST on the left
 * and the count on the right reads like the spec row on a piece of gear -- and
 * matches how the transport row is laid out a moment later.
 *
 * Counts LIVE entries, matching the N-of-M shown during playback: a number here
 * that disagreed with the one a second later would undermine both. */
static void ui_splash_summary(uint32_t n)
{
    char b[24];
    uint32_t i = 0;
    if (n >= 100u) b[i++] = (char)('0' + n / 100u % 10u);
    if (n >= 10u)  b[i++] = (char)('0' + n / 10u  % 10u);
    b[i++] = (char)('0' + n % 10u);
    const char *tail = (n == 1u) ? " TRACK" : " TRACKS";
    for (uint32_t k = 0; tail[k] && i < sizeof(b) - 1u; k++) b[i++] = tail[k];
    b[i] = 0;

    uint16_t bg = ui_grad_at(UI_SPL_INFO_Y);
    uint32_t w  = fb_text_width(b, TS_1X);   /* hoisted: the label clips to it */
    fb_set_color(UI_DIM, bg);
    /* A clipped list has to say so here too. The count alone is the one thing
     * that cannot reveal it -- a playlist cut to 128 looks exactly like a
     * playlist of 128. */
    /* Name the playlist rather than saying "PLAYLIST", so a user running more
     * than one can see which is loaded. Falls back to the generic word when
     * APF will not say. */
    const char *lbl = pl_truncated ? "PLAYLIST CLIPPED"
                    : pl_name[0]   ? pl_name
                    :                "PLAYLIST";
    fb_text_clipped(UI_MARGIN, UI_SPL_INFO_Y, lbl,
                    TS_1X, TS_1X, UI_INNER_W - w - 12u);
    fb_set_color(ui_accent, bg);
    fb_text_clipped(FB_W - UI_MARGIN - w, UI_SPL_INFO_Y, b, TS_1X, TS_1X, w);
}

static inline uint32_t dt_read(uint32_t word);   /* defined with the playlist code */

/* APF's datatable exactly as it stood before this core touched it. 1 KB, taken
 * once at boot, because by the time anyone can press a button our own 0190 has
 * already overwritten words 0..63. */
static uint32_t dt_snap[256];

static void dt_snapshot(void)
{
    for (uint32_t w = 0; w < 256u; w++) dt_snap[w] = dt_read(w);
}

/* APF's dataslot ID/size table, BOOT value against LIVE value.
 *
 * The table is {slot_id, size} pairs at stride 2 from word 0 -- measured, not
 * assumed. Our 0190 response struct used to sit at word 0 and APF overwrote 64
 * words there on every getfile, destroying the table on the first track change.
 * The structs now live at words 64+.
 *
 * This screen is the verification: change track, then press Select+A. If BOOT
 * and LIVE still agree, the table survived and the fix holds. If LIVE has
 * turned into path characters, something is still writing over it.
 */
static void dt_dump_boot(void)
{
    fb_rect(0, 0, FB_W, FB_H, UI_BG);
    fb_set_color(ui_accent, UI_BG);
    fb_text_clipped(UI_MARGIN, 6u, "SLOT TABLE  boot / live", TS_1X, TS_1X,
                    UI_INNER_W);

    char b[44], *q;
    uint32_t y = 26u;
    int bad = 0;

    for (uint32_t w = 0; w < 8u; w++) {
        uint32_t live = dt_read(w);
        /* A size entry is ALLOWED to change: APF updates it when the slot's
         * file changes, which is the whole point of the table. Slot 2 is the
         * MP3 slot and moves on every track change -- flagging that as damage
         * was wrong and reported a healthy table as clobbered.
         *
         * What must never move: the slot IDs (even words), and the sizes of
         * the three files that are fixed for the session. */
        int may_change = (w == 3u);          /* slot 2's size */
        if (live != dt_snap[w] && !may_change) bad = 1;
        q = b;
        *q++ = 'w'; q = ui_dec(q, w);
        while (q - b < 4) *q++ = ' ';
        *q++ = ' ';
        for (int sh = 28; sh >= 0; sh -= 4) {
            uint32_t n = (dt_snap[w] >> sh) & 0xFu;
            *q++ = (char)(n < 10u ? ('0' + n) : ('A' + n - 10u));
        }
        *q++ = ' ';
        *q++ = (live == dt_snap[w]) ? '=' : (may_change ? '~' : '!');
        *q++ = ' ';
        for (int sh = 28; sh >= 0; sh -= 4) {
            uint32_t n = (live >> sh) & 0xFu;
            *q++ = (char)(n < 10u ? ('0' + n) : ('A' + n - 10u));
        }
        *q = 0;
        fb_set_color((live == dt_snap[w]) ? UI_WHITE
                                          : (may_change ? UI_DIM : UI_RED), UI_BG);
        fb_text_clipped(UI_MARGIN, y, b, TS_1X, TS_1X, UI_INNER_W);
        y += 17u;
    }

    y += 8u;
    fb_set_color(bad ? UI_RED : ui_accent, UI_BG);
    fb_text_clipped(UI_MARGIN, y, bad ? "TABLE CLOBBERED" : "TABLE INTACT  (~ = ok)",
                    TS_1X, TS_1X, UI_INNER_W);
    y += 22u;

    /* Where our own structs live now, so the screen is self-describing. */
    fb_set_color(UI_DIM, UI_BG);
    fb_text_clipped(UI_MARGIN, y, "resp w64  param w128  set w192",
                    TS_1X, TS_1X, UI_INNER_W);
    y += 17u;

    /* And the live words our structs occupy, to confirm they are where we
     * think and not somewhere unexpected. */
    q = b;
    const char *t = "w64 "; while (*t) *q++ = *t++;
    for (int sh = 28; sh >= 0; sh -= 4) {
        uint32_t n = (dt_read(64u) >> sh) & 0xFu;
        *q++ = (char)(n < 10u ? ('0' + n) : ('A' + n - 10u));
    }
    t = "  w128 "; while (*t) *q++ = *t++;
    for (int sh = 28; sh >= 0; sh -= 4) {
        uint32_t n = (dt_read(128u) >> sh) & 0xFu;
        *q++ = (char)(n < 10u ? ('0' + n) : ('A' + n - 10u));
    }
    *q = 0;
    fb_text_clipped(UI_MARGIN, y, b, TS_1X, TS_1X, UI_INNER_W);
}

/* One line of the getting-started screen.
 *
 * The background comes from ui_grad_at(y), NOT from one colour sampled once:
 * glyphs paint their own background, so a line drawn low on the screen with a
 * colour taken from the middle sits in a rectangle of visibly the wrong shade.
 * The old three-line block spanned 70 px and got away with it; this one spans
 * nearly 200. Clipped to UI_INNER_W so there is a real right margin -- the
 * previous FB_W - UI_MARGIN let a long line run to the very edge. */
static void ui_gs_line(uint32_t y, const char *s, uint16_t fg, uint32_t ts)
{
    fb_set_color(fg, ui_grad_at(y));
    fb_text_clipped(UI_MARGIN, y, s, ts, ts, UI_INNER_W);
}

/* Shown when there is nothing to play: no playlist, an empty one, or one whose
 * every entry is missing. It is the first thing a new user sees, so it is a
 * getting-started card rather than a bare error -- the three steps are the
 * whole setup, in the order they have to happen. Widths were measured against
 * font_metrics.h; the widest line is 310 px of the 360 available. */
static void ui_idle_screen(const char *reason)
{
    ui_splash();

    /* Sits between the card (ends at 136) and the heading (170), 8 px clear of
     * each. At 148 it crowded the heading and read as part of it rather than
     * as a separate alert. */
    if (reason) ui_gs_line(144u, reason, UI_RED, TS_1X);

    ui_gs_line(170u, "Getting started",                     ui_accent, TS_15X);

    /* 18 px within a step, 26 between them. An even pitch throughout made the
     * three steps read as one eight-line block -- the grouping has to be
     * visible or the numbers are doing all the work. */
    /* Colour carries meaning here, so it follows one rule: prose is white,
     * literal values and menu names are grey. That is why step 2 stays white
     * across both its lines -- they are one sentence, and changing colour
     * halfway through it reads as a mistake -- while the path and the two
     * menu entries are grey. */
    ui_gs_line(206u, "1  Copy .mp3 files to your SD card:", UI_WHITE,  TS_1X);
    ui_gs_line(224u, "   /Assets/mp3player/common/",        UI_DIM,    TS_1X);

    ui_gs_line(250u, "2  For the best experience, list them", UI_WHITE, TS_1X);
    ui_gs_line(268u, "   in a playlist.m3u in that folder.",  UI_WHITE, TS_1X);

    /* The run of spaces is not eyeballed: the font is proportional, so the
     * second column only lines up because the padding was computed from
     * font_adv[] to put both at x = MARGIN + 134. Retyping either label
     * without recomputing will break the alignment. */
    ui_gs_line(294u, "3  Press the Analogue button:",       UI_WHITE,  TS_1X);
    ui_gs_line(312u, "   Load MP3          one track",      UI_DIM,    TS_1X);
    ui_gs_line(330u, "   Load Playlist   your whole .m3u",  UI_DIM,    TS_1X);
}

static void ui_load_failed(void)
{
    /* Wake first if the screen is dark. This draws straight to the framebuffer
     * rather than through ui_draw_dynamic(), so without this it would paint
     * over a blanked screen while screen_blank stayed set -- lit, but frozen,
     * with every later update still suppressed and nothing to clear it.
     *
     * Waking is also the right call on its own terms. Blanking exists so
     * routine events -- track changes, meters, toasts -- do not light the panel
     * while music plays. Playback having STOPPED is not routine, and a silent
     * player behind a dark screen gives the user nothing to act on. The idle
     * timer restarts from here, so it blanks again on its own. */
    ui_blank_wake();
    fb_rect(0, 0, FB_W, FB_H, UI_BG);
    fb_set_color(UI_RED, UI_BG);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y, "LOAD FAILED", TS_2X, TS_2X, UI_INNER_W);

    char b[24], *p = b;
    const char *t = "HEAD ";
    while (*t) *p++ = *t++;
    for (int i = 0; i < 4; i++) p = ui_hex2(p, head_bytes[i]);
    *p = 0;
    fb_set_color(UI_DIM, UI_BG);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y + FB_CELL(TS_2X) + 14u, b,
                    TS_1X, TS_1X, UI_INNER_W);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y + FB_CELL(2u) + 40u,
                    "USE CORE MENU TO PICK", TS_1X, TS_1X, UI_INNER_W);

    /* Stay alive so a reload can rescue us. */
    for (;;) {
        poll_input();
        if (reload_pending) return;
    }
}

/* Called from the main loop every frame. Under rev 6 this had to be throttled
 * to 1-in-3 and stripped back, because each element cost one command PER ROW
 * and a clock redraw alone was ~180 MMIO round-trips landing in the same
 * per-frame budget that keeps the PCM FIFO fed. The engine owns those loops
 * now: the meter is 2 commands regardless of height, the clock is 5, and text
 * paints its own background so nothing needs erasing first. That is roughly a
 * 30x reduction, which is why the throttle is gone.
 *
 * The delta-guards below stay -- not for cost any more, but because redrawing
 * an unchanged value would make the meter and clock flicker as they are
 * rewritten mid-scanout (the framebuffer is single-buffered). */
static void ui_draw_dynamic(void)
{
    if (screen_blank) return;
    /* Shift a new sample in and repaint the band. Every bar moves each update,
     * so there is nothing to gain from change-detection here -- instead it is
     * throttled, and each bar is two rects (lit + unlit), which the engine
     * draws in one command apiece. */
    /* A new accent has to reach three separate things, each of which only
     * redraws when its own value changes -- so they are all invalidated
     * explicitly rather than waiting for the music to move them. */
    if (ui_accent_changed) {
        ui_accent_changed = 0;
        ui_accent = ui_palette[ui_pal_idx];
        /* The BACKGROUND is tinted from the accent now, so a colour change is
         * no longer a matter of recolouring a few elements -- the whole screen
         * is a different colour underneath them, and every cached element sits
         * on it. Hence a full repaint here, where the explicit invalidations
         * below were previously enough on their own.
         *
         * This is the one place the tint costs something: a repaint is ~360
         * gradient rects plus the chrome, pushed while the decoder is not
         * running. Colour is changed by a deliberate button press rather than
         * continuously, so it should stay under the FIFO's reserve -- but this
         * is the thing to listen for if a colour change ever ticks. */
        ui_grad_set(ui_accent);
        ui_draw_chrome();
        /* No invalidation list here any more. This used to repeat most of
         * ui_draw_chrome's, and keeping two copies is exactly how the mode row
         * ended up missing from the one that mattered. ui_draw_chrome repaints
         * the whole screen and now owns the full list.
         *
         * ui_wave_force is NOT invalidation -- it is a behaviour request, to
         * recolour the meter even while paused, which nothing else would do. */
        ui_wave_force = 1;
    }

    /* Frozen, not decaying, while paused: shifting the history along with a
     * zero sample scrolled the whole waveform off the screen, which reads as
     * "lost the audio" rather than "stopped". */
    /* The VU keeps updating while paused until both needles reach rest: a
     * meter that freezes mid-deflection looks broken, and the fall to zero is
     * the most characterful thing an analogue movement does. Every other mode
     * is genuinely static when paused and stays frozen. */
    uint32_t vu_settling = (viz_mode == VIZ_VU) && (vu_l || vu_r);
    if ((!paused || ui_wave_force || vu_settling) && ++ui_last_vu >= 2u) {
        ui_last_vu = 0;

        uint32_t wf = ui_wave_force; ui_wave_force = 0;
        uint32_t amp = (peak_amp * UI_WAVE_H) / 32768u;
        if (amp > UI_WAVE_H) amp = UI_WAVE_H;
        /* Frozen while paused: the forced pass exists only to recolour. */
        if (!paused) {
            for (uint32_t i = 0; i < UI_WAVE_N - 1u; i++) {
                wave[i]    = wave[i + 1];
                wave_pk[i] = wave_pk[i + 1];
            }
            wave[UI_WAVE_N - 1u]    = (unsigned char)amp;
            wave_pk[UI_WAVE_N - 1u] = (unsigned char)amp;

        }
        (void)wf;
        /* Peaks sink slowly back toward the bar, so the marker trails the
         * loudest recent moment instead of sitting at the ceiling. */
        for (uint32_t i = 0; i < UI_WAVE_N; i++)
            if (wave_pk[i] > wave[i]) wave_pk[i]--;

        uint32_t ww  = ui_wave_w();
        uint16_t bed = ui_grad_at(UI_WAVE_Y);

        /* ---- WATERFALL ----------------------------------------------------
         * Scroll the whole strip one pixel left with a single COPY, then draw
         * only the new right-hand column. That is ~4 commands a frame against
         * the bars' ~72, because COPY moves a block for the price of one
         * command -- the same primitive the album-art slide uses.
         *
         * Colour encodes loudness, so the strip becomes a picture of the
         * track's dynamics rather than an instantaneous reading. */
        /* ---- SCROLLING WAVEFORM -------------------------------------------
         * The waterfall's COPY-scroll, but the new column is drawn MIRRORED
         * about a centre line instead of colour-coded from the bottom -- a
         * DAW-style envelope building up left to right. ~5 commands a frame,
         * because COPY moves the whole strip for the price of one. */
        if (viz_mode == VIZ_SCROLL) {
            const uint32_t x0 = UI_MARGIN, w = ww;
            const uint32_t cy = UI_WAVE_Y + UI_WAVE_H / 2u;
            const uint32_t half = UI_WAVE_H / 2u - 1u;
            if (!paused) {
                fb_copy(x0 + 1u, UI_WAVE_Y, x0, UI_WAVE_Y, w - 1u, UI_WAVE_H);

                uint32_t a = (peak_amp * half) / 32768u;
                if (a > half) a = half;

                uint32_t cx = x0 + w - 1u;
                fb_rect(cx, UI_WAVE_Y, 1, UI_WAVE_H, bed);      /* clear column */
                if (a) fb_rect(cx, cy - a, 1, a * 2u + 1u,
                               ui_mix(UI_TRACK, ui_accent, a, half));
                else   fb_rect(cx, cy, 1, 1, UI_TRACK);         /* silence line */
            }
            goto viz_done;
        }

        /* ---- MIRRORED BARS ------------------------------------------------
         * The same wave[] history the bars use, grown up AND down from a
         * centre line. Same cost as the bars; different shape entirely. */
        if (viz_mode == VIZ_MIRROR) {
            const uint32_t cy = UI_WAVE_Y + UI_WAVE_H / 2u;
            const uint32_t half = UI_WAVE_H / 2u - 1u;
            for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                uint32_t x   = UI_MARGIN + (i * ww) / UI_WAVE_N;
                uint32_t xn  = UI_MARGIN + ((i + 1u) * ww) / UI_WAVE_N;
                uint32_t lit = (xn - x > UI_WAVE_GAP) ? (xn - x - UI_WAVE_GAP) : 1u;
                uint32_t h   = (wave[i] * half) / UI_WAVE_H;
                if (!h) h = 1u;
                uint16_t lc  = paused ? ui_mix(UI_TRACK, ui_accent, 1u, 3u) : ui_accent;
                uint16_t c   = ui_mix(UI_TRACK, lc, i + 1u, UI_WAVE_N);
                fb_rect(x, UI_WAVE_Y, lit, UI_WAVE_H, bed);
                fb_rect(x, cy - h, lit, h * 2u + 1u, c);
            }
            goto viz_done;
        }

        /* ---- PEAK DOTS ----------------------------------------------------
         * Only the peak-hold markers, no bars: a row of floating dots tracing
         * the loudness contour. ~2 commands a column and the sparsest mode
         * here. */
        if (viz_mode == VIZ_DOTS) {
            for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                uint32_t x   = UI_MARGIN + (i * ww) / UI_WAVE_N;
                uint32_t xn  = UI_MARGIN + ((i + 1u) * ww) / UI_WAVE_N;
                uint32_t lit = (xn - x > UI_WAVE_GAP) ? (xn - x - UI_WAVE_GAP) : 1u;
                uint32_t pk  = wave_pk[i];
                if (pk < 2u) pk = 2u;
                uint16_t c   = ui_mix(UI_TRACK, ui_accent, i + 1u, UI_WAVE_N);
                fb_rect(x, UI_WAVE_Y, lit, UI_WAVE_H, bed);
                fb_rect(x, UI_WAVE_Y + UI_WAVE_H - pk, lit, 2u, c);
            }
            goto viz_done;
        }

        if (viz_mode == VIZ_WATER) {
            const uint32_t x0 = UI_MARGIN, w = ww;
            if (!paused) {
                fb_copy(x0 + 1u, UI_WAVE_Y, x0, UI_WAVE_Y, w - 1u, UI_WAVE_H);

                uint32_t a = (peak_amp * UI_WAVE_H) / 32768u;
                if (a > UI_WAVE_H) a = UI_WAVE_H;

                /* Column drawn as three bands -- quiet bed, body, hot tip --
                 * so loud passages read as brighter AND taller. */
                uint32_t cx = x0 + w - 1u;
                fb_rect(cx, UI_WAVE_Y, 1, UI_WAVE_H - a, bed);
                if (a) {
                    uint16_t c = ui_mix(UI_TRACK, ui_accent, a, UI_WAVE_H);
                    fb_rect(cx, UI_WAVE_Y + UI_WAVE_H - a, 1, a, c);
                    fb_rect(cx, UI_WAVE_Y + UI_WAVE_H - a, 1, 1, UI_WHITE);
                }
            }
            goto viz_done;
        }

        /* ---- VU METERS ----------------------------------------------------
         * Two analogue movements side by side. Geometry is derived from
         * ui_wave_w() every pass rather than assumed: hiding the album art
         * widens the box from ~246 to ~360, and a fixed layout would leave the
         * pair huddled at the left -- the same trap the waterfall fell into. */
        if (viz_mode == VIZ_VU) {
            /* The face -- arc, ticks, labels -- never changes, so it is drawn
             * ONCE and left alone. Clearing the whole box and repainting
             * everything each pass is what made the L and R labels flicker:
             * they were being erased and redrawn while the panel was being
             * scanned out. Only the needle is erased and redrawn now. */
            /* Width changes with the art panel, and the face geometry is
             * derived from it, so a cached face drawn at another width is
             * stale even if nothing erased it. */
            if (wf || ww != vu_face_w) vu_face = 0;

            if (!vu_face) fb_rect(UI_MARGIN, UI_WAVE_Y, ww, UI_WAVE_H, bed);

            for (int ch = 0; ch < 2; ch++) {
                uint32_t half = ww / 2u;
                uint32_t ox   = UI_MARGIN + (uint32_t)ch * half;
                uint32_t pivx = ox + half / 2u;
                uint32_t pivy = UI_WAVE_Y + UI_WAVE_H - 4u;
                uint32_t len  = UI_WAVE_H - 18u;
                /* Needle stops short of the ticks, so erasing it can never rub
                 * them out and they never need repainting. */
                uint32_t nlen = len - 6u;

                uint32_t pkc = ch ? peak_r : peak_l;
                uint32_t tgt = (pkc * 255u) / 32768u;
                if (tgt > 255u) tgt = 255u;
                uint32_t *v = ch ? &vu_r : &vu_l;
                if (paused) tgt = 0;
                if (tgt > *v) { *v += VU_ATT; if (*v > tgt) *v = tgt; }
                else          { *v = (*v > VU_DEC) ? (*v - VU_DEC) : 0u;
                                if (*v < tgt) *v = tgt; }

                if (!vu_face) {
                    for (uint32_t t = 0; t <= 80u; t++) {
                        uint32_t q = (t * 16u) / 80u, f = (t * 16u) % 80u;
                        uint32_t q1 = (q < 16u) ? q + 1u : 16u;
                        int32_t sn = vu_sn[q] + (int32_t)((vu_sn[q1] - vu_sn[q]) * (int32_t)f) / 80;
                        int32_t cs = vu_cs[q] + (int32_t)((vu_cs[q1] - vu_cs[q]) * (int32_t)f) / 80;
                        int32_t ar = (int32_t)len + 4;
                        int32_t ax = (int32_t)pivx + (ar * sn) / 4096;
                        int32_t ay = (int32_t)pivy - (ar * cs) / 4096;
                        if (ay < (int32_t)UI_WAVE_Y) continue;
                        if (ax < (int32_t)ox || ax + 1 >= (int32_t)(ox + half)) continue;
                        /* Everything on the face is a TONE OF THE ACCENT.
                         * Fixed grey and red meant changing colour only moved
                         * the needle and the labels, and the meter looked
                         * unchanged. The peak zone is the accent at full
                         * strength against a dimmed scale, so it still reads as
                         * "the loud end" in any palette. */
                        fb_rect((uint32_t)ax, (uint32_t)ay, 2, 2,
                                (t >= 60u) ? ui_accent
                                           : ui_mix(bed, ui_accent, 2u, 5u));
                    }
                    for (uint32_t t = 0; t <= 4u; t++) {
                        uint32_t i = t * 4u;
                        for (uint32_t d = 0; d < 4u; d++) {
                            int32_t ar = (int32_t)len - 1 - (int32_t)d;
                            int32_t ax = (int32_t)pivx + (ar * vu_sn[i]) / 4096;
                            int32_t ay = (int32_t)pivy - (ar * vu_cs[i]) / 4096;
                            if (ay < (int32_t)UI_WAVE_Y) continue;
                            fb_rect((uint32_t)ax, (uint32_t)ay, 1, 1,
                                    (t >= 3u) ? ui_accent
                                              : ui_mix(bed, ui_accent, 3u, 5u));
                        }
                    }
                    fb_set_color(ui_accent, bed);
                    fb_text_clipped(ox + 6u, UI_WAVE_Y + 2u, ch ? "R" : "L",
                                    TS_1X, TS_1X, 16u);
                }

                uint8_t shown = ch ? vu_shown_r : vu_shown_l;
                uint8_t now   = (uint8_t)*v;
                if (vu_face && now == shown) continue;   /* nothing moved */

                /* Erase the old needle, then draw the new one. Two passes over
                 * the same geometry costs less than repainting the face. The
                 * erase is skipped on the first draw after the face is laid
                 * down, when there is no old needle to remove. */
                for (int pass = vu_face ? 0 : 1; pass < 2; pass++) {
                    int32_t  sn, cs;
                    vu_angle(pass ? now : shown, &sn, &cs);
                    /* One colour throughout its travel. Flashing at the top drew the eye
                     * to the loudest moments, which is the opposite of what a
                     * meter is for -- the scale already marks the peak zone. */
                    uint16_t col = pass ? ui_accent : bed;
                    for (uint32_t k = 2; k <= VU_STEPS; k++) {
                        int32_t rr = ((int32_t)nlen * (int32_t)k) / (int32_t)VU_STEPS;
                        int32_t nx = (int32_t)pivx + (rr * sn) / 4096;
                        int32_t ny = (int32_t)pivy - (rr * cs) / 4096;
                        if (nx < (int32_t)ox || nx >= (int32_t)(ox + half)) continue;
                        if (ny < (int32_t)UI_WAVE_Y) continue;
                        uint32_t th = (k > VU_STEPS - 6u) ? 1u : 2u;
                        fb_rect((uint32_t)nx, (uint32_t)ny, th, th, col);
                    }
                }
                fb_rect(pivx - 2u, pivy - 2u, 5, 5, ui_accent);
                fb_rect(pivx - 1u, pivy - 1u, 3, 3, bed);

                if (ch) vu_shown_r = now; else vu_shown_l = now;
            }
            vu_face = 1;
            vu_face_w = (uint16_t)ww;
            goto viz_done;
        }

        /* ---- OSCILLOSCOPE -------------------------------------------------
         * One clear, then one vertical rect per column: ~65 commands, fewer
         * than the bars. */
        if (viz_mode == VIZ_WAVE) {
            const int32_t ey = (int32_t)(UI_WAVE_H / 2u) - 1;
            const uint32_t cy = UI_WAVE_Y + UI_WAVE_H / 2u;

            fb_rect(UI_MARGIN, UI_WAVE_Y, ww, UI_WAVE_H, bed);
            fb_rect(UI_MARGIN, cy, ww, 1, UI_TRACK);      /* zero line */

            if (!paused) {
                int32_t prev_y = 0;
                for (uint32_t c = 0; c < WAVE_COLS; c++) {
                    uint32_t x  = UI_MARGIN + (c * ww) / WAVE_COLS;
                    uint32_t xn = UI_MARGIN + ((c + 1u) * ww) / WAVE_COLS;
                    uint32_t w  = (xn > x) ? (xn - x) : 1u;

                    int32_t v = (wav_v[c] * ey) / SCOPE_UNIT;
                    if (v >  ey) v =  ey;
                    if (v < -ey) v = -ey;

                    /* Span from the previous sample to this one, so the trace
                     * is continuous rather than a row of disconnected marks --
                     * and stays thin, because consecutive samples in a short
                     * window are close together. */
                    int32_t a = (c == 0) ? v : prev_y;
                    int32_t lo = (a < v) ? a : v;
                    int32_t hi = (a < v) ? v : a;
                    prev_y = v;

                    uint32_t top = (uint32_t)((int32_t)cy - hi);
                    uint32_t h   = (uint32_t)(hi - lo) + 2u;   /* min 2 px line */
                    if (top + h > UI_WAVE_Y + UI_WAVE_H) h = UI_WAVE_Y + UI_WAVE_H - top;
                    uint16_t col = ui_mix(UI_TRACK, ui_accent, c + 1u, WAVE_COLS);
                    fb_rect(x, top, w, h, col);
                }
            }
            goto viz_done;
        }

        /* ---- STEREO PHASE SCOPE -------------------------------------------
         * One rect to clear, then one per point: ~65 commands, fewer than the
         * bars. The whole trace is redrawn each pass rather than erased point
         * by point, which would double the count for no gain. */
        if (viz_mode == VIZ_SCOPE) {
            const uint32_t r  = UI_WAVE_H / 2u;          /* usable radius */
            const uint32_t cx = UI_MARGIN + ww / 2u;
            const uint32_t cy = UI_WAVE_Y + r;

            fb_rect(UI_MARGIN, UI_WAVE_Y, ww, UI_WAVE_H, bed);

            /* Centre cross: without it a quiet passage is an empty box, and
             * there is no way to tell "silent" from "not working". */
            fb_rect(cx, UI_WAVE_Y, 1, UI_WAVE_H, UI_TRACK);
            fb_rect(UI_MARGIN, cy, ww, 1, UI_TRACK);

            if (!paused) {
                const int32_t ex = (int32_t)(ww / 2u) - 2;   /* horizontal reach */
                const int32_t ey = (int32_t)r - 2;           /* vertical reach   */
                /* Oldest first, so the newest trace lands on top of the
                 * fading ones rather than under them. */
                for (uint32_t age = SCOPE_HIST; age-- > 0; ) {
                    uint32_t f = (scope_head + SCOPE_HIST - age) % SCOPE_HIST;
                    const signed char *sx = scope_x[f], *sy = scope_y[f];
                    uint16_t c  = ui_mix(bed, ui_accent, SCOPE_HIST - age, SCOPE_HIST);
                    uint32_t sz = age ? 1u : 2u;     /* newest trace is fatter */
                    for (uint32_t k = 0; k < SCOPE_N; k++) {
                        int32_t px = (int32_t)cx + (sx[k] * ex) / SCOPE_UNIT;
                        int32_t py = (int32_t)cy - (sy[k] * ey) / SCOPE_UNIT;
                        if (px < (int32_t)UI_MARGIN ||
                            px + (int32_t)sz > (int32_t)(UI_MARGIN + ww)) continue;
                        if (py < (int32_t)UI_WAVE_Y ||
                            py + (int32_t)sz > (int32_t)(UI_WAVE_Y + UI_WAVE_H)) continue;
                        fb_rect((uint32_t)px, (uint32_t)py, sz, sz, c);

                        /* Newest trace only: drop a point midway to the next
                         * sample so the figure closes into a curve instead of
                         * a dotted outline. Only the top layer gets this --
                         * doing it on every frame of history would triple the
                         * command count for detail that is fading out anyway. */
                        if (!age && k + 1u < SCOPE_N) {
                            int32_t qx = (int32_t)cx + (((sx[k] + sx[k+1]) / 2) * ex) / SCOPE_UNIT;
                            int32_t qy = (int32_t)cy - (((sy[k] + sy[k+1]) / 2) * ey) / SCOPE_UNIT;
                            if (qx >= (int32_t)UI_MARGIN &&
                                qx + 1 < (int32_t)(UI_MARGIN + ww) &&
                                qy >= (int32_t)UI_WAVE_Y &&
                                qy + 1 < (int32_t)(UI_WAVE_Y + UI_WAVE_H))
                                fb_rect((uint32_t)qx, (uint32_t)qy, 1, 1, c);
                        }
                    }
                }
            }
            goto viz_done;
        }

        /* ---- L/R LEVELS ---------------------------------------------------
         * The only mode that uses stereo information; the others collapse both
         * channels into one number. Two bars plus two peak-hold markers. */
        if (viz_mode == VIZ_LEVELS) {
            uint32_t la = (peak_l * ww) / 32768u; if (la > ww) la = ww;
            uint32_t ra = (peak_r * ww) / 32768u; if (ra > ww) ra = ww;
            if (!paused) { lvl_l = (unsigned char)(la * 255u / (ww ? ww : 1u));
                           lvl_r = (unsigned char)(ra * 255u / (ww ? ww : 1u)); }
            if (lvl_l > lvl_pl) lvl_pl = lvl_l; else if (lvl_pl) lvl_pl--;
            if (lvl_r > lvl_pr) lvl_pr = lvl_r; else if (lvl_pr) lvl_pr--;

            const uint32_t bh = UI_WAVE_H / 3u;          /* bar height */
            const uint32_t gap = UI_WAVE_H - 2u * bh;    /* space between */
            uint16_t lit = paused ? ui_mix(UI_TRACK, ui_accent, 1u, 3u) : ui_accent;
            for (int ch = 0; ch < 2; ch++) {
                uint32_t y  = UI_WAVE_Y + (ch ? bh + gap : 0u);
                uint32_t v  = ch ? lvl_r : lvl_l;
                uint32_t pk = ch ? lvl_pr : lvl_pl;
                uint32_t bw = (v  * ww) / 255u;
                uint32_t px = (pk * ww) / 255u;
                if (bw) fb_rect(UI_MARGIN, y, bw, bh, lit);
                if (bw < ww) fb_rect(UI_MARGIN + bw, y, ww - bw, bh, UI_TRACK);
                if (px > bw + 1u && px < ww)
                    fb_rect(UI_MARGIN + px, y, 1, bh, UI_WHITE);
            }
            goto viz_done;
        }

        for (uint32_t i = 0; i < UI_WAVE_N; i++) {
            /* Bar edges come from scaling the index across the full width, so
             * the row always reaches its right edge. A single per-bar width
             * (ww / N) throws away the remainder -- at 36 bars in 246 px that
             * was 30 px of unused space piling up at the end, which is what
             * made the meter look like it stopped well short of the art. */
            uint32_t x   = UI_MARGIN + (i * ww) / UI_WAVE_N;
            uint32_t xn  = UI_MARGIN + ((i + 1u) * ww) / UI_WAVE_N;
            uint32_t lit = (xn - x > UI_WAVE_GAP) ? (xn - x - UI_WAVE_GAP) : 1u;
            uint32_t h = wave[i];
            if (h < 2u) h = 2u;                      /* always show a floor */
            /* A scroll moves every bar, but in quiet or steady passages most
             * land on the height already drawn there. Skipping those costs one
             * compare and saves two SDRAM rect bursts each. */
            uint32_t pk = wave_pk[i];
            if (pk < h) pk = h;
            if (h == wave_drawn[i] && pk == wave_pk_drawn[i]) continue;
            wave_drawn[i]    = (unsigned char)h;
            wave_pk_drawn[i] = (unsigned char)pk;
            /* Newest bars brightest: a cheap sense of direction. */
            /* Paused pulls the whole meter back toward the bed, so stopping
             * reads as a state change across the UI rather than one word. */
            uint16_t lit_c = paused ? ui_mix(UI_TRACK, ui_accent, 1u, 3u)
                                    : ui_accent;
            uint16_t c = ui_mix(UI_TRACK, lit_c, i + 1u, UI_WAVE_N);
            fb_rect(x, UI_WAVE_Y + UI_WAVE_H - h, lit, h, c);
            if (UI_WAVE_H > h)
                fb_rect(x, UI_WAVE_Y, lit, UI_WAVE_H - h, bed);
            if (pk > h + 1u)                       /* 1 px peak-hold marker */
                fb_rect(x, UI_WAVE_Y + UI_WAVE_H - pk, lit, 1, UI_WHITE);
        }
    viz_done: ;
    }

    /* Sticky underrun latch (pcm_fifo.v) stays set until the next pcm_flush()
     * -- draw the indicator once when it first trips rather than every call;
     * costs nothing once shown. Cleared (screen wiped) by ui_draw_chrome() on
     * the next track load, which also clears the underrun flag via load_track.
     *
     * Switched OFF on request now that audio is healthy. Kept rather than
     * deleted because it is the fastest way to tell "the decoder lost the
     * race" from "something else is wrong with the audio" -- set
     * UI_SHOW_UNDERRUN back to 1 if stutter ever returns. */
#if UI_SHOW_UNDERRUN
    if (!ui_underrun_shown && pcm_underrun()) {
        ui_underrun_shown = 1;
        fb_rect(UI_UNDERRUN_X, UI_UNDERRUN_Y, UI_UNDERRUN_SZ, UI_UNDERRUN_SZ, UI_RED);
    }
#endif

    /* Elapsed time by running accumulator rather than (frames*1152)/samprate.
     * That is a 64-bit divide -- __udivdi3, hundreds of cycles in software on
     * RV32 -- and it ran EVERY frame inside the same budget that keeps the PCM
     * FIFO fed. Adds and compares give the identical answer. */
    if (samprate && frames != ui_last_frames) {
        ui_last_frames = frames;
        ui_sec_acc += samp_per_frame;
        while (ui_sec_acc >= samprate) { ui_sec_acc -= samprate; ui_sec++; }
    }
    uint32_t sec = ui_sec;

    /* Measured average: bytes actually consumed divided by elapsed time. Only
     * after a few seconds, so a partly-filled ring cannot skew it. */
    /* Measured over the window since the last seek, never over the whole file.
     *
     * Taking file_pos and sec as absolutes made this self-referential: ui_sec is
     * computed FROM meas_rate on every seek, and meas_rate was then recomputed
     * FROM ui_sec. Holding the seek fed that loop four times a second, the rate
     * inflated, the clock collapsed backwards and never recovered. It only ever
     * bit files with no Xing header, because track_secs short-circuits this
     * whole branch -- which is exactly why it failed on some songs and not
     * others. */
    if (!track_secs) {
        uint32_t buffered = ring_fill - ring_rd;
        uint32_t played   = (file_pos > buffered) ? file_pos - buffered : 0u;
        if (played > meas_pos0 && sec > meas_sec0 + 3u)
            meas_rate = (played - meas_pos0) / (sec - meas_sec0);
    }
    if (sec != ui_last_sec) {
        ui_last_sec = sec;
        /* Elapsed, and total when the file size makes it knowable. Padded to a
         * fixed width so the digits do not jitter as they change. */
        char buf[16], *q = buf;
        q = ui_mmss(q, sec);
        /* Always draw the field. Showing nothing when the length is unknown
         * makes a missing VALUE look identical to a broken DISPLAY, which has
         * already cost two rounds of chasing the wrong half. */
        uint32_t total = ui_total_secs();
        *q++ = ' '; *q++ = '/'; *q++ = ' ';
        if (total) q = ui_mmss(q, total);
        else { *q++ = '-'; *q++ = '-'; *q++ = ':'; *q++ = '-'; *q++ = '-'; }
        *q = 0;
        uint16_t cbg = ui_grad_at(UI_TIME_Y);
        /* Clear FIRST, then set the colour. fb_rect() writes the colour
         * registers (fg = bg = its fill), so setting the text colour before a
         * clear leaves fg == bg and the glyphs paint invisibly. */
        fb_rect(UI_MARGIN, UI_TIME_Y, UI_INNER_W, FB_CELL(TS_15X), cbg);
        fb_set_color(UI_WHITE, cbg);
        fb_text_clipped(UI_MARGIN, UI_TIME_Y, buf, TS_15X, TS_15X, UI_INNER_W);
    }

    /* Slide the art panel toward its target. Each step is a background repaint
     * of the strip plus ONE copy command, so the animation costs about a dozen
     * commands per step -- the whole reason the copy primitive exists. */
    {
        uint32_t target = art_shown ? ART_X : FB_W;
        if (art_x != target && (int32_t)(cycles() - art_next) >= 0) {
            art_next = cycles() + CLK_HZ / 60u;
            uint32_t stepn = 10u, prev = art_x;
            if (art_x > target) art_x = (art_x - target > stepn) ? art_x - stepn : target;
            else                art_x = (target - art_x > stepn) ? art_x + stepn : target;

            /* Repaint everything the panel no longer covers -- on BOTH sides.
             *
             * Moving right leaves a gap on the left, which was already handled.
             * The missed case was moving LEFT: while sliding in, the panel is
             * clipped at the screen edge, so at x=390 it painted 390..400, at
             * x=380 it painted 380..400, and so on. Once it settles at its rest
             * position its right edge is at 380 and everything it had drawn in
             * 380..400 during the slide was never cleaned up. That is the
             * residue on the right. */
            uint32_t right = art_x + ART_W;
            uint32_t dirty = 0;
            if (art_x > prev) { ui_art_bg_range(prev, art_x - prev); dirty = 1; }
            if (right < FB_W) { ui_art_bg_range(right, FB_W - right); dirty = 1; }
            if (dirty)
                for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                    wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
                }
            ui_art_draw();
        }
    }

    /* Marquee. Steps by whole characters rather than pixels: the engine draws
     * a glyph at any x but does not clip one partially off the left edge, so a
     * pixel scroll would need clipping support that does not exist. One step
     * every ~350 ms reads as a scroll without being distracting. */
    ui_marq_step(&ui_mq_title,  UI_WHITE);
    ui_marq_step(&ui_mq_artist, UI_DIM);

    /* Loud, and it stays: a wrong file size means the card's directory is
     * damaged, which will not fix itself and puts every file in that folder in
     * question -- not something to mention in a toast that scrolls away. */
    if (size_suspect && !ui_size_warned) {
        ui_size_warned = 1;
        fb_set_color(UI_RED, UI_PANEL);
        fb_text_clipped(UI_MARGIN, ui_info_y, "! FILE SIZE WRONG - CHECK SD CARD",
                        TS_1X, TS_1X, ui_text_w);
    }

    /* Format line, drawn once the decoder has told us what the stream is. */
    uint32_t info = track_kbps * 1000u + track_hz / 100u
                  + (track_encoder[0] ? (uint32_t)track_encoder[0] << 24 : 0u);
    if (track_kbps && info != ui_last_info && !size_suspect) {
        ui_last_info = info;
        char b[40], *q = b;
        q = ui_dec(q, track_kbps);
        *q++ = ' '; *q++ = 'k'; *q++ = 'b'; *q++ = 'p'; *q++ = 's';
        *q++ = ' '; *q++ = '-'; *q++ = ' ';
        q = ui_dec(q, track_hz / 1000u);
        *q++ = '.';
        q = ui_dec(q, (track_hz % 1000u) / 100u);
        *q++ = ' '; *q++ = 'k'; *q++ = 'H'; *q++ = 'z';
        /* Who encoded it, from the LAME tag beside the duration header. This
         * belongs here with the other format facts rather than in a screen of
         * its own -- and putting it here is what stops the bitrate being
         * repeated, which it was when this had its own view. */
        if (track_encoder[0]) {
            *q++ = ' '; *q++ = '-'; *q++ = ' ';
            for (const char *t = track_encoder; *t && q - b < 30; ) *q++ = *t++;
        }
        *q = 0;
        fb_set_color(UI_FAINT, UI_PANEL);
        fb_text_clipped(UI_MARGIN, ui_info_y, b, TS_1X, TS_1X, ui_text_w);
    }

    /* Thin progress bar. Total length comes from the file size APF reports
     * (0190/008A) minus the tag, divided by the byte rate -- there is no
     * duration in the stream itself unless the tag happens to carry TLEN. */
    /* Draw the track UNCONDITIONALLY. Previously the whole bar was gated on
     * knowing the duration, so an unknown file size meant nothing appeared at
     * all -- indistinguishable from the feature being broken. An empty bar
     * that never fills is at least self-describing. */
    /* Recomputed only when the displayed second moves -- the bar cannot change
     * faster than that, so running these divides every frame was pure waste. */
    uint32_t done = (ui_last_prog == 0xFFFFFFFFu) ? 0u : ui_last_prog;
    if (sec != ui_prog_sec) {
        ui_prog_sec = sec;
        done = 0;
        uint32_t total = ui_total_secs();
        if (total) {
            done = (sec * UI_INNER_W) / total;
            if (done > UI_INNER_W) done = UI_INNER_W;
        }
    }
    if (done != ui_last_prog) {
        ui_last_prog = done;
        /* Clear a band taller than the bar first: the knob overhangs it above
         * and below, so redrawing only the bar would leave the old knob's
         * overhang behind as two floating stubs. */
        fb_rect(UI_MARGIN, UI_PROG_Y - 3u, UI_INNER_W, UI_PROG_H + 6u,
                ui_grad_at(UI_PROG_Y));
        if (done) fb_rect(UI_MARGIN, UI_PROG_Y, done, UI_PROG_H, ui_accent);
        if (UI_INNER_W > done)
            fb_rect(UI_MARGIN + done, UI_PROG_Y, UI_INNER_W - done,
                    UI_PROG_H, UI_TRACK);
        /* Position knob -- also the visual handle the seek controls move. */
        {
            uint32_t kx = UI_MARGIN + done;
            if (kx < UI_MARGIN + 2u) kx = UI_MARGIN + 2u;
            if (kx > UI_MARGIN + UI_INNER_W - 3u) kx = UI_MARGIN + UI_INNER_W - 3u;
            fb_rect(kx - 2u, UI_PROG_Y - 3u, 5u, UI_PROG_H + 6u, UI_WHITE);
        }
    }

    /* Toast: hold, then dissolve into the background.
     *
     * The fade is real, not a trick -- glyphs are drawn fg-on-bg, so stepping
     * the foreground toward the background colour genuinely dissolves the
     * text. Redrawn only when the step changes, so a 10-step fade costs ten
     * repaints of one short line, not one per frame. */
    if (ui_toast_t0) {
        uint16_t tbg2 = ui_grad_at(UI_TOAST_Y);
        uint32_t age  = cycles() - ui_toast_t0;
        uint32_t step = (age <= UI_TOAST_HOLD) ? 0u
                      : ((age - UI_TOAST_HOLD) * UI_TOAST_STEPS) / UI_TOAST_FADE;
        if (step > UI_TOAST_STEPS) step = UI_TOAST_STEPS;

        if (step != ui_toast_step) {
            ui_toast_step = step;
            if (step >= UI_TOAST_STEPS) {
                fb_rect(UI_MARGIN, UI_TOAST_Y, UI_INNER_W, UI_TOAST_H, tbg2);
                ui_toast_t0  = 0;
                ui_toast_end = UI_MARGIN;
            } else {
                fb_set_color(ui_mix(UI_WHITE, tbg2, step, UI_TOAST_STEPS), tbg2);
                uint32_t end = fb_text_clipped(UI_MARGIN, UI_TOAST_Y, ui_toast,
                                               TS_1X, TS_1X, UI_INNER_W);
                /* Erase only the TAIL the previous message left behind.
                 *
                 * CHAR paints its own background, so a message replacing a
                 * LONGER one only repaints the cells it covers and the old
                 * ending stays on screen -- visible when flipping settings
                 * faster than a toast fades. Clearing the whole line instead
                 * would cost a rect on every one of the ten fade steps and
                 * put an erase-then-draw on a single-buffered framebuffer,
                 * which is what makes things flicker when scanout catches the
                 * gap.
                 *
                 * fb_text_clipped returns where it stopped, so the leftover is
                 * exactly [end, previous end). In the common case -- the same
                 * string redrawn a shade dimmer -- end is unchanged and this
                 * costs nothing. */
                if (end < ui_toast_end)
                    fb_rect(end, UI_TOAST_Y, ui_toast_end - end, UI_TOAST_H, tbg2);
                ui_toast_end = end;
            }
        }
    }

    /* Transport state, in the gap to the right of the clock. Both strings are
     * the same length so one overwrites the other cleanly. */
    {
        uint16_t tbg = ui_grad_at((UI_TIME_Y + 10u));
        /* Left end of its own row. The arrows sit at a FIXED x derived from
         * the WIDER of the two words, so switching PLAYING <-> PAUSED cannot
         * shuffle them sideways. */
        const uint32_t ly = UI_TRANSPORT_Y;
        const uint32_t lx = UI_MARGIN;
        const uint32_t lbl_w = fb_text_width("STOPPED", TS_1X);
        const uint32_t ix = lx + lbl_w + 12u;
        const uint32_t iy = ly;

        /* Breathe: a triangle over 64 draws. Paused redraws at ~30 Hz, playing
         * is throttled below, so both land near a 2 s cycle. */
        uint32_t ph  = (++ui_breath) & 63u;
        uint32_t lvl = (ph < 32u) ? ph : (63u - ph);

        if (paused) {
            /* Stopped is a settled state, so it does not breathe -- the pulse
             * says "waiting to resume", which stop is not. It also means the
             * label and icon are drawn once instead of every frame. */
            uint16_t c = stopped ? UI_WHITE : ui_mix(UI_PANEL, UI_WHITE, lvl, 31u);
            if (!stopped || ui_last_pause != 2u) {
                fb_rect(lx, ly, lbl_w + 8u, FB_CELL(TS_1X), tbg);
                fb_set_color(c, tbg);
                fb_text_clipped(lx, ly, stopped ? "STOPPED" : "PAUSED",
                                TS_1X, TS_1X, lbl_w + 8u);
                fb_rect(ix, iy, UI_ARR_SPAN, UI_ICONBOX_H, tbg);
                if (stopped) ui_icon_stop(ix, iy, c);
                else         ui_icon_pause(ix, iy, c);
            }
            ui_last_pause = stopped ? 2u : 0xFFFFFFFFu;
        } else {
            if (ui_last_pause != 0u) {
                ui_last_pause = 0u;
                fb_rect(lx, ly, lbl_w + 8u, FB_CELL(TS_1X), tbg);
                fb_set_color(ui_accent, tbg);
                fb_text_clipped(lx, ly, "PLAYING", TS_1X, TS_1X, lbl_w + 8u);
            }
            /* The arrow breathes too, but this path runs EVERY frame rather
             * than at the paused refresh rate, so it is throttled -- redrawing
             * a dozen rects 38 times a second for an idle animation is exactly
             * the kind of cost that used to disturb the decoder. */
            if ((int32_t)(cycles() - ui_icon_next) >= 0) {
                ui_icon_next = cycles() + CLK_HZ / 30u;

                /* Brightness falls off CONTINUOUSLY with how long ago each
                 * arrow was the lit one, rather than snapping between a few
                 * fixed levels. The peak rotates and each arrow trails a smooth
                 * glow behind it, so the step boundaries stop being visible. */
                uint32_t t = ui_arr_t++ % UI_ARR_TICKS;
                fb_rect(ix, iy, UI_ARR_SPAN, UI_ICONBOX_H, tbg);
                for (uint32_t i = 0; i < UI_ARR_N; i++) {
                    uint32_t dist = (t + UI_ARR_TICKS - i * UI_ARR_STEPS)
                                    % UI_ARR_TICKS;
                    if (dist >= UI_ARR_TAIL) continue;
                    uint32_t l = 31u - (dist * 31u) / UI_ARR_TAIL;
                    ui_icon_arrow(ix + i * (UI_ARR_W + UI_ARR_GAP), iy,
                                  UI_ARR_W, UI_ARR_H,
                                  ui_mix(UI_PANEL, ui_accent, l, 31u));
                }
            }
        }

        /* Repeat / shuffle / position. Static between changes, so it is drawn
         * only when something actually changed rather than every frame -- the
         * same discipline the arrows needed, for the same reason. */
        if (ui_mode_dirty) {
            ui_mode_dirty = 0;

            const uint32_t mx = ix + UI_ARR_SPAN + 14u;
            const uint32_t my = iy + (UI_ICONBOX_H > UI_MODE_H
                                    ? (UI_ICONBOX_H - UI_MODE_H) / 2u : 0u);

            /* Cleared to the WIDEST name, not the current one: CLASSICAL is
             * 101 px and POP is 35, so a narrower clear would leave the tail of
             * the previous preset on screen. */
            fb_rect(mx, iy, (UI_MODE_W + 10u) * 2u + 106u, UI_ICONBOX_H, tbg);
            /* Inactive modes stay visible but recede, so the controls advertise
             * themselves instead of only appearing once found. */
            ui_icon_repeat(mx, my,
                           rep_mode == REP_OFF ? UI_FAINT : ui_accent,
                           rep_mode == REP_ONE);
            ui_icon_shuffle(mx + UI_MODE_W + 10u, my,
                            shuffle_on ? ui_accent : UI_FAINT);

            /* The preset NAME, not the word "EQ" -- it is the same amount of
             * screen and says which one is on rather than merely that the
             * feature exists. Persistent, so a user who walks away and comes
             * back can see the state without pressing anything. Dimmed on FLAT,
             * the same "off but still visible" treatment the repeat and shuffle
             * icons use. */
            fb_set_color(eq_idx ? ui_accent : UI_FAINT, tbg);
            fb_text_clipped(mx + (UI_MODE_W + 10u) * 2u, my - 2u,
                            eq_name[eq_idx], TS_1X, TS_1X, 110u);

            /* "4 / 12", right-aligned so the numbers do not shuffle sideways as
             * the track index gains a digit.
             *
             * Counted over LIVE entries, not lines in the file: an entry that
             * will not open is not a song. Both halves use the same counting or
             * the position could exceed the total. */
            if (pl_count) {
                char pos[16]; char *q = pos;
                q = ui_dec(q, (uint32_t)pl_live_ordinal(pl_pos));
                *q++ = ' '; *q++ = '/'; *q++ = ' ';
                q = ui_dec(q, (uint32_t)pl_live_count());
                *q = 0;
                uint32_t pw = fb_text_width(pos, TS_1X);
                uint32_t px = FB_W - UI_MARGIN - pw;
                fb_rect(px - 4u, ly, pw + 8u, FB_CELL(TS_1X), tbg);
                fb_set_color(UI_DIM, tbg);
                fb_text_clipped(px, ly, pos, TS_1X, TS_1X, pw + 4u);
            }
        }
    }

    /* Only ever appears if the CPU actually blocked on a full draw FIFO. If
     * the audio is clean this stays invisible; if it is not, this says in one
     * number whether drawing is to blame -- which rev 6 could only establish
     * by A/B-ing the entire feature on hardware. */
    /* TEMPORARY diagnostic line, redrawn only when a value changes:
     *   A  tag length (where playback starts)
     *   S  file size APF reports for the slot
     *   T  computed total seconds
     *   F  slot identity from 0190, low byte -- 00 means it did not answer
     * These are exactly the inputs behind "started in the wrong place" and
     * "wrong total", so a bad load says which one is at fault instead of
     * needing another round of inference. Set UI_SHOW_DIAG 1 to show it. */
#if UI_SHOW_DIAG
    uint32_t diag = audio_start ^ slot_size ^ (track_secs << 3) ^ cur_file_id;
    if (diag != ui_last_stall) {
        ui_last_stall = diag;
        char b[64], *q = b;        /* A=tag length  S=file size  T=total secs  F=slot identity (low byte)
         * Exactly the four inputs behind "started in the wrong place" and
         * "wrong total", so a bad load says which one is at fault. */
        *q++ = 'A';
        q = ui_hex2(q, (uint8_t)(audio_start >> 8));
        q = ui_hex2(q, (uint8_t)audio_start);
        *q++ = ' '; *q++ = 'S';
        q = ui_hex2(q, (uint8_t)(slot_size >> 16));
        q = ui_hex2(q, (uint8_t)(slot_size >> 8));
        q = ui_hex2(q, (uint8_t)slot_size);
        *q++ = ' '; *q++ = 'T';
        q = ui_hex2(q, (uint8_t)(track_secs >> 8));
        q = ui_hex2(q, (uint8_t)track_secs);
        *q++ = ' '; *q++ = 'F';
        q = ui_hex2(q, (uint8_t)cur_file_id);
        *q = 0;
        uint16_t dbg = ui_grad_at((FB_H - 24u));
        fb_rect(UI_MARGIN, FB_H - 24u, UI_INNER_W, FB_CELL(TS_1X), dbg);
        fb_set_color(UI_RED, dbg);
        fb_text_clipped(UI_MARGIN, FB_H - 24u, b, TS_1X, TS_1X, UI_INNER_W);
    }
#endif

    /* Speed-branch readout. These five values ARE the suspected fault, not a
     * general-purpose dump:
     *
     *   1.0x/1.2x  which mode -- so a screenshot is unambiguous
     *   T   track_secs. THE discriminator. Non-zero means ui_byte_rate() uses
     *       the exact rate from the Xing header and the meas_rate branch is
     *       skipped entirely. Zero is the path the old hold-to-seek bug lived
     *       in, and "wrong on some files, fine on others" is its signature.
     *   M   meas_rate, the measured bytes/sec. If this DRIFTS at 1.2x while
     *       staying put at 1.0x on the same file, the fault is found.
     *   R   what ui_byte_rate() actually returns -- the number seek and the
     *       elapsed clock consume. Shown separately from M because which of
     *       the three sources won is exactly what is in question.
     *   S   ui_sec, the elapsed clock.
     *   K   file_pos in KB, so the position is visible without eight digits.
     *
     * Redrawn on a change of ui_sec, i.e. about once a second, which is cheap
     * enough not to disturb the very budget being investigated.
     *
     * HOW TO USE IT: play a file that misbehaves, note T. Watch M and R at
     * 1.0x for ten seconds, hold A, watch them again. Compare, do not infer. */
#if UI_SHOW_SPEED_DIAG
    /* ONE row, at y336..351.
     *
     * There were two, and the upper one sat at y318..333 against the toast
     * band at y314..329 -- they repainted over each other every second, which
     * is why a toast only ever showed as a hint. Anything added here must stay
     * below y330.
     *
     * Only the open question is carried: N is playlist notifications the RTL
     * delivered, L is times pl_load() actually ran. The seek and resume
     * investigations are closed, so their fields are gone. */
    if (ui_sec != ui_last_spd) {
        ui_last_spd = ui_sec;
        char b[64], *q = b;
        const char *sp = speed_fast ? "1.2x" : "1.0x";
        while (*sp) *q++ = *sp++;
        *q++ = ' '; *q++ = 'N'; q = ui_dec(q, pl_notify_n);
        *q++ = ' '; *q++ = 'L'; q = ui_dec(q, pl_load_n);
        *q = 0;
        uint16_t sbg = ui_grad_at((FB_H - 24u));
        fb_rect(UI_MARGIN, FB_H - 24u, UI_INNER_W, FB_CELL(TS_1X), sbg);
        fb_set_color(UI_RED, sbg);
        fb_text_clipped(UI_MARGIN, FB_H - 24u, b, TS_1X, TS_1X, UI_INNER_W);
    }
#endif
}


/* cont1_key bit assignments (APF standard layout) */
#define KEY_UP      (1u << 0)
#define KEY_DOWN    (1u << 1)
#define KEY_LEFT    (1u << 2)
#define KEY_RIGHT   (1u << 3)
#define KEY_A       (1u << 4)
#define KEY_B       (1u << 5)
#define KEY_X       (1u << 6)
#define KEY_Y       (1u << 7)   /* was entirely unused before the EQ */
#define KEY_L1      (1u << 8)
#define KEY_R1      (1u << 9)
#define KEY_SELECT  (1u << 14)
#define KEY_START   (1u << 15)
#define IN_MENU     (1u << 16)

#define UNCACHED    0xC0000000u
#define MP3_SLOT_ID 2u
#define FW_SLOT_ID  1u   /* touched only to flush APF's slot cache */

/* MP3 ring buffer: an APF DMA target, so firmware must read it through the
 * UNCACHED alias -- the cached window can return stale lines with no
 * indication. Its address comes from the linker, which reserves the region
 * above the heap, so the heap can never grow into it. */
extern char _ring_start, _ring_size;
#define RING_OFF     ((uint32_t)(uintptr_t)&_ring_start)
#define RING_SIZE    ((uint32_t)(uintptr_t)&_ring_size)
#define REFILL_CHUNK 4096u

static uint8_t * const ring = (uint8_t *)(uintptr_t)(UNCACHED + (uint32_t)(uintptr_t)&_ring_start);

/* Separate DMA landing zone for the ID3 re-probe and the art decoder. It
 * CANNOT share the ring: the ring holds audio the decoder is consuming, so
 * re-reading offset 0 into it would destroy playback. */
extern char _tag_start, _tag_size;
#define TAG_OFF  ((uint32_t)(uintptr_t)&_tag_start)
#define TAG_SIZE ((uint32_t)(uintptr_t)&_tag_size)
static uint8_t * const tagbuf = (uint8_t *)(uintptr_t)(UNCACHED + (uint32_t)(uintptr_t)&_tag_start);

static uint32_t st0;
static short pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

/* ------------------------------------------------------------- controls --
 * A / Start : play-pause          Left / Right      : tap  = seek -/+ ~5 s
 * Up / Down : volume                                  hold = previous/next
 * B         : restart this track from 0:00
 * Select    : show/hide art       Select + L        : cycle repeat off/all/one
 * L / R     : accent colour       Select + R        : shuffle on/off
 *
 * Left/Right and Select both do one thing on a tap and another when held or
 * combined. Both resolve on RELEASE, so the tap action cannot fire and then be
 * followed by the hold action for the same press. A tap is tens of
 * milliseconds, so deferring it that long is not perceptible.
 */
static uint32_t skip_req;                /* +1 next, -1 previous (as unsigned) */
static uint32_t pl_reload_pending;       /* user picked a different .m3u       */
static uint32_t pl_dump_req;             /* Select+B: show the 0190 struct     */
static uint32_t dt_dump_req;             /* Select+A: show the boot datatable  */
static uint32_t ui_dump_mode;            /* dump screen is up; drawing paused  */

/* Arm the idle timer. Called on every button press and whenever the timeout
 * setting changes. */
/* Counts SECONDS, deliberately. A cycles() deadline cannot express this: the
 * counter is 32-bit at 60 MHz, so it wraps every 71.6 s and the usual
 * (int32_t)(cycles() - deadline) >= 0 idiom only spans 35.8 s. One minute is
 * already past that and two minutes overflows the multiply outright, so the
 * first version could not have worked at any setting. Every other timeout in
 * this core is sub-second, which is why nothing had hit the ceiling before.
 *
 * A one-second tick is comfortably inside the safe range, and seconds are what
 * the setting is measured in anyway. */
static void ui_blank_touch(void)
{
    blank_sec  = 0;
    blank_tick = cycles() + CLK_HZ;
}

static void ui_blank_enter(void)
{
    if (screen_blank) return;
    screen_blank = 1u;
    fb_rect(0, 0, FB_W, FB_H, 0x0000u);
}

static void ui_blank_wake(void)
{
    if (!screen_blank) return;
    screen_blank = 0;
    ui_draw_chrome();          /* everything suppressed while blank, redrawn */
}

/* One call per main-loop pass. Re-arms from NOW rather than advancing by a
 * fixed step, so a long stall (a track load) cannot leave a backlog of ticks to
 * catch up on. Drift does not matter for a screen blanker. */
/* Record where we are, once a second. Two MMIO writes and no SD access, so the
 * cost is nothing -- the settings register file is read back by APF each frame
 * and APF does the storing, into its own file.
 *
 * Only while PLAYING. Saving while paused or idle would overwrite a good point
 * with 0:00 on the way out of a track, which is precisely the moment the value
 * matters. The FILE index is saved, not pl_pos: pl_pos indexes the play order,
 * which a reshuffle rewrites, and the same song would then come back as a
 * different entry. */
static void resume_pump(void)
{
    static uint32_t last_sec = 0xFFFFFFFFu;
    /* Nothing may mark the settings dirty until settings_load() has adopted
     * APF's stored values. settings_store() publishes ALL eight words, so a
     * dirty flag raised first would write firmware DEFAULTS over the user's
     * saved volume, meter and the rest -- which is exactly what reset two of
     * them on the last build. */
    if (!settings_adopted) return;
    /* A pending resume must not be overwritten by the position it is about to
     * replace. Without this the seconds saved while waiting -- 0, 1, 2 -- land
     * on top of the value the seek is holding, and the point destroys itself
     * in the gap between arming and firing. (Y was observed incrementing on
     * hardware, which is this saver doing exactly that.) */
    if (resume_seek_req) return;
    /* PLAYLIST PLAYBACK ONLY. A file picked with Load MP3 does not record a
     * position, by decision: making resume work for arbitrary picked files
     * needed a cross-boot file identity that does not exist here, and every
     * attempt at one produced a different bug.
     *
     * It also buys something. A standalone track can no longer overwrite the
     * saved point, so playing a song in the middle of an audiobook does not
     * cost you your place in it. An audiobook that wants resume goes in a
     * playlist -- a one-line .m3u is enough. */
    if (!track_from_pl) return;
    if (!resume_on || idle || (paused & 1u) || !track_file[0]) return;
    if (ui_sec == last_sec) return;
    last_sec = ui_sec;

    uint16_t f = (track_from_pl && pl_count && pl_pos < pl_count)
               ? pl_order[pl_pos] : 0u;
    uint32_t w = RS_PACK(f, ui_sec, track_from_pl);
    if (w != resume_word) { resume_word = w; resume_saves++; settings_mark_dirty(); }
}

static void ui_blank_pump(void)
{
    if (screen_blank || !blank_min) return;
    if ((int32_t)(cycles() - blank_tick) < 0) return;
    blank_tick = cycles() + CLK_HZ;
    if (blank_sec < 0xFFFFu) blank_sec++;
    if (blank_sec >= blank_min * 60u) ui_blank_enter();
}

/* Black the whole frame. Only the framebuffer -- there is no way to switch the
 * panel itself off from a core, so "blank" means every pixel black. The
 * backlight stays on regardless; see the note on blank_min. */
static void poll_input(void)
{
    static uint32_t prev;
    static uint32_t lr_t0[2];            /* when Left/Right went down          */
    static uint8_t  lr_fired[2];         /* the hold already skipped           */
    static uint8_t  sel_used;            /* Select was used as a modifier      */
    uint32_t in   = REG(R_INPUT);
    uint32_t keys = in & 0xFFFFu;
    uint32_t edge = keys & ~prev;        /* rising edges only  */
    uint32_t fall = prev & ~keys;        /* falling edges      */
    prev = keys;

    /* Any press is activity. If the screen is blank the press ONLY wakes it and
     * is then swallowed -- reaching for a sleeping player to see what is on
     * should not pause it or skip the track. */
    if (edge || fall) ui_blank_touch();
    if (screen_blank) {
        if (edge) ui_blank_wake();
        /* Swallow the waking press -- but do NOT return. The tail of this
         * function handles the OS menu and the reload notification, and
         * returning early meant "Load MP3" was ignored while the screen was
         * blank. Clearing keys too, so a button already held cannot trigger a
         * hold action either; `prev` was assigned above and still carries the
         * real state, so the next pass sees correct edges. */
        edge = 0; fall = 0; keys = 0;
    }

    /* A plays and pauses. Start only ever STOPS -- pressing it again does
     * nothing, which is what separates it from pause: stop is a state you
     * leave with play, not a toggle. Stopping also returns to 0:00. */
    /* A: TAP plays/pauses, HOLD toggles 1.2x speed.
     *
     * Resolves on RELEASE, the same discipline Left/Right and Select already
     * use. Firing the tap action on the press instead would mean a long press
     * pauses AND changes speed -- it is on the way into every hold. Same
     * PL_HOLD_MS the Left/Right scrub uses, rather than a second threshold to
     * learn. */
    {
        static uint32_t a_t0;
        static uint8_t  a_fired;      /* the hold action already ran this press */
        const uint32_t  a_hold_cy = CLK_HZ / 1000u * PL_HOLD_MS;

        if (edge & KEY_A) { a_t0 = cycles(); a_fired = 0; }

        if ((keys & KEY_A) && !a_fired &&
            (int32_t)(cycles() - a_t0) >= (int32_t)a_hold_cy) {
            a_fired = 1;
            speed_fast ^= 1u;
            pcm_rate_apply(track_hz);
            /* Name the speed. An unlabelled 1.2x just sounds like a bad rip,
             * and the only other clue is the elapsed clock running fast. */
            ui_toast_msg(speed_fast ? "SPEED 1.2x" : "SPEED NORMAL");
        }

        if ((fall & KEY_A) && !a_fired) {
            /* Select+A shows the boot datatable snapshot, mirroring Select+B
             * for the 0190 struct. Plain A still plays/pauses. */
#if DEBUG_DIAG
            if (keys & KEY_SELECT) { sel_used = 1; dt_dump_req = 1u; }
            else
#endif
            {
                paused ^= 1u;
                if (!(paused & 1u)) stopped = 0;  /* playing is never "stopped" */
            }
        }
    }
    if (edge & KEY_X) {
        /* Forward only. A reverse on Select+X existed and was dropped: nine
         * modes wrap in nine taps, and every Select combo the user has to
         * remember costs more than it saves. */
        viz_mode = (uint8_t)((viz_mode + 1u) % VIZ_COUNT);
        ui_wave_clear();                 /* modes do not share a screen layout */
        ui_wave_force = 1u;
        for (uint32_t i = 0; i < UI_WAVE_N; i++) {
            wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
        }
        if (art_ready && art_shown) ui_art_draw();
        ui_toast_msg(viz_mode == VIZ_BARS   ? "METER: BARS"
                   : viz_mode == VIZ_WATER  ? "METER: WATERFALL"
                   : viz_mode == VIZ_LEVELS ? "METER: L/R LEVELS"
                   : viz_mode == VIZ_SCOPE  ? "METER: PHASE SCOPE"
                   : viz_mode == VIZ_WAVE   ? "METER: OSCILLOSCOPE"
                   : viz_mode == VIZ_VU     ? "METER: VU"
                   : viz_mode == VIZ_SCROLL ? "METER: WAVEFORM"
                   : viz_mode == VIZ_MIRROR ? "METER: MIRRORED BARS"
                                            : "METER: PEAK DOTS");
        settings_mark_dirty();
    }
    if (edge & KEY_Y) {
        /* Forward only, matching X. Y was completely unused before the EQ. */
        eq_idx = (uint8_t)((eq_idx + 1u) % EQ_COUNT);
        eq_apply = 1u;
    }
    if (edge & KEY_START) {
#if DEBUG_DIAG
        /* Seek state, live. Kept after the hold-to-seek fault: that took five
         * attempts and four of them were guesses, so the next question about
         * this subsystem should start from numbers.
         *
         * COMBO: hold Select AND L, then press START. Not "Select+L", which is
         * the repeat toggle -- the first write-up of this said that and the
         * readout duly never appeared.
         *
         *   P = file_pos/1k   L = computed limit/1k   Z = slot_size/1k
         *   S = ui_sec        R = ui_byte_rate */
        if ((keys & KEY_SELECT) && (keys & KEY_L1)) {
            sel_used = 1;
            char b[24]; uint32_t i = 0;
            uint32_t rate = ui_seek_rate();
            uint32_t lim  = (slot_size > audio_start && rate)
                          ? slot_size - 3u * rate : 0u;
            const uint32_t v[5] = { file_pos >> 10, lim >> 10, slot_size >> 10,
                                    ui_sec, ui_byte_rate() };
            const char *lbl = "PLZSR";
            for (uint32_t k = 0; k < 5u && i + 7u < sizeof(b); k++) {
                b[i++] = lbl[k];
                uint32_t n = v[k], div = 10000u; int lead = 0;
                while (div) {
                    uint32_t d = n / div % 10u;
                    if (d || lead || div == 1u) { b[i++] = (char)('0' + d); lead = 1; }
                    div /= 10u;
                }
                if (k < 4u) b[i++] = ' ';
            }
            b[i] = 0;
            ui_toast_set(b, 0xFFFFFFFFu, 0);
            return;
        }
        if (keys & KEY_SELECT) {
            /* Load-phase breakdown for the track just loaded, in ms: Head read,
             * Size probe, Art decode, Prefill, Total. Measured all along but
             * never surfaced, so every question about a slow load used to be
             * answered by estimating. */
            sel_used = 1;
            char b[24];
            uint32_t i = 0;
            const char *lbl = "HSAPT";
            const uint16_t v[5] = { ld_head, ld_size, ld_art, ld_pre, ld_total };
            for (uint32_t k = 0; k < 5u && i + 6u < sizeof(b); k++) {
                b[i++] = lbl[k];
                uint16_t n = v[k];
                if (n >= 1000u) { b[i++] = (char)('0' + n / 1000u % 10u); }
                if (n >= 100u)  { b[i++] = (char)('0' + n / 100u  % 10u); }
                if (n >= 10u)   { b[i++] = (char)('0' + n / 10u   % 10u); }
                b[i++] = (char)('0' + n % 10u);
                if (k < 4u) b[i++] = ' ';
            }
            b[i] = 0;
            ui_toast_set(b, 0xFFFFFFFFu, 0);
        } else
#endif
        if (!stopped) {
            stopped = 1u; paused |= 1u; stop_req = 1u;
        }
    }
    if (edge & KEY_B) {
#if DEBUG_DIAG
        if (keys & KEY_SELECT) { sel_used = 1; pl_dump_req = 1u; }
        else
#endif
        stop_req = 1u;              /* restart = reposition, NOT a cold reload */
    }

    /* ---- Left/Right: tap changes track, hold seeks ----
     * Skipping is the common action, so it gets the cheap gesture. Holding
     * then scrubs: the seek REPEATS while the button is down, which is what
     * makes holding useful rather than just a slower single jump. */
    {
        static uint8_t lr_reps[2];        /* consecutive scrub repeats, per side */
        const uint32_t kmask[2] = { KEY_LEFT, KEY_RIGHT };
        const uint32_t hold_cy  = CLK_HZ / 1000u * PL_HOLD_MS;
        const uint32_t rep_cy   = CLK_HZ / 1000u * 250u;   /* scrub rate */
        for (int i = 0; i < 2; i++) {
            if (edge & kmask[i]) { lr_t0[i] = cycles(); lr_fired[i] = 0; lr_reps[i] = 0; }

            /* Select + Left/Right: one second a press, for landing on a spot
             * rather than sweeping past it. The shoulder buttons already carry
             * Select+L and Select+R, so the D-pad pair was free. */
            if ((edge & kmask[i]) && (keys & KEY_SELECT)) {
                sel_used    = 1;
                lr_fired[i] = 1;              /* release must not skip track */
                seek_secs   = 1u;
                seek_req    = i ? 1u : (uint32_t)-1;
                ui_toast_msg(i ? "SEEK +1s" : "SEEK -1s");
                continue;
            }
            if (keys & KEY_SELECT) continue;  /* no scrub while modifying */

            if ((keys & kmask[i]) &&
                (int32_t)(cycles() - lr_t0[i]) >= (int32_t)hold_cy) {
                lr_fired[i] = 1;                  /* release must not skip */
                /* Accelerate: a fixed step means crossing a long track is a
                 * lot of repeats, and a big step means you cannot land near
                 * anything. Start small and grow the longer it is held. */
                if (lr_reps[i] < 255u) lr_reps[i]++;
                seek_secs = (lr_reps[i] > 8u) ? 30u : (lr_reps[i] > 3u) ? 10u : 5u;
                seek_req  = i ? 1u : (uint32_t)-1;
                ui_toast_set(i ? "SEEK +" : "SEEK -", seek_secs, "s");
                lr_t0[i] = cycles() + rep_cy - hold_cy;   /* next repeat */
            }

            if (fall & kmask[i]) {
                if (!lr_fired[i]) {               /* a tap: change track */
                    if (pl_count) skip_req = i ? 1u : (uint32_t)-1;
                    else          ui_toast_msg("NO PLAYLIST");
                }
                lr_fired[i] = 0;
            }
        }
    }

    /* Up/Down are now guarded by Select, which they were not before: plain
     * Select+Up used to change the volume AND toggle the art panel on release,
     * because nothing claimed the combo. Select+Down needs it properly. */
    if (!(keys & KEY_SELECT)) {
        if (edge & KEY_UP)   { volume = (volume + VOL_STEP > VOL_MAX)
                                      ? VOL_MAX : volume + VOL_STEP;
                               vol_apply(); ui_toast_set("VOLUME", volume, "%");
                               settings_mark_dirty(); }
        if (edge & KEY_DOWN) { volume = (volume < VOL_STEP) ? 0u : volume - VOL_STEP;
                               vol_apply(); ui_toast_set("VOLUME", volume, "%");
                               settings_mark_dirty(); }
    }

    /* Select+Down cycles the blank timeout. It is the ONLY way to reach this
     * now -- the Core Settings entry was given up so resume could have the
     * slot -- so the cycle has to cover the useful values, not just on/off.
     * Sits beside Select+L (repeat) and Select+R (shuffle), which is where a
     * user already looks for settings-ish combos. Not persisted: it is back to
     * OFF every launch, which is the honest cost of the trade. */
    else if (edge & KEY_DOWN) {
        static const uint8_t bl[] = { 0u, 1u, 5u, 10u, 30u };
        uint32_t i = 0;
        while (i < sizeof(bl) / sizeof(bl[0]) && bl[i] != blank_min) i++;
        i = (i + 1u) % (sizeof(bl) / sizeof(bl[0]));
        blank_min = bl[i];
        sel_used  = 1;
        ui_blank_touch();               /* restart the countdown from now */
        if (blank_min) ui_toast_set("SCREEN BLANK", blank_min, " MIN");
        else           ui_toast_msg("SCREEN BLANK OFF");
    }

    /* ---- Select as a modifier for L/R ---- */
    if (edge & KEY_SELECT) sel_used = 0;

    if (keys & KEY_SELECT) {
        if (edge & KEY_L1) {
            sel_used = 1;
            rep_mode = (uint8_t)((rep_mode + 1u) % 3u);
            ui_toast_msg(rep_mode == REP_OFF ? "REPEAT OFF"
                       : rep_mode == REP_ALL ? "REPEAT ALL" : "REPEAT ONE");
            ui_mode_dirty = 1u;
            settings_mark_dirty();
        }
        if (edge & KEY_R1) {
            sel_used   = 1;
            shuffle_on = (uint8_t)!shuffle_on;
            if (shuffle_on) pl_rng = cycles() | 1u;
            if (pl_count) {
                uint16_t cur = pl_order[pl_pos];
                pl_reorder();
                pl_resync(cur);          /* keep playing what is playing */
            }
            ui_toast_msg(shuffle_on ? "SHUFFLE ON" : "SHUFFLE OFF");
            ui_mode_dirty = 1u;
            settings_mark_dirty();
        }
    } else {
        /* Apply the colour HERE, not in ui_draw_dynamic(). Deferring it meant a
         * track change could run load_track() -> ui_draw_chrome() first and
         * repaint everything in the PREVIOUS accent, so the choice appeared to
         * be forgotten. ui_accent_changed still drives the invalidation work. */
        if (edge & KEY_R1) { ui_pal_idx = (ui_pal_idx + 1u) % UI_PALETTE_N;
                             ui_accent = ui_palette[ui_pal_idx];
                             ui_grad_set(ui_accent);
                             ui_accent_changed = 1u;
                             ui_toast_set("COLOR: ", 0xFFFFFFFFu,
                                          ui_palette_name[ui_pal_idx]);
                             settings_mark_dirty(); }
        if (edge & KEY_L1) { ui_pal_idx = (ui_pal_idx + UI_PALETTE_N - 1u) % UI_PALETTE_N;
                             ui_accent = ui_palette[ui_pal_idx];
                             ui_grad_set(ui_accent);
                             ui_accent_changed = 1u;
                             ui_toast_set("COLOR: ", 0xFFFFFFFFu,
                                          ui_palette_name[ui_pal_idx]);
                             settings_mark_dirty(); }
    }

    /* Only a Select that was NOT used as a modifier toggles the art panel. */
    if ((fall & KEY_SELECT) && !sel_used) art_toggle = 1u;

    /* Pause while the OS menu ("Load MP3" etc) is open, without clobbering the
     * user's own A/Start pause -- bit 1 is the menu's, bit 0 is theirs. */
    /* Opening the OS menu is the strongest signal that the core is about to
     * be left, so a pending settings write goes out now rather than waiting
     * out its quiet window that may never elapse. */
    if (in & IN_MENU) { if (!(paused & 2u)) set_flush_now = 1u; paused |= 2u; }
    else paused &= ~2u;

    /* Only SET the flag here. This runs from inside the sample-push wait,
     * which is where the CPU spends most of its time when keeping up, so a
     * reload is noticed immediately instead of after the current frame. */
    if (REG(R_RELOAD) & RL_PENDING) reload_pending = 1u;
    {
        uint32_t rl = REG(R_RELOAD);
        /* Count the EDGE. The bit is sticky until acked and poll_input runs
         * every loop iteration, so counting the level would tick thousands of
         * times per pick and say nothing. */
        if ((rl & RL_PL_RELOAD) && !pl_reload_pending) pl_notify_n++;
        if (rl & RL_PL_RELOAD) pl_reload_pending = 1u;
        pl_reload_seen |= rl;              /* sticky: every bit ever observed */
    }
}

/* Drops every sample queued in the hardware FIFO. Required on any
 * discontinuity (track change, seek, restart) -- without it the OLD position's
 * queued ~43 ms keeps draining while the new position spins up. */
static inline void pcm_flush(void)
{
    REG(R_PCM_ST) = 1u;
    fade_left    = FADE_SAMPLES;  /* every flush is a discontinuity */
    under_shadow = 0;             /* flush clears the sticky underrun flag */
}

static uint32_t rd_seq0, rd_deadline, rd_len;
static int      rd_pending, rd_ok;

static uint8_t  eof_hit;  /* a refill ran off the end: the file's true extent */
static uint8_t  sw_prev_head[16];   /* head of the song being left */
static uint8_t  sw_have_prev;
static uint32_t eof_at;   /* offset the failures are accumulating at        */
static uint32_t eof_fails;

/* Periodic ID3 re-probe: a light backstop behind the 0190 identity gate. */

static int  target_read_poll(void);
static void refill_drain(void);

static void target_read_start_slot(uint32_t slot, uint32_t off,
                                   uint32_t dst_off, uint32_t len)
{
    refill_drain();                     /* exactly one command in flight, ever */
    rd_seq0 = (REG(R_TGT_GO) >> 8) & 0xFFu;

    REG(R_TGT_ID)  = slot;
    REG(R_TGT_OFF) = off;
    REG(R_TGT_ADR) = dst_off;
    REG(R_TGT_LEN) = len;
    REG(R_TGT_GO)  = TGT_READ;

    rd_len      = len;
    rd_deadline = cycles() + CLK_HZ * 5u;
    rd_pending  = 1;
}

static void target_read_start(uint32_t off, uint32_t dst_off, uint32_t len)
{
    target_read_start_slot(MP3_SLOT_ID, off, dst_off, len);
}

/* Waits out an in-flight read WITHOUT committing it: callers that drain are
 * about to reset or move the ring anyway, and the bytes land above ring_fill
 * where nothing will read them. */
static void refill_drain(void)
{
    while (rd_pending) if (target_read_poll()) rd_pending = 0;
}

/* Completion is detected by watching the SEQUENCE COUNTER change, not the
 * `done` bit. `done` stays asserted from the previous command until the next
 * one is picked up, so polling it right after issuing GO can observe the
 * PREVIOUS command's completion and return with the transfer still in flight.
 * Only the first command after reset escapes that -- which is exactly why the
 * boot read worked and every later one did not. See tgt_cmd.v. */
static int target_read_poll(void)
{
    uint32_t s = REG(R_TGT_GO);
    if (((s >> 8) & 0xFFu) != rd_seq0) { rd_ok = (((s >> 2) & 7u) == 0u); return 1; }
    if ((int32_t)(cycles() - rd_deadline) >= 0) { rd_ok = 0; return 1; }
    return 0;
}

/* Blocking form -- only for the head read, prefill and the probes, where there
 * is no audio to starve. The steady-state path must NOT use this. */
static int target_read_slot(uint32_t slot, uint32_t off, uint32_t dst_off, uint32_t len)
{
    target_read_start_slot(slot, off, dst_off, len);
    /* The boot indicator is animated from HERE -- this spin is where the time
     * actually goes during a playlist read. ui_boot_tick() is a single compare
     * and return unless a note is armed, which it only is around pl_load(), so
     * every other caller of this function is unaffected. */
    while (!target_read_poll()) { ui_boot_tick(); ui_wave_anim_tick(); }
    rd_pending = 0;
    return rd_ok;
}

static int target_read(uint32_t off, uint32_t dst_off, uint32_t len)
{
    return target_read_slot(MP3_SLOT_ID, off, dst_off, len);
}

/* core_bridge_cmd's datatable, reachable by both the CPU and APF. Used for the
 * 0190 response struct. */
static inline uint32_t dt_read(uint32_t word)
{
    REG(R_DT_ADDR) = word;
    return REG(R_DT_DATA);
}

static uint32_t probe_clamp_ref;    /* what a past-EOF read returns, if any */
static int      probe_clamps;

/* Is `off` inside the file? Poison the landing zone and see whether anything
 * lands. Deliberately does not trust the command's status: a short read at the
 * tail can still report success. */
static int probe_readable(uint32_t off)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + TAG_OFF);
    *w = 0xA5A5A5A5u;
    /* 512 bytes, not 4: a four-byte transfer is the sort of request a host may
     * refuse or round, and if every probe fails the search reports "no size". */
    if (!target_read_slot(MP3_SLOT_ID, off, TAG_OFF, 512u)) return 0;
    if (*w == 0xA5A5A5A5u) return 0;              /* nothing landed: past EOF */
    /* Some hosts CLAMP instead of failing -- a read past the end quietly
     * returns the tail, so the sentinel always says "readable". When that is
     * detected, treat "identical to a hopeless offset" as past-EOF instead. */
    if (probe_clamps && *w == probe_clamp_ref) return 0;
    return 1;
}


/* Measure the file. APF only reports a size with a RELOAD notification, so a
 * track loaded at boot has none -- which is why the progress bar and the
 * end-of-track check need this. Binary-searching the last readable offset
 * needs no notification, no struct layout and no status semantics. */
static uint32_t probe_file_size(void)
{
    probe_clamps = 0;
    {
        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + TAG_OFF);
        *w = 0xA5A5A5A5u;
        if (target_read_slot(MP3_SLOT_ID, 60u << 20, TAG_OFF, 512u) &&
            *w != 0xA5A5A5A5u) {
            probe_clamp_ref = *w;
            probe_clamps    = 1;
        }
    }

    if (!probe_readable(0)) return 0;

    uint32_t lo = 0, hi = 1u << 22;                    /* start at 4 MB */
    while (hi < (1u << 26) && probe_readable(hi)) { lo = hi; hi <<= 1; }
    if (hi >= (1u << 26) && probe_readable(hi)) return 0;   /* runaway */

    while (hi - lo > 4096u) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (probe_readable(mid)) lo = mid; else hi = mid;
    }
    return lo + 4096u;
}

static void slot_filename(char *out, uint32_t out_size);

/* Ask APF which file is CURRENTLY in the slot (0190) and reduce its response
 * to one number. This is the authoritative answer to "has the slot changed
 * yet?" -- every content-based guess at that was defeated by a read that was
 * wrong but stable. HASHES the struct rather than parsing it, so nothing here
 * depends on a field offset that would otherwise be guesswork. */
static uint32_t slot_file_id(void)
{
    refill_drain();                     /* one command in flight, ever */
    uint32_t seq0 = (REG(R_TGT_GO) >> 8) & 0xFFu;
    REG(R_TGT_ID) = MP3_SLOT_ID;
    REG(R_TGT_GO) = TGT_GETFILE;

    uint32_t deadline = cycles() + CLK_HZ;
    for (;;) {
        uint32_t st = REG(R_TGT_GO);
        if (((st >> 8) & 0xFFu) != seq0) break;
        if ((int32_t)(cycles() - deadline) >= 0) return 0;   /* 0 = unknown */
    }

    slot_filename(track_file, sizeof(track_file));

    uint32_t h = 2166136261u;                   /* FNV-1a over the struct */
    for (uint32_t w = 0; w < 64u; w++) {
        uint32_t v = dt_read(w);
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (b * 8)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;                          /* keep 0 for "unknown" */
}

/* Pull the filename out of the 0190 response WITHOUT knowing its layout: the
 * longest run of printable ASCII in the struct IS the name. */
static void slot_filename(char *out, uint32_t out_size)
{
    uint8_t raw[256];
    for (uint32_t w = 0; w < 64u; w++) {
        uint32_t v = dt_read(w);
        /* BIG-endian unpack: the bridge byte-swaps, and a little-endian
         * unpack scrambles the name into 4-byte groups. */
        raw[w * 4 + 0] = (uint8_t)(v >> 24);
        raw[w * 4 + 1] = (uint8_t)(v >> 16);
        raw[w * 4 + 2] = (uint8_t)(v >> 8);
        raw[w * 4 + 3] = (uint8_t)v;
    }

    uint32_t best = 0, best_len = 0, i = 0;
    while (i < sizeof(raw)) {
        uint32_t start = i;
        while (i < sizeof(raw) && raw[i] >= 0x20u && raw[i] < 0x7Fu) i++;
        if (i - start > best_len) { best_len = i - start; best = start; }
        i++;
    }

    uint32_t n = best_len;
    if (n > out_size - 1u) n = out_size - 1u;
    for (uint32_t k = 0; k < n; k++) out[k] = (char)raw[best + k];
    out[n] = 0;
}

/* Force APF to forget what it knows about the MP3 slot. Its fragment cache is
 * dropped whenever a DIFFERENT slot is accessed, and after a reload those
 * cached fragments still describe the OLD file. LOAD TIME ONLY -- doing this
 * while streaming makes every refill re-walk the cluster chain. */
static void target_flush_slot_cache(void)
{
    target_read_slot(FW_SLOT_ID, 0, TAG_OFF, 512);
}

#include "art.inc"
#include "playlist.inc"
#include "settings.inc"

/* Slide unconsumed bytes down and pull in ONE chunk. Compaction keeps Helix's
 * input pointer arithmetic simple -- it wants a flat span, not a wrap. */
static int refill_one(void)
{
    if (ring_fill + REFILL_CHUNK > RING_SIZE) {
        if (ring_rd == 0) return 1;                 /* genuinely full */

        uint32_t align = ring_rd & ~3u;             /* word-aligned source */
        uint32_t keep  = ring_fill - align;
        uint32_t off   = ring_rd - align;

        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + RING_OFF);
        uint32_t words = (keep + 3u) >> 2;
        uint32_t src   = align >> 2;
        for (uint32_t i = 0; i < words; i++) w[i] = w[src + i];

        ring_fill = keep;
        ring_rd   = off;
    }

    if (!target_read(file_pos, RING_OFF + ring_fill, REFILL_CHUNK)) return 0;
    file_pos  += REFILL_CHUNK;
    ring_fill += REFILL_CHUNK;
    return 1;
}


/* Issue a read, then go back to decoding and collect it later.
 *
 * This is NOT an optimisation. Until the handshake was fixed the target
 * command returned early, so a refill cost almost nothing and the DMA landed
 * in the background -- accidental async I/O, and the only reason the decoder
 * kept up. Making the handshake correct made refills genuinely blocking, and a
 * 4 KB read stalling the decoder is far more than the slack left after decode.
 *
 * Safe because a read only ever writes ABOVE ring_fill while the decoder only
 * reads BELOW it. Compaction, which does move the decodable region, is gated
 * on no read being in flight. */
static void refill_pump(void)
{
    if (rd_pending) {
        if (!target_read_poll()) return;
        rd_pending = 0;
        if (!rd_ok) {
            /* A RING read past the end of the file FAILS on this host --
             * probe_file_size() was built on exactly that behaviour. So a
             * failed refill IS the end of the file announcing itself, and this
             * is where the size of a headerless file gets discovered: at the
             * end, where the stream reports it for free, instead of by twenty
             * BLOCKING reads at the start of playback -- which were the tic on
             * the one track with no Xing header. These reads are async; the
             * decoder never waits on them.
             *
             * Halve toward the true boundary so the tail is not lost: a 4 KB
             * read straddling EOF fails outright rather than shortening. */
            if (rd_len > 512u) {
                target_read_start(file_pos, RING_OFF + ring_fill, rd_len / 2u);
                return;
            }
            /* CONFIRM before believing it. A read failing once does not mean
             * end-of-file: right after a 0192 the slot is still settling and a
             * refill can fail transiently. Treating that as EOF set slot_size
             * to the current position and the main loop restarted the track --
             * exactly the "plays half a second, jumps back to the beginning"
             * report, and impossible on stop/restart because no file changes
             * there. Three consecutive failures at the SAME offset, which a
             * real end always produces and a settling slot does not. */
            if (eof_at != file_pos) { eof_at = file_pos; eof_fails = 0; }
            if (++eof_fails < 3u) {
                target_read_start(file_pos, RING_OFF + ring_fill, 512u);
                return;
            }
            eof_hit = 1u;
            if (!slot_size || slot_size > file_pos) slot_size = file_pos;
            return;
        }
        file_pos += rd_len; ring_fill += rd_len;
        return;
    }

    /* No NEW I/O during a track transition: the slot may already be serving
     * the incoming file, and a refill at the outgoing position would hand the
     * decoder bytes from the middle of the wrong track. In-flight reads are
     * still collected above. */
    if (reload_armed || reload_pending) return;

    /* Ring first, always -- audio starvation beats a late tag update. */
    if (ring_fill - ring_rd >= RING_SIZE / 2u) {
        return;
    }
    if (eof_hit) return;                    /* nothing past the end to fetch */

    if (ring_fill + REFILL_CHUNK > RING_SIZE) {
        if (ring_rd == 0) return;                           /* genuinely full */

        uint32_t align = ring_rd & ~3u;
        uint32_t keep  = ring_fill - align;
        uint32_t off   = ring_rd - align;

        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + RING_OFF);
        uint32_t words = (keep + 3u) >> 2;
        uint32_t src   = align >> 2;
        for (uint32_t i = 0; i < words; i++) w[i] = w[src + i];

        ring_fill = keep;
        ring_rd   = off;
    }

    target_read_start(file_pos, RING_OFF + ring_fill, REFILL_CHUNK);
}

/* Load just enough to start decoding, then let the asynchronous refill top up
 * during playback. Filling the whole ring here was eight BLOCKING reads with
 * the PCM FIFO just flushed -- the FIFO covers only ~46 ms, so anything longer
 * was an audible gap on every track change. */
#define PREFILL_CHUNKS 3u

static int prefill(void)
{
    eof_hit = 0; eof_fails = 0; eof_at = 0xFFFFFFFFu;   /* every reposition passes here */
    uint32_t want = PREFILL_CHUNKS * REFILL_CHUNK;
    if (want > RING_SIZE) want = RING_SIZE;
    while (ring_fill < want && ring_fill + REFILL_CHUNK <= RING_SIZE)
        if (!refill_one()) return 0;
    return 1;
}

/* ID3v2 size is "syncsafe": 7 significant bits per byte. Total tag size is
 * (10-byte header + size) PLUS a 10-byte footer when that flag is set. Takes
 * the buffer explicitly so the same parsing serves the ring at load time and
 * the probe buffer during playback. */
static uint32_t id3_len(const uint8_t *b)
{
    if (b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    uint32_t footer = (b[5] & 0x10u) ? 10u : 0u;
    return 10u + footer +
           (((uint32_t)(b[6] & 0x7Fu) << 21) |
            ((uint32_t)(b[7] & 0x7Fu) << 14) |
            ((uint32_t)(b[8] & 0x7Fu) << 7)  |
            ((uint32_t)(b[9] & 0x7Fu)));
}

/* Extracts a text frame (TIT2, TPE2, TALB, ...) from a tag already in memory.
 * Scoped deliberately: only what the caller loaded, and only ISO-8859-1/UTF-8
 * -- UTF-16 is reported as its own case rather than silently garbled. Handles
 * v2.3 (plain big-endian size) and v2.4 (syncsafe). */
/* Decode one text frame BODY -- the encoding byte and the bytes after it -- into
 * out. Shared by the in-memory parser and the card walk so the two cannot drift
 * on what a given encoding means.
 *
 * UTF-16 (encodings 1 and 2) used to be refused outright, surfacing as its own
 * error text. That is honest but it is still a track with no title, and UTF-16
 * is what several taggers emit by default -- one of the ten tracks on the test
 * card has every text frame in it. The font atlas is ASCII 0x20..0x7E, so
 * anything above Latin-1 could not be drawn regardless; a code unit that does
 * not fit becomes '?', which loses an accent but keeps the title. */
static int id3_text_body(const uint8_t *b, uint32_t fsize, char *out,
                         uint32_t out_size)
{
    if (fsize < 2u) return ID3_NO_FRAME;
    uint8_t  enc = b[0];
    uint32_t n   = fsize - 1u;
    uint32_t i   = 0;

    if (enc == 1u || enc == 2u) {
        uint32_t s  = 1u;
        int      be = (enc == 2u);              /* 2 is UTF-16BE, no BOM */
        if (enc == 1u && n >= 2u) {
            if (b[1] == 0xFFu && b[2] == 0xFEu)      { be = 0; s = 3u; }
            else if (b[1] == 0xFEu && b[2] == 0xFFu) { be = 1; s = 3u; }
        }
        while (s + 1u < fsize && i + 1u < out_size) {
            uint32_t u = be ? (((uint32_t)b[s] << 8) | b[s + 1u])
                            : (((uint32_t)b[s + 1u] << 8) | b[s]);
            if (!u) break;
            out[i++] = (u < 0x100u) ? (char)u : '?';
            s += 2u;
        }
    } else {
        if (n > out_size - 1u) n = out_size - 1u;
        for (i = 0; i < n; i++) {
            uint8_t c = b[1u + i];
            if (c == 0) break;
            out[i] = (char)c;
        }
    }
    out[i] = 0;
    return i ? ID3_OK : ID3_NO_FRAME;
}

static int id3_find_text(const uint8_t *ring, uint32_t avail,
                          uint32_t tag_total_len, const char *frame_id,
                          char *out, uint32_t out_size)
{
    if (ring[0] != 'I' || ring[1] != 'D' || ring[2] != '3') return ID3_NO_TAG;
    uint8_t  major = ring[3];
    uint32_t scan_limit = tag_total_len < avail ? tag_total_len : avail;

    uint32_t p = 10;
    while (p + 10 <= scan_limit) {
        if (ring[p] == 0) break;   /* padding reached */

        uint32_t fsize = (major >= 4)
            ? (((uint32_t)(ring[p+4] & 0x7Fu) << 21) | ((uint32_t)(ring[p+5] & 0x7Fu) << 14) |
               ((uint32_t)(ring[p+6] & 0x7Fu) << 7)  | ((uint32_t)(ring[p+7] & 0x7Fu)))
            : (((uint32_t)ring[p+4] << 24) | ((uint32_t)ring[p+5] << 16) |
               ((uint32_t)ring[p+6] << 8)  |  (uint32_t)ring[p+7]);

        if (fsize == 0 || p + 10 + fsize > scan_limit) break;

        if (ring[p] == (uint8_t)frame_id[0] && ring[p+1] == (uint8_t)frame_id[1] &&
            ring[p+2] == (uint8_t)frame_id[2] && ring[p+3] == (uint8_t)frame_id[3] &&
            fsize > 1)
            return id3_text_body(&ring[p + 10], fsize, out, out_size);
        p += 10u + fsize;
    }
    return ID3_NO_FRAME;
}

/* Walk the tag off the CARD, a frame header at a time, and fill whatever text
 * fields are still empty.
 *
 * The in-memory parser above cannot reach these. It stops at the first frame
 * whose body runs past what is loaded, and three tracks on the test card put
 * APIC FIRST: 41 KB, 34 KB and 847 KB of picture ahead of every text frame. The
 * ring is 32 KB, so pulling more of the tag in -- the existing fallback -- can
 * never work for them however much it pulls. Those tracks displayed nothing at
 * all, which reads as "this file has no tags" rather than as a limitation.
 *
 * Cost is proportional to the NUMBER of frames, not to the size of the tag: the
 * body of a picture frame is skipped by arithmetic, never read. Widespread
 * Panic's 851 KB tag is about a dozen 16-byte reads. That is what makes this
 * affordable where an earlier frame-by-frame walk was not -- that one ran on
 * every track; this runs only where the cheap path already failed, which is the
 * handful of tracks that would otherwise show nothing. */
/* ID3v1: a fixed 128-byte block at the very END of the file, starting "TAG".
 *
 * Predates ID3v2 and is still the only tag a lot of older files carry, so
 * without this they display nothing at all. Fields are fixed width and padded
 * with spaces or NULs rather than terminated, so each one has to be trimmed
 * from the right.
 *
 * Only ever called after the v2 paths have found nothing, so a file carrying
 * both keeps its v2 values -- those are longer, and not limited to 30
 * characters or to Latin-1. */
static void id3v1_read(void)
{
    if (slot_size < 128u) return;
    if (!target_read_slot(MP3_SLOT_ID, slot_size - 128u, TAG_OFF, 128u)) return;
    if (tagbuf[0] != 'T' || tagbuf[1] != 'A' || tagbuf[2] != 'G') return;

    static const struct { uint8_t off, len; } f[3] = {
        {   3u, 30u },      /* title  */
        {  33u, 30u },      /* artist */
        {  63u, 30u },      /* album  */
    };
    char *const dst[3] = { track_title, track_artist, track_album };
    const uint32_t cap[3] = { sizeof(track_title), sizeof(track_artist),
                              sizeof(track_album) };

    for (uint32_t k = 0; k < 3u; k++) {
        if (dst[k][0]) continue;                  /* v2 already supplied it */
        uint32_t n = f[k].len;
        while (n && (tagbuf[f[k].off + n - 1u] == ' ' ||
                     tagbuf[f[k].off + n - 1u] == 0)) n--;
        if (n > cap[k] - 1u) n = cap[k] - 1u;
        for (uint32_t i = 0; i < n; i++) dst[k][i] = (char)tagbuf[f[k].off + i];
        dst[k][n] = 0;
    }

    if (!track_year[0]) {
        uint32_t n = 0;
        while (n < 4u && tagbuf[93u + n] >= '0' && tagbuf[93u + n] <= '9') {
            track_year[n] = (char)tagbuf[93u + n]; n++;
        }
        track_year[n] = 0;
    }

    /* ID3v1.1 puts a track number in the last two bytes of the comment: a NUL
     * followed by the number. A plain v1 comment runs through both, so the NUL
     * is what distinguishes them. */
    if (!track_trk[0] && tagbuf[125] == 0 && tagbuf[126]) {
        uint32_t t = tagbuf[126], i = 0;
        if (t >= 100u) track_trk[i++] = (char)('0' + t / 100u % 10u);
        if (t >= 10u)  track_trk[i++] = (char)('0' + t / 10u % 10u);
        track_trk[i++] = (char)('0' + t % 10u);
        track_trk[i] = 0;
    }
}

static void id3_walk_collect(uint32_t tag_len)
{
    /* TPE2 (band/album artist) is preferred, but plenty of files carry only
     * TPE1 (lead performer); TDRC is v2.4's year, TYER v2.3's. Duplicated
     * targets are harmless because a field already filled is skipped, so the
     * first of each pair to appear in the tag wins. */
    static const char *const want[7] = { "TIT2", "TPE2", "TPE1", "TALB",
                                         "TRCK", "TDRC", "TYER" };
    char *const dst[7] = { track_title, track_artist, track_artist, track_album,
                           track_trk, track_year, track_year };
    const uint32_t cap[7] = { sizeof(track_title), sizeof(track_artist),
                              sizeof(track_artist), sizeof(track_album),
                              sizeof(track_trk), sizeof(track_year),
                              sizeof(track_year) };

    if (tag_len < 20u) return;

    /* A sliding window rather than a read per frame header. Frames after a
     * picture are packed tightly -- Sea Wolf has eight in 200 bytes -- so a
     * header-sized read each time cost 29 round trips where four cover it. The
     * window also usually holds the text body, saving a second read. */
    uint32_t wo = 0, wl = 0;
#define ID3_WIN 512u
#define ID3_HAVE(o, n) ((o) >= wo && (o) + (n) <= wo + wl)

    if (!target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, ID3_WIN)) return;
    wo = 0; wl = ID3_WIN;
    if (tagbuf[0] != 'I' || tagbuf[1] != 'D' || tagbuf[2] != '3') return;
    uint8_t major = tagbuf[3];

    uint32_t p = 10;
    while (p + 10u <= tag_len) {
        if (!ID3_HAVE(p, 10u)) {
            if (!target_read_slot(MP3_SLOT_ID, p, TAG_OFF, ID3_WIN)) return;
            wo = p; wl = ID3_WIN;
        }
        const uint8_t *h = tagbuf + (p - wo);
        if (h[0] == 0) return;                         /* padding reached */

        uint32_t fsize = (major >= 4)
            ? (((uint32_t)(h[4] & 0x7Fu) << 21) | ((uint32_t)(h[5] & 0x7Fu) << 14) |
               ((uint32_t)(h[6] & 0x7Fu) << 7)  |  (uint32_t)(h[7] & 0x7Fu))
            : (((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) |
               ((uint32_t)h[6] << 8)  |  (uint32_t)h[7]);
        if (!fsize || p + 10u + fsize > tag_len) return;

        for (uint32_t k = 0; k < 7u; k++) {
            if (dst[k][0]) continue;                   /* already have it */
            if (h[0] != (uint8_t)want[k][0] || h[1] != (uint8_t)want[k][1] ||
                h[2] != (uint8_t)want[k][2] || h[3] != (uint8_t)want[k][3])
                continue;
            /* n is the whole frame body: the encoding byte plus its text. Pass
             * exactly what was READ -- passing one more made the decoder take a
             * byte beyond the buffer, which showed up as the next frame's first
             * letter stuck on the end of every walked title. */
            uint32_t n = fsize < 160u ? fsize : 160u;
            if (ID3_HAVE(p + 10u, n)) {
                id3_text_body(tagbuf + (p + 10u - wo), n, dst[k], cap[k]);
            } else if (target_read_slot(MP3_SLOT_ID, p + 10u, TAG_OFF, n)) {
                id3_text_body(tagbuf, n, dst[k], cap[k]);
                wl = 0;                    /* the read replaced the window */
            }
            break;
        }
        p += 10u + fsize;
    }
#undef ID3_HAVE
#undef ID3_WIN
}

static int title_is_stale(const char *title)
{
    if (slot_size == stale_ref_size) return 0;
    if (!title[0] || !stale_ref_title[0]) return 0;
    for (uint32_t i = 0; i < sizeof(stale_ref_title); i++) {
        if (title[i] != stale_ref_title[i]) return 0;
        if (!title[i]) break;
    }
    return 1;
}

/* Look for a VBR header in the first audio bytes. Searching for the ASCII tag
 * directly, rather than computing its offset from the frame header's
 * version/channel layout, keeps this independent of MPEG version. */
static uint32_t vbr_frame_count(void)
{
    uint32_t lim = ring_fill < 2048u ? ring_fill : 2048u;
    if (lim < 32u) return 0;
    for (uint32_t i = 0; i + 20u < lim; i++) {
        uint8_t a = ring[i], b = ring[i+1], c = ring[i+2], d = ring[i+3];
        if ((a == 'X' && b == 'i' && c == 'n' && d == 'g') ||
            (a == 'I' && b == 'n' && c == 'f' && d == 'o')) {
            uint32_t flags = ((uint32_t)ring[i+4] << 24) | ((uint32_t)ring[i+5] << 16) |
                             ((uint32_t)ring[i+6] << 8)  |  (uint32_t)ring[i+7];
            if (!(flags & 1u)) return 0;            /* no FRAMES field */
            /* The BYTES field, when present, is the file's own statement of how
             * long its audio is -- an exact figure to check the directory
             * against, where a bitrate estimate is only a guess on VBR. */
            if (flags & 2u)
                track_bytes = ((uint32_t)ring[i+12] << 24) | ((uint32_t)ring[i+13] << 16) |
                              ((uint32_t)ring[i+14] << 8)  |  (uint32_t)ring[i+15];

            /* The LAME extension follows the optional Xing fields, so its
             * offset depends on which flags are set -- FRAMES and BYTES are 4
             * bytes each, the TOC is 100, QUALITY is 4. Computing it from the
             * flags rather than assuming a fixed offset is the difference
             * between reading an encoder name and reading the middle of the
             * seek table. */
            uint32_t e = i + 8u;
            if (flags & 1u) e += 4u;            /* FRAMES  */
            if (flags & 2u) e += 4u;            /* BYTES   */
            if (flags & 4u) e += 100u;          /* TOC     */
            if (flags & 8u) e += 4u;            /* QUALITY */

            /* Only accept it if the whole field is in the buffer AND the string
             * is printable. A short read or a file with no extension would
             * otherwise put ring garbage on screen as an encoder name. */
            if (e + 10u < lim) {
                int ok = 1;
                for (uint32_t k = 0; k < 9u; k++)
                    if (ring[e+k] < 0x20u || ring[e+k] > 0x7Eu) { ok = 0; break; }
                if (ok) {
                    for (uint32_t k = 0; k < 9u; k++) track_encoder[k] = (char)ring[e+k];
                    track_encoder[9] = 0;
                    /* Trailing spaces: some encoders pad the field. */
                    for (int k = 8; k >= 0 && track_encoder[k] == ' '; k--)
                        track_encoder[k] = 0;
                    track_vbr_method = ring[e+9] & 0x0Fu;
                }
            }
            return ((uint32_t)ring[i+8]  << 24) | ((uint32_t)ring[i+9]  << 16) |
                   ((uint32_t)ring[i+10] << 8)  |  (uint32_t)ring[i+11];
        }
        if (a == 'V' && b == 'B' && c == 'R' && d == 'I') {
            track_bytes = ((uint32_t)ring[i+10] << 24) | ((uint32_t)ring[i+11] << 16) |
                          ((uint32_t)ring[i+12] << 8)  |  (uint32_t)ring[i+13];
            return ((uint32_t)ring[i+14] << 24) | ((uint32_t)ring[i+15] << 16) |
                   ((uint32_t)ring[i+16] << 8)  |  (uint32_t)ring[i+17];
        }
    }
    return 0;
}

/* Reads the head of the file and skips any ID3 tag, leaving the ring and
 * file_pos positioned at real audio. Returns 0 on I/O failure. */
static int read_track_head(void)
{
    refill_drain();     /* settle anything in flight before touching the ring */

    int attempt = 0, have_prev = 0;
    uint32_t skip = 0, prev_skip = 0;
    char prev_try[48];
    /* PROVE THE SLOT HAS SETTLED before reading anything we will act on.
     *
     * After a 0192 the slot does not switch instantly, and the old design
     * loaded immediately and then tried to DETECT having read the wrong file --
     * stale-title compares, retry budgets, periodic re-probes. Every one of
     * those was a way of noticing a bad read after committing to it, and a
     * wrong tag length means a wrong audio_start, which means the first audio
     * read lands mid-frame: a single loud click, exactly as reported, and
     * impossible on stop/restart where no file changes.
     *
     * Cheaper and surer: read the first 16 bytes twice, ~30 ms apart, and
     * require them to agree. A slot mid-switch does not return the same bytes
     * twice running; a settled one always does. Up to ~1 s, then proceed
     * anyway -- the retry loop below is the existing backstop.
     *
     * Costs nothing on the common path: the first two reads agree and it
     * proceeds, and it does not run at all for a restart, which never gets
     * here. */
    {
        /* Wait until the slot returns something DIFFERENT from the song we
         * just left, then stable.
         *
         * Requiring only stability was useless: a slot still serving the
         * PREVIOUS file returns identical bytes every time, so the check
         * passed instantly on exactly the stale data it existed to reject.
         * Proving "settled" is not the same as proving "switched".
         *
         * sw_prev_head is the outgoing file's first 16 bytes, captured before
         * the 0192. Sixteen, not four: every ID3v2.3 tag begins "ID3" plus a
         * version, so four bytes are the same constant for every tagged file --
         * the discriminator-that-does-not-discriminate this project has been
         * caught by before. Sixteen reaches the tag LENGTH and the first frame
         * id, which do differ.
         *
         * Then two agreeing reads, so a half-switched slot is not trusted
         * either. ~1 s cap, after which the existing retry loop takes over. */
        uint8_t prev[16];
        uint32_t tries = 0, agree = 0, differs = !sw_have_prev;
        for (uint32_t i = 0; i < sizeof(prev); i++) prev[i] = 0u;
        while (tries++ < 32u && !(differs && agree >= 2u)) {
            if (!target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, 16u)) { agree = 0; continue; }
            if (!differs) {
                for (uint32_t i = 0; i < sizeof(prev); i++)
                    if (tagbuf[i] != sw_prev_head[i]) { differs = 1; break; }
            }
            uint32_t same = 1;
            for (uint32_t i = 0; i < sizeof(prev); i++) {
                if (tagbuf[i] != prev[i]) same = 0;
                prev[i] = tagbuf[i];
            }
            agree = same ? agree + 1u : 0u;
            if (!(differs && agree >= 2u)) {
                uint32_t wait = cycles() + CLK_HZ / 32u;   /* ~30 ms */
                while ((int32_t)(cycles() - wait) < 0) { }
            }
        }
        sw_have_prev = 0;         /* one switch, one use */
    }

    /* ONCE, before the loop -- not on every retry.
     *
     * Flushing makes APF forget the slot's cluster chain, so the next read has
     * to walk it from the start. That is the point of doing it after a file
     * change, but doing it again on each retry means every attempt pays a full
     * walk, and the walk is proportional to file size -- which is why the
     * longest file in the set was the one that hiccuped. One flush is enough to
     * discard the stale chain; the retries that follow want the fresh one kept,
     * not thrown away again. */
    target_flush_slot_cache();

    for (;;) {

    /* ...then throw away one MP3-slot read before the one that matters. Only
     * the FIRST read after a file change comes back stale: the audio has
     * always been correct, and the audio comes from the SECOND read. */
    target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, 512);

    file_pos = 0; ring_fill = 0; ring_rd = 0;

    /* Poison the landing zone: stale-content and nothing-arrived otherwise
     * produce byte-for-byte the same evidence, since whatever was already in
     * the ring is also old mid-stream audio. */
    {
        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + RING_OFF);
        w[0] = 0xA5A5A5A5u; w[1] = 0xA5A5A5A5u;
    }

    if (!target_read(0, RING_OFF, REFILL_CHUNK)) return 0;
    ring_fill = REFILL_CHUNK;

    for (int i = 0; i < 4; i++) head_bytes[i] = ring[i];

    /* Converge rather than try to RECOGNISE a bad read: re-read until two
     * consecutive reads agree. That needs no reference value, no file size and
     * no theory of the cause, and it terminates on its own. */
    skip = id3_len(ring);
    track_title[0]  = 0;
    track_artist[0] = 0;
    track_album[0]  = 0;
    track_year[0]   = 0;
    track_trk[0]    = 0;
    title_status = ID3_NO_TAG;
    if (skip) {
        /* MUST happen before the audio re-read below, which overwrites ring[]
         * with audio content. TPE2 (band/album artist) rather than TPE1. */
        title_status = id3_find_text(ring, ring_fill, skip, "TIT2",
                                     track_title,  sizeof(track_title));
        id3_find_text(ring, ring_fill, skip, "TPE2",
                      track_artist, sizeof(track_artist));
        id3_find_text(ring, ring_fill, skip, "TALB",
                      track_album, sizeof(track_album));
        id3_find_text(ring, ring_fill, skip, "TRCK",
                      track_trk, sizeof(track_trk));

        /* Everything above only saw the first 4 KB of the tag. If the title is
         * not in there, the text frames sit past a large picture -- so walk the
         * tag properly rather than reporting a well-formed file as untagged.
         * Only the fields actually missing are looked up again. */
        /* Text frames past the artwork: pull MORE OF THE TAG into the ring and
         * parse it in memory, rather than walking it a frame header at a time.
         *
         * The walk cost ~29 separate SD reads, all inside the window where
         * pcm_flush() has emptied the FIFO -- audible on the one track whose
         * frames sit past a 15 KB picture, and on no other, because no other
         * track reaches this path. A 16 KB tag is three more 4 KB reads, and
         * the in-memory parser then finds everything for free. The ring is
         * 32 KB and is reloaded with audio immediately below, so filling it
         * with tag bytes here costs nothing. */
        if (title_status != ID3_OK && skip > ring_fill) {
            uint32_t want = skip;
            if (want > RING_SIZE) want = RING_SIZE;
            int ok = 1;
            while (ring_fill < want && ok) {
                uint32_t n2 = want - ring_fill;
                if (n2 > REFILL_CHUNK) n2 = REFILL_CHUNK;
                ok = target_read(ring_fill, RING_OFF + ring_fill, n2);
                if (ok) ring_fill += n2;
            }
            title_status = id3_find_text(ring, ring_fill, skip, "TIT2",
                                         track_title,  sizeof(track_title));
            if (!track_artist[0]) id3_find_text(ring, ring_fill, skip, "TPE2",
                                                track_artist, sizeof(track_artist));
            if (!track_album[0])  id3_find_text(ring, ring_fill, skip, "TALB",
                                                track_album, sizeof(track_album));
            if (!track_trk[0])    id3_find_text(ring, ring_fill, skip, "TRCK",
                                                track_trk, sizeof(track_trk));
            if (!track_year[0] && id3_find_text(ring, ring_fill, skip, "TDRC",
                                                track_year, sizeof(track_year)) != ID3_OK)
                id3_find_text(ring, ring_fill, skip, "TYER",
                              track_year, sizeof(track_year));
        }

        /* Still nothing found: the text frames sit behind a picture too large
         * for the ring to reach, however much of the tag was pulled in. Walk
         * the tag off the card, which skips a picture by arithmetic instead of
         * having to load it. Measured on the test card, this is the difference
         * between three tracks showing no metadata at all and showing all of
         * it. Runs only on those tracks -- anything the cheap path resolved
         * never gets here. */
        if (title_status != ID3_OK) {
            id3_walk_collect(skip);
            if (track_title[0]) title_status = ID3_OK;
        }
        /* Still nothing: the file may carry only an ID3v1 block at the end. */
        if (title_status != ID3_OK) {
            id3v1_read();
            if (track_title[0]) title_status = ID3_OK;
        }
        for (uint32_t i = 0; i < sizeof(track_trk); i++)
            if (track_trk[i] == '/') { track_trk[i] = 0; break; }  /* "5/12" -> "5" */
        if (id3_find_text(ring, ring_fill, skip, "TDRC",
                          track_year, sizeof(track_year)) != ID3_OK)
            id3_find_text(ring, ring_fill, skip, "TYER",
                          track_year, sizeof(track_year));

        track_year[4] = 0;
    }

    {
        int same = have_prev && (skip == prev_skip);
        for (uint32_t i = 0; same && i < sizeof(track_title); i++) {
            if (track_title[i] != prev_try[i]) same = 0;
            if (!track_title[i]) break;
        }
        if (same) break;                 /* two reads agree -> settled */
        if (attempt >= 1) break;         /* 0190 gates the load; belt and braces */
    }

    for (uint32_t i = 0; i < sizeof(track_title); i++) prev_try[i] = track_title[i];
    prev_skip = skip;
    have_prev = 1;

    reload_retries++;      /* R on screen = convergence passes, not failures */
    attempt++;
    uint32_t until = cycles() + CLK_HZ / 4u;         /* ~250 ms, then re-read */
    while ((int32_t)(cycles() - until) < 0) { }
    }

    audio_start  = skip;
    track_frames = 0;
    track_encoder[0] = 0; track_vbr_method = 0;
    track_secs   = 0;
    meas_rate    = 0; meas_pos0 = 0; meas_sec0 = 0;
    vbr_seen     = 0;

    /* Remember what we settled on, so later probes have a reference the load
     * itself cannot corrupt. */
    for (uint32_t i = 0; i < sizeof(track_title); i++) last_title[i] = track_title[i];
    cur_file_id = slot_file_id();

    if (skip) {
        file_pos  = skip;
        ring_fill = 0; ring_rd = 0;
        if (!target_read(skip, RING_OFF, REFILL_CHUNK)) return 0;
        ring_fill = REFILL_CHUNK;
    }
    track_bytes  = 0;
    size_suspect = 0;
    track_frames = vbr_frame_count();

    /* CROSS-CHECK the directory against the file's own account of itself.
     *
     * A Xing/VBRI BYTES field states exactly how many bytes of audio follow the
     * header, so audio_start + that is the file's real length -- give or take an
     * ID3v1 trailer. A corrupt directory entry inflates the SIZE while leaving
     * the audio intact, which is the damage seen twice on this card: .mp3s
     * reporting ~4x their true length with every frame still decoding.
     *
     * 1/4 over is far beyond any legitimate trailer and far below the observed
     * corruption, so it neither cries wolf nor misses the real thing. Files
     * without a Xing header simply are not checked -- a precise test on some
     * files beats a vague one on all of them. */
    if (track_bytes && slot_size) {
        uint32_t real = audio_start + track_bytes;
        if (slot_size > real + real / 4u) {
            size_suspect = 1;
            slot_size    = real;      /* trust the audio, not the directory */
        }
    }
    /* Nothing to check on a file with no Xing/VBRI header -- it never states
     * its own length, so there is nothing to disagree with. Better an exact
     * test on the files that can be tested than a guess applied to all of
     * them; a player that cries wolf about healthy files is worse than one
     * that stays quiet about a case it genuinely cannot judge. */
    return 1;
}

/* Everything needed to start a track from the beginning, shared by boot and by
 * a reload. ONE function deliberately -- two copies of this drift apart. */
static int load_track(void)
{
    /* Helix carries bit-reservoir state internally, so a fresh instance is
     * needed rather than just resetting our own bookkeeping. */
    if (dec) MP3FreeDecoder(dec);
    dec = MP3InitDecoder();
    if (!dec) { REG(R_STAT2) = 0xF0000000u; return 0; }

    /* Silence the old track's tail before anything else touches the ring. */
    pcm_flush();

    frames = 0; errs = 0; rate_set = 0; min_level = 0xFFFFFFFFu;
    track_kbps = 0; track_hz = 0; samp_per_frame = 1152u;
    bytes_per_sec = 16000u;
    paused = 0;
    seek_req = 0; soft_restart_req = 0;
    st0 = 0; REG(R_STAT0) = 0; REG(R_STAT1) = 0; REG(R_STAT2) = 0; REG(R_STAT3) = 0;
    st0 |= (1u << 0); REG(R_STAT0) = st0;            /* decoder up */

    /* The RTL only latches R_SLOT_SZ on a reload edge, so at boot there is
     * genuinely nothing to read. Take APF's number when there is one -- but
     * NOT after a core-initiated 0192 open, which raises no such edge and
     * would leave the PREVIOUS track's size here: wrong total time, wrong
     * end-of-track, and an end-of-track that fires early or never. */
    slot_size = force_size_probe ? 0u : REG(R_SLOT_SZ);
    force_size_probe = 0;

    uint32_t t0 = cycles(), tphase = t0;
    if (!read_track_head()) { REG(R_STAT2) = 0xE0000000u; return 0; }
    ld_head = LD_MS(cycles() - tphase); tphase = cycles();
    if (audio_start) { st0 |= (1u << 1); REG(R_STAT0) = st0; }

    /* The size probe is ~20 blocking reads -- measured at 480 ms, and it runs
     * with the FIFO empty, which is most of the hiccup. Two attempts at caching
     * it away both silently failed to hit, so the answer is not a better cache:
     * an operation that long simply cannot live here.
     *
     * It is now INCREMENTAL. The main loop performs one read per pass, only
     * when the buffer is full, so the search spreads harmlessly across a couple
     * of seconds of playback instead of stalling the start of it. Nothing needs
     * the size immediately: it feeds the total time, the progress bar and the
     * end-of-track check, none of which matter in the first second.
     *
     * A file that declares its own length still skips all of this. */
    if (slot_size <= audio_start && track_bytes)
        slot_size = audio_start + track_bytes;
    if (slot_size <= audio_start)
        /* Blocking, and DELIBERATELY so, here and only here: this is the
         * silent part of a load -- the FIFO is flushed and its output has
         * glided to zero -- so these reads cost gap length, not audio. This
         * probe was deleted while hunting the transition tic; the tic turned
         * out to be the FIFO edges and the outgoing-track stutter, fixed where
         * they actually were. Deleting this only traded away the instant,
         * accurate total time. It was never the click. */
        slot_size = probe_file_size();
    ld_size = LD_MS(cycles() - tphase); tphase = cycles();

    /* Cover art BEFORE the chrome, because whether it exists decides the
     * layout: no art means no panel and a full-width waveform. Also before
     * prefill, so its blocking reads cannot starve playback. */
    /* Skip the art entirely when the file has not changed.
     *
     * This is the long pole in a restart. pcm_flush() has already emptied the
     * FIFO, and everything between it and prefill() runs with the DAC holding a
     * DC level -- the head read, the size probe, and a full JPEG decode with
     * its own SD reads. Re-decoding artwork that is already in the SDRAM stash
     * bought nothing and dominated that gap, which is what the hiccup on B
     * actually was. Same file, same picture: reuse it.
     *
     * Keyed on the 0190 file identity, so a genuine track change still decodes
     * and only a restart of the same file skips. */
    art_ready = 0;
    int has_art;
    if (cur_file_id && cur_file_id == art_file_id) {
        has_art = art_have;
    } else {
        ui_art_mount();
        has_art = art_decode(audio_start);
        if (!has_art) ui_art_placeholder();
        ui_art_round();
        art_file_id = cur_file_id;
    }
    art_ready = 1;
    ld_art = LD_MS(cycles() - tphase); tphase = cycles();

    /* Panel state follows the TRACK, not the session. art_x is set directly
     * rather than animated -- a track change should not look like a slide. */
    art_have  = (uint8_t)has_art;
    art_shown = (uint32_t)(has_art && art_pref);
    art_x     = art_shown ? ART_X : FB_W;

    ui_draw_chrome();   /* title/artist are populated now -- draw the UI */

    if (!prefill()) { REG(R_STAT2) = 0xD0000000u; return 0; }

    /* Warm the decoder BEFORE playback, still inside the silent gap.
     *
     * The user's stop-vs-skip experiment isolated this: a warm decoder starts
     * clean, a fresh one tics. A fresh decoder's first successful frame is
     * synthesised through empty polyphase and overlap state; decode up to and
     * including that frame here and discard it, so the first frame that
     * actually PLAYS goes through a decoder in steady state -- the same
     * condition the proven-clean warm path starts from. Costs ~26 ms of gap
     * and the first ~26 ms of the track.
     *
     * Discarding was tried once before and made things worse -- but that was
     * before the glide, when the FIFO held a DC level and every extra frame
     * lengthened the hold. The output now rests at true zero through the gap,
     * so the trade is purely: one inaudible frame for a steady-state start. */
    {
        /* Discard until the RESERVOIR is genuinely populated, not just until
         * one frame has decoded.
         *
         * MP3 lets a frame reference up to 511 bytes of main_data from frames
         * BEFORE it. After a decoder reset that data does not exist, so early
         * frames are synthesised from an empty reservoir -- garbage, at full
         * amplitude. Discarding a single frame covers that only if one frame
         * exceeds 511 bytes, which holds at 256 kbps and fails at 112.
         *
         * This is why the fault tracked the Xing header rather than anything
         * about the loads: a Xing header IS a real frame containing silence, so
         * on those files the reservoir warms up on inaudible content before any
         * music is decoded. Headerless files start straight into audio and had
         * no such runway.
         *
         * Consume at least 512 bytes of successfully decoded frames -- one
         * frame at high bitrates, two or three at low, adaptive rather than a
         * fixed count, and 26-78 ms of a track's very start. */
        int guard = 12;                      /* never loop on a broken stream */
        uint32_t warmed = 0;                 /* bytes of DECODED frames so far */
        while (guard-- && warmed < 512u) {
            int bl = (int)(ring_fill - ring_rd);
            if (bl < 512) break;
            int off = MP3FindSyncWord(&ring[ring_rd], bl);
            if (off < 0) break;
            ring_rd += (uint32_t)off; bl -= off;
            unsigned char *ib = &ring[ring_rd];
            int before = bl;
            int e = MP3Decode(dec, &ib, &bl, pcm, 0);
            uint32_t used = (uint32_t)(before - bl);
            ring_rd += used;
            if (e == 0) {
                warmed += used;              /* only real frames count */

                /* Establish the stream's real rate HERE, off the warm-up
                 * frame, not on the first frame that plays.
                 *
                 * The hand-off ends load_track by running the reposition body,
                 * which finishes with ui_draw_dynamic() -- and that used to run
                 * before any frame had been decoded, so track_secs was 0 and
                 * bytes_per_sec was still the 16000 placeholder. The total was
                 * computed from a fake bitrate and drawn wrong on EVERY file,
                 * where before only headerless ones were ever estimated.
                 *
                 * These frames are decoded and discarded anyway; taking the
                 * frame info from them costs nothing and means the duration is
                 * right before anything can draw it. */
                if (!rate_set) {
                    MP3FrameInfo wfi;
                    MP3GetLastFrameInfo(dec, &wfi);
                    if (wfi.samprate) {
                        pcm_rate_apply(wfi.samprate);
                        if (wfi.bitrate) bytes_per_sec = wfi.bitrate / 8u;
                        samprate   = wfi.samprate;
                        track_hz   = wfi.samprate;
                        if (wfi.nChans && wfi.outputSamps)
                            samp_per_frame = (uint32_t)wfi.outputSamps
                                           / (uint32_t)wfi.nChans;
                        track_kbps = wfi.bitrate / 1000u;
                        if (track_frames && wfi.nChans) {
                            uint32_t spf = (uint32_t)wfi.outputSamps / (uint32_t)wfi.nChans;
                            track_secs = (uint32_t)(((uint64_t)track_frames * spf)
                                                    / wfi.samprate);
                        }
                        rate_set = 1;
                    }
                }
            }
            else if (!used) ring_rd++;       /* no progress: step past it */
        }
    }

    ld_pre   = LD_MS(cycles() - tphase);
    ld_total = LD_MS(cycles() - t0);
    return 1;
}

int main(void)
{
    /* Bitstream/firmware interlock. On mismatch paint an unmistakable pattern
     * and stop, rather than running on stale RTL and presenting it as a
     * mysterious hardware fault. */
    if (REG(R_VERSION) != EXPECT_VERSION) {
        REG(R_STAT0) = 0xAAAAAAAAu; REG(R_STAT1) = 0x55555555u;
        REG(R_STAT2) = 0xAAAAAAAAu; REG(R_STAT3) = 0x55555555u;
        for (;;) { }
    }

    /* Clear the screen FIRST. SDRAM powers up holding garbage and the scanout
     * engine displays it the moment video comes alive, so anything slow before
     * the first fill is visible as a screenful of noise. */
    fb_rect(0, 0, FB_W, FB_H, UI_BG);

    vol_apply();

    /* ---- DIAGNOSTIC: dump APF's datatable, hold SELECT at boot ----
     *
     * Analogue's docs say a slot's size comes from "the Dataslot ID/Size Table
     * BRAM in the core" and that 0xF8xxxxxx is reserved for framework
     * communication -- but they never give the table's layout. This core has
     * been using that same BRAM as scratch (0190 response at words 0..63, 0192
     * parameters at 64..127, settings at 128+), which is the leading suspect
     * for both the 0184 corruption and the nonvolatile boot hang.
     *
     * So read it before anything of ours touches it. MUST run before
     * settings_load() and pl_load(): the first 0190 overwrites words 0..63.
     *
     * SNAPSHOT here, VIEW later. Gating this on a held button did not work:
     * it runs microseconds after the CPU leaves reset, before the Pocket has
     * delivered any controller state, so the read was always 0. The capture
     * has to happen now; the viewing does not. Select+A shows it. */
    dt_snapshot();

    /* Settings FIRST, so the splash is drawn in the accent the user actually
     * chose. It only reads a slot -- nothing on screen depends on it -- and
     * painting before it meant the very first thing shown was always the
     * default orange regardless of what had been saved. */
    settings_load_ok = (uint8_t)settings_load();

    /* Derive the background tint ONCE, here, where ui_accent has reached its
     * final value whether it was restored or left at the default. Doing it
     * inside the restore instead would miss the default path entirely --
     * settings_load() returns early when nothing has been saved -- and the
     * first boot would show a saved-colour UI on the untinted ramp. */
    ui_grad_set(ui_accent);

    /* Something deliberate on screen BEFORE the slow work -- reading the
     * playlist, opening a track, decoding cover art. Previously the first
     * paint came after all of that. */
    ui_splash_anim();

    /* Before the track, deliberately: reading the playlist slot makes APF drop
     * its fragment cache for the MP3 slot, so doing it once here costs nothing
     * while doing it mid-stream would make every refill re-walk the cluster
     * chain. No playlist on the card simply leaves pl_count at 0. */
    /* Armed only around this call, so the indicator means "reading the .m3u"
     * and nothing else -- the track open and artwork decode that follow are a
     * separate wait and deliberately do not claim this label. */
    ui_boot_note("LOADING PLAYLIST");
    pl_load();
    ui_boot_clear();
    /* Only when there is something to report. A "0 TRACKS" line would be
     * answered a moment later by the idle screen saying the same thing at
     * length, and saying it twice in two places reads as a fault. */
    if (pl_count) ui_splash_summary(pl_live_count());
    /* Land it HERE. Running on through load_track() was tried and looked wrong:
     * ART_Y sits inside the meter band, so the artwork panel paints over the
     * bars while the animation is still redrawing them, and the settle never
     * gets a clean frame. */
    ui_wave_anim_stop();

    /* Try whatever is already in the slot -- a file picked from the Pocket's
     * browser, or one left there by a previous session. If that comes to
     * nothing and a playlist exists, start it; the common case is then launch
     * straight into music with no interaction at all. */
    /* The same indicator the .m3u read gets, for the same reason. Opening a
     * track blocks on the head read, the prefill and the artwork decode, and
     * with nothing on this row the splash just sits there looking hung until
     * the player appears -- which is precisely how it was reported.
     *
     * The dots cost nothing to animate: ui_boot_tick() is driven from inside
     * target_read_slot()'s spin, and it returns immediately unless a note is
     * armed, so arming one here is the whole change. Cleared before anything
     * else paints, since PLAYLIST / N TRACKS lands on this same row. */
    /* Say which of the two is happening. A resume takes the same visible
     * moment as an ordinary load, and "RESUMING" is the only clue the user
     * gets that the position was remembered before the player appears. */
    ui_boot_note((resume_on && resume_word && RS_SECS(resume_word) > 2u)
                 ? "RESUMING TRACK" : "LOADING TRACK");
    /* A saved playlist point goes STRAIGHT to the playlist, before the slot is
     * consulted at all.
     *
     * The MP3 slot still holds whatever played last -- 0192 leaves it there --
     * so trying the slot first meant from_slot almost always won and the
     * playlist branch never ran. The seek then had to be applied to whatever
     * the slot happened to contain, which is how a standalone mp3 ended up
     * being repositioned to a playlist track's timestamp. Choosing the source
     * first makes the position unambiguous: it belongs to the track this
     * branch just started. */
    int from_slot = 0, from_list = 0;
    if (resume_on && resume_word && RS_PL(resume_word) && pl_count
        && RS_SECS(resume_word) > 2u) {
        uint16_t f = RS_TRACK(resume_word);
        if (f < pl_count) {
            pl_resync(f);
            from_list = pl_play_at(pl_pos);
        }
    }
    if (!from_list) from_slot = load_track();
    /* The splash is already up; leave it while the playlist track loads
     * rather than flashing instructions that are about to be replaced.
     *
     * Start at the SAVED track when there is one. pl_resync() finds the file
     * index in whatever order this boot produced, so a resumed track is right
     * even when shuffle has just reshuffled the list. */
    /* Nothing in the slot and nothing resumed: start the playlist normally. */
    if (!from_slot && !from_list && pl_count) from_list = pl_play_at(0);
    ui_boot_cancel();          /* not _clear: see the note on that function */

    /* Arm the position seek only if the file that ACTUALLY opened is the one
     * the point was saved against. A .m3u edited since would otherwise apply
     * one track's timestamp to another. Under two seconds is not worth a
     * reposition -- it is just the start of the track. */
    /* NO LONGER GATED ON FILE IDENTITY.
     *
     * Two attempts at an identity that survives a power cycle both failed.
     * cur_file_id hashes the 0190 response, which moves between boots.
     * Hashing the filename should have been stable and was not either -- the
     * name is recovered as the longest printable-ASCII run out of a shared
     * datatable, so which run wins can differ depending on what else has been
     * through that buffer. Every saved point was rejected, and the feature has
     * never once worked because of a check meant to protect it.
     *
     * So ask a question that can actually be answered. Instead of "is this the
     * same file", ask "is this position sensible for the file I have" -- which
     * needs no identity, only the file itself:
     *
     *   - the target must land inside the file (checked at the seek)
     *   - it must not be past a known duration (checked here)
     *
     * If a playlist has been edited underneath us the worst case is starting a
     * track 56 seconds in, which B undoes. That is a far smaller cost than a
     * guard that rejects everything, and unlike the guard it cannot fail
     * silently -- a wrong resume is audible immediately. */
    /* Only the track the resume branch itself started. from_slot means the
     * slot's own file, which the point says nothing about. */
    if (!resume_on || !resume_word || !RS_PL(resume_word)) resume_dbg = 3u;
    else if (!from_list)                       resume_dbg = 4u;
    else {
        resume_at = RS_SECS(resume_word);
        /* Past the end of a track whose length we know: not this file. */
        if (track_secs && resume_at + 2u >= track_secs) {
            resume_at = 0; resume_dbg = 2u;
        } else if (resume_at > 2u) {
            resume_seek_req = 1u; resume_dbg = 1u;
            /* Longer than the reload gate's own hard cap of 5 s, or this
             * expires while the load it is waiting for is still settling. */
            resume_deadline = cycles() + CLK_HZ * 12u;
        } else {
            resume_dbg = 3u;
        }
    }

    if (from_slot) {
        pl_report();
    } else if (!from_list) {
        idle = 1;
        ui_wave_anim_stop();
        /* Say WHICH nothing this is. A playlist whose every entry is
         * mistyped and no playlist at all both land here, and the idle
         * screen is otherwise indistinguishable from the splash that was
         * already up -- which is exactly what "the core never leaves the
         * boot screen" was. */
        ui_idle_screen(pl_count            ? "No playable tracks in playlist"
                     : pl_status == PL_ERR_EMPTY ? "Playlist has no tracks"
                     : (const char *)0);
    }

    for (;;) {
        poll_input();

        /* Ticks the idle counter and blanks when it reaches the timeout.
         *
         * MUST be at the top of the loop, not the bottom: paused, stopped and
         * idle all `continue` before reaching the end, so a pump down there
         * only ever ran while a track was actively decoding -- which is the one
         * state that does NOT need burn-in protection, the meters being in
         * motion anyway. A player parked on a static screen (paused, or stopped
         * at the end of a non-repeating playlist, which sets `paused` itself)
         * is the whole reason this feature exists and was exactly the case it
         * missed. poll_input() resets the counter on a press just above, so the
         * ordering here is right. */
        ui_blank_pump();
        resume_pump();

        /* Keeps the LOADING dots moving through the reload gate. ui_boot_tick()
         * is otherwise driven only from inside target_read_slot()'s spin, and
         * the settle/probe wait issues no reads at all -- so without this the
         * indicator would appear and then sit frozen for over a second, which
         * looks more broken than no indicator. Returns immediately unless a
         * note is armed. */
        ui_boot_tick();

        /* A reload is NOT acted on the instant it is announced: 008A fires
         * when the user PICKS a file, not when the slot is readable. The old
         * track keeps decoding through the wait, so this costs no silence. */
        if (reload_pending && !reload_armed) {
            reload_status  = REG(R_RELOAD);

            /* Capture what we are leaving BEFORE slot_size is replaced. */
            for (uint32_t i = 0; i < sizeof(last_title); i++)
                stale_ref_title[i] = last_title[i];
            stale_ref_size    = slot_size;
            stale_ref_file_id = cur_file_id;
            slot_size         = REG(R_SLOT_SZ);

            /* Same cut as a skip, for the same reasons: the slot may already
             * be serving the NEW file, so both continuing to decode the ring
             * and refilling it are wrong. The old advice that the old track
             * "keeps playing through the wait" predates 0192 and was never
             * safe once the slot switches under the reader. */
            pcm_flush();
            refill_drain();
            ring_fill = 0; ring_rd = 0;

            /* The user has chosen a file, so any resume still waiting to fire
             * is void. It was armed at boot for the track that was in the slot
             * THEN, and applying its position to something just picked would
             * drop the new file in at an unrelated timestamp.
             *
             * Only a genuine 008A reaches here -- pl_arm_load() drives the
             * gate by setting reload_armed directly and never sets
             * reload_pending -- so resume's own playlist load cannot cancel
             * itself here. */
            resume_seek_req = 0;
            track_from_pl  = 0u;            /* the user chose this one */

            REG(R_RELOAD)  = 1;             /* ack */
            reload_pending = 0;
            reload_armed    = 1;
            reload_probe_at = cycles();
            reload_settle   = cycles() + CLK_HZ * 3u / 2u;   /* blind fallback */
            reload_at       = cycles() + CLK_HZ * 5u;        /* hard cap       */
            /* Same gap: this gate waits at least 1.5 s before the track even
             * opens. The splash carries LOADING TRACK from the idle screen,
             * but with the player up there was nothing at all. */
            /* The boot path sets resume_seek_req before the main loop, so the
             * gate load that follows is part of RESUMING -- saying LOADING
             * here overwrote the splash's own message with a contradiction. */
            ui_boot_note(resume_seek_req ? "RESUMING TRACK" : "LOADING TRACK");

            /* Say so NOW, not when the gate below finally opens. That wait is
             * at least 1.5 s and can reach 5 s, and with the screen unchanged
             * for that long the pick looks ignored -- which is what "I have to
             * load it twice" was: no feedback, so the natural response is to
             * pick again. Same row as LOADING PLAYLIST, via the same helper,
             * because they are the same status line saying what it is doing.
             *
             * Only from the idle screen: UI_BOOT_Y falls between the
             * getting-started steps, so the splash goes up first to give the
             * indicator a clean card. With the player already on screen its
             * own repaint is the feedback. */
            if (idle) { ui_splash(); ui_boot_note("LOADING TRACK"); }
        }
        if (reload_armed) {
            /* ASK APF, do not infer: slot_file_id() hashes the 0190 response,
             * so "has the slot changed?" is answered by the file's identity
             * rather than by its contents. */
            /* ONE rule for both cases, because the old split had no
             * confirmation at all when stale_ref_file_id was 0 -- it waited a
             * blind 1.5 s and then read the slot whether or not APF had
             * switched it. That is the state every first load of a session is
             * in (nothing has been opened, so there is no previous identity),
             * so the very first Load MP3 was the one case that guessed. When
             * the guess was early the read failed, the idle screen came back,
             * and the second attempt worked because the slot had caught up by
             * then: exactly the reported "load it twice".
             *
             * A missing previous identity does not mean there is nothing to
             * confirm. Zero means APF is serving nothing; non-zero means it is
             * serving a file. So non-zero IS the change when there was no
             * previous id, and the same comparison covers both cases.
             *
             * reload_settle stays as a FLOOR rather than an alternative: if a
             * previous load failed the slot can still hold the old file, whose
             * id is non-zero and would satisfy the probe immediately. */
            int ready = 0;
            if ((int32_t)(cycles() - reload_settle)   >= 0 &&
                (int32_t)(cycles() - reload_probe_at) >= 0) {
                reload_probe_at = cycles() + CLK_HZ / 10u;
                /* Refreshed each probe: a toast holds ~1 s then dissolves, and
                 * this gate waits at least 1.5 s, so without this it fades out
                 * mid-load and reads as nothing happening. */
                ui_toast_msg("LOADING TRACK");
                uint32_t id = slot_file_id();
                if (id != 0u && id != stale_ref_file_id) ready = 1;
            }

            if (ready || (int32_t)(cycles() - reload_at) >= 0) {
                reload_armed = 0;

                /* The indicator went up when the pick was detected, not here.
                 * was_idle only decides what to restore if the open fails. */
                int was_idle = idle;
                if (!was_idle)
                    ui_boot_note(resume_seek_req ? "RESUMING TRACK"
                                                 : "LOADING TRACK");
                int opened   = load_track();
                ui_boot_cancel();          /* chrome has repainted the row */
                /* Icons and the PLAYING label are tracked separately, so a
                 * wiped row needs both invalidated or the label never returns. */
                ui_mode_dirty = 1;
                ui_last_pause = 0xFFFFFFFFu;

                if (!opened && was_idle) {
                    /* The splash went up to carry the indicator; put the
                     * instructions back rather than leaving a bare card. */
                    ui_idle_screen((const char *)0);
                }

                if (opened) {
                    idle = 0;
                    /* Hand off to the reposition body that is PROVEN clean.
                     *
                     * The user's evidence has been consistent for many rounds:
                     * stop-then-play and restart never click, a track change
                     * always does, and both end on the same audio at the same
                     * position. Rather than keep hunting the specific defect
                     * inside load_track's start-up -- one guess a round, none of
                     * them right -- let load_track do only what it uniquely
                     * must (open the file, read its tag, decode its art) and
                     * then START it the way the working path starts things:
                     * flush, rewind to audio_start, refill, prefill.
                     *
                     * Costs one extra prefill inside a gap that is already
                     * silent. Buys the guarantee that every route into playback
                     * is the same route. */
                    stop_req = 1u;
                    if (hold_paused) { hold_paused = 0; paused |= 1u; stopped = 1u; }
                    /* Nothing queues the first track any more. Picking a
                      * file is an explicit request to hear it, and the old
                      * behaviour -- the first track after launch loading
                      * paused, so music never starts the instant the core
                      * opens -- read as the player being stuck. */
                } else if (!idle) ui_load_failed();
                continue;
            }
        }

        /* Select+B: freeze on the raw 0190 struct. Press again to resume;
         * decoding continues throughout, only drawing is suspended. */
        if (dt_dump_req) {
            dt_dump_req = 0;
            ui_dump_mode ^= 1u;
            if (ui_dump_mode) dt_dump_boot();
            else              ui_draw_chrome();
            continue;
        }

        if (pl_dump_req) {
            pl_dump_req = 0;
            ui_dump_mode ^= 1u;
            if (ui_dump_mode) pl_dump_struct();
            else              ui_draw_chrome();
            continue;
        }

        /* The user picked a different playlist. Re-reading slot 3 flushes the
         * MP3 slot's fragment cache, so this pauses briefly rather than doing
         * it underneath a running stream. */
        /* ARM, do not act. 008A says the user picked a playlist, not that APF
         * has switched the slot -- exactly the fault the MP3 slot needed a
         * gate for, and it showed here as "sometimes you have to pick the new
         * playlist twice": the first read still returned the OLD list, and the
         * second attempt worked only because the slot had caught up by then.
         *
         * Record what we are leaving, then wait for 0190 to report something
         * else. Positive confirmation, not a blind delay. */
        if (pl_reload_pending) {
            pl_reload_pending = 0;
            REG(R_RELOAD) = RL_PL_RELOAD;            /* ack just this bit */
            for (uint32_t i = 0; i < sizeof(pl_leaving); i++)
                pl_leaving[i] = pl_name_raw[i];
            pl_reload_armed = 1u;
            pl_probe_at     = cycles();
            pl_reload_at    = cycles() + CLK_HZ * 5u;
            /* Say something immediately. The gate below waits for APF to
             * switch the slot -- usually quick, five second cap -- and with
             * the player still up and the old track still playing there is
             * otherwise nothing to show the pick registered. The boot row
             * cannot be used: UI_BOOT_Y is the live transport row. */
            ui_boot_note("LOADING PLAYLIST");
            continue;
        }

        if (pl_reload_armed) {
            int expired = (int32_t)(cycles() - pl_reload_at) >= 0;
            int due     = (int32_t)(cycles() - pl_probe_at)  >= 0;

            if (due || expired) {
                pl_probe_at = cycles() + CLK_HZ / 10u;
                pl_name_read();
                /* Refreshed each probe: a toast holds ~1 s then dissolves, so
                 * a slow switch would fade it out mid-load. */
                ui_toast_msg("LOADING PLAYLIST");

                int changed = 0;
                for (uint32_t i = 0; i < sizeof(pl_leaving); i++) {
                    if (pl_name_raw[i] != pl_leaving[i]) { changed = 1; break; }
                    if (!pl_leaving[i]) break;
                }

                if (changed || expired) {
                    pl_reload_armed = 0;
                    pl_load_n++;
                    ui_boot_note("LOADING PLAYLIST");
                    /* Same cut as a skip: pl_load() blocks on slot-3 reads for
                     * longer than the FIFO holds, and picking a playlist means
                     * leaving the current track anyway. */
                    pcm_flush();
                    refill_drain();
                    ring_fill = 0; ring_rd = 0;
                    pl_load();
                    /* Release the row WITHOUT wiping: it is the transport row
                     * in the player, and ui_mode_dirty below repaints it. */
                    ui_boot_cancel();
                    ui_mode_dirty = 1;
                    ui_last_pause = 0xFFFFFFFFu;
                    pl_report();
                    /* Only take playback if nothing else is claiming it. A
                     * Load MP3 pick can bring a playlist notification with it,
                     * and starting track 1 then discards the chosen file. */
                    if (pl_count && !reload_pending && !reload_armed)
                        pl_play_at(0);
                    ui_mode_dirty = 1;
                    continue;
                }
            }
            /* Still waiting for APF to switch the slot. Fall through so the
             * current track keeps playing while it does. */
        }

        /* Track skip (Left/Right held). pl_play_at() issues 0192; APF then
         * raises 008A and the ordinary reload path does the actual loading. */
        if (skip_req) {
            uint32_t d = skip_req; skip_req = 0;
            /* load_track() clears `paused`, so a track changed while paused or
             * stopped would start playing on its own. Browsing a playlist
             * without committing to hearing it is the point of allowing this
             * while paused. */
            hold_paused = (paused & 1u) ? 1u : 0u;
            /* Cut the outgoing track AT THE PRESS. pl_open_name blocks on 0190
             * and 0192 for longer than the 43 ms the FIFO holds, so leaving
             * the old track running meant it dipped to silence, faded BACK IN
             * for the settle window, then was cut again -- a stutter heard in
             * the outgoing song on every skip. A skip means the user is done
             * with this track; end it cleanly at the button. */
            pcm_flush();
            refill_drain();
            if (pl_skip(d == 1u ? 1 : -1)) {
                /* Slot now serves the NEW file: the ring's remaining bytes are
                 * the only old-track audio left, and refilling at the old
                 * offset would read the wrong file. Drop them; stay silent
                 * until the reload lands. */
                ring_fill = 0; ring_rd = 0;
                ui_toast_set("TRACK", (uint32_t)pl_live_ordinal(pl_pos), 0);
                ui_mode_dirty = 1;
            }
            continue;
        }

        /* EQ preset change. The filter is in the RTL and the audio never stops
         * flowing, so this is seamless in a way a track change can never be --
         * no flush, no reload, nothing to resynchronise. */
        if (eq_apply) {
            eq_apply = 0;
            REG(R_EQ) = eq_idx;
            ui_mode_dirty = 1u;          /* the mode row NAMES the preset */
            settings_mark_dirty();
        }

        /* Only when nothing is loading: a write is an SD round trip, and the
         * quiet window means it never lands in the middle of a track change. */
        if (!reload_armed && !reload_pending) settings_pump();


        if (art_toggle) {
            art_toggle = 0;
            art_pref  = (uint8_t)!art_pref;
            art_shown = (uint32_t)(art_pref && art_have);
            settings_mark_dirty();
            art_next   = cycles();          /* start moving immediately */
            /* Only the waveform changes shape; repainting the whole chrome
             * here would flash the text for no reason. */
            ui_wave_clear();
        }

        /* Probe-driven correction: jump to the audio_start it just proved
         * right, keeping the tag it just recovered. Deliberately does not go
         * through load_track(), which would re-read the head. */
        if (soft_restart_req) {
            soft_restart_req = 0;
            if (dec) MP3FreeDecoder(dec);
            dec = MP3InitDecoder();
            if (!dec) { REG(R_STAT2) = 0xF0000000u; ui_load_failed(); continue; }
            pcm_flush();
            refill_drain();
            frames = 0; errs = 0; rate_set = 0; min_level = 0xFFFFFFFFu;
            track_kbps = 0; track_hz = 0;
            file_pos  = audio_start;
            ring_fill = 0; ring_rd = 0;
            if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
            continue;
        }

        /* Reposition to 0:00 -- serves BOTH Start (stop, stays paused) and B
         * (restart, keeps playing). One body, differing only in the pause
         * flags poll_input set.
         *
         * B used to run a FULL cold load here -- fresh decoder, head reads,
         * art -- and that path clicked where this one does not. The proof was
         * the user's own experiment: stop-then-play lands on the identical
         * audio at the identical position through this body and is clean, so
         * restart now IS this body. The cold reload was a relic of the
         * stale-tag era; for the SAME file there is nothing to re-resolve.
         * Track CHANGES still load cold, as they must. */
        /* Absolute reposition for a resumed track. Deliberately the same body
         * as stop_req below, differing only in the target: that path is the
         * one proven not to click, and every route into playback going through
         * the same code is why track changes stopped clicking at all.
         *
         * Runs once, after the track is open and its rate is known. If the
         * rate is not known yet or the target lands past the end of the file,
         * the resume is simply dropped and the track plays from the start --
         * a wrong seek is worse than none. */
        if (resume_seek_req) {
            uint32_t rate = ui_byte_rate();
            uint32_t want = audio_start + rate * resume_at;

            /* RETRY, do not drop. slot_size is ZERO at the moment the track
             * finishes opening -- APF has not reported a size yet, there may
             * be no Xing header, and the probe has not run -- so a one-shot
             * attempt always failed on D9 even though the size arrived a
             * moment later. The readout proved it: D9 was latched at the seek
             * while Z already read 3266 KB by the time the row drew.
             *
             * Falling THROUGH rather than continuing is the whole trick: the
             * size and rate only become known BY decoding, so blocking the
             * loop to wait for them would wait forever. Bounded at five
             * seconds, after which the reason is recorded and the track just
             * plays from the start. */
            /* NOTHING may be reloading. pl_play_at() does not load the track
             * itself -- pl_arm_load() sets slot_size = 0, force_size_probe and
             * reload_armed, and the real load_track() runs later in the reload
             * handler, which finishes with stop_req and a reposition to 0:00.
             *
             * That is what D6-but-no-resume was: the seek genuinely ran, and
             * then the pending load wiped it. It also explains the earlier D9,
             * since pl_arm_load() is what zeroed slot_size in the first place.
             *
             * stop_req is included for the same reason -- it is queued
             * repositioning that would land after this one. */
            if (!idle && rate && slot_size && want < slot_size
                && !reload_pending && !reload_armed && !stop_req) {
                pcm_flush();
                refill_drain();
                file_pos  = want;
                ring_fill = 0; ring_rd = 0;
                frames = 0; min_level = 0xFFFFFFFFu;
                ui_sec = resume_at; ui_sec_acc = 0;
                ui_last_sec = 0xFFFFFFFFu; ui_prog_sec = 0xFFFFFFFFu;
                ui_last_pause = 0xFFFFFFFFu;
                /* Restart the measurement window here, exactly as a manual
                 * seek does -- carrying it across a reposition is what made
                 * meas_rate self-referential once before. */
                meas_pos0 = file_pos; meas_sec0 = ui_sec;
                if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
                ui_draw_dynamic();
                resume_dbg = 6u;                 /* actually repositioned */
                resume_seek_req = 0;
                continue;
            }
            if ((int32_t)(cycles() - resume_deadline) >= 0) {
                resume_seek_req = 0;             /* give up, and say why */
                if      (reload_pending || reload_armed || stop_req)
                                             resume_dbg = 11u;
                else if (idle)               resume_dbg = 7u;
                else if (!rate)              resume_dbg = 8u;
                else if (!slot_size)         resume_dbg = 9u;
                else                         resume_dbg = 10u;
            }
            /* still waiting -- fall through so decoding continues */
        }

        if (stop_req) {
            stop_req = 0;
            if (idle) continue;             /* nothing loaded to reposition */
            pcm_flush();
            refill_drain();
            file_pos  = audio_start;
            ring_fill = 0; ring_rd = 0;
            frames = 0; min_level = 0xFFFFFFFFu;
            ui_sec = 0; ui_sec_acc = 0;
            ui_last_sec = 0xFFFFFFFFu; ui_prog_sec = 0xFFFFFFFFu;
            ui_last_pause = 0xFFFFFFFFu;      /* repaint the transport label */
            if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
            ui_draw_dynamic();
            continue;
        }

        /* Seeking is allowed while paused or stopped -- moving through a track
         * without having to play it is the point of a transport. This sits
         * ABOVE the paused check for that reason; it used to be below it, so
         * the controls did nothing unless audio was running. */
        if (seek_req) {
            uint32_t step = ui_byte_rate() * (seek_secs ? seek_secs : 5u);
            uint32_t want;

            if (seek_req == 1u) {
                /* Forward stops short of the end. Backward has always clamped
                 * to audio_start; forward never did, so holding it walked
                 * file_pos past the end of the file, the refill came back empty
                 * and the EOF path advanced the track. Three seconds of tail is
                 * left so the end is audible and the track finishes normally. */
                want = file_pos + step;
                uint32_t rate = ui_seek_rate();
                if (slot_size > audio_start && rate) {
                    uint32_t last = slot_size - 3u * rate;
                    if (last < audio_start) last = audio_start;
                    if (want > last) want = last;
                }
                /* A forward seek may never end up BEHIND where it started.
                 *
                 * slot_size is not a constant: when a refill runs off the end,
                 * refill_pump() sets slot_size = file_pos to record the file's
                 * true extent. Seeking near the end makes the prefill do exactly
                 * that, so slot_size collapses to the current position -- and
                 * the limit computed from it lands BEHIND file_pos. The clamp
                 * then produced a backwards target, the movement guard saw a
                 * genuine change and repositioned, and slot_size shrank again
                 * on the next prefill. That is the clock walking backwards and
                 * never recovering, and why it depended on the song: it turns
                 * on where the reads first start failing.
                 *
                 * Clamping to file_pos makes the request a no-op instead, which
                 * the movement guard below then drops entirely. */
                if (want < file_pos) want = file_pos;
            } else {
                want = (file_pos > audio_start + step)
                     ? file_pos - step : audio_start;
            }

            seek_req = 0;

            /* A seek that does not MOVE must do nothing at all.
             *
             * Clamping alone was not enough: once parked at the limit, every
             * repeat still ran the whole reposition below -- pcm_flush(),
             * refill_drain(), ring_fill = 0, prefill() -- at the same offset,
             * four times a second. Playback advanced the clock between repeats
             * and each reposition snapped it back to the parked position, which
             * is the clock walking backwards; the constant re-prefill at the
             * boundary is what then fell into EOF. Holding at either limit is
             * now genuinely inert. */
            if (want != file_pos) {
                file_pos = want;
                stopped  = 0;                 /* no longer at 0:00 */

                uint32_t rate = ui_byte_rate();
                ui_sec      = rate ? (file_pos - audio_start) / rate : 0u;
                ui_sec_acc  = 0;
                ui_last_sec = 0xFFFFFFFFu;
                ui_prog_sec = 0xFFFFFFFFu;

                /* Start a fresh measurement window: the jump just moved
                 * file_pos without any time passing, and folding that into the
                 * average is what made the rate run away. */
                meas_pos0 = file_pos;
                meas_sec0 = ui_sec;

                pcm_flush();
                refill_drain();
                ring_fill = 0; ring_rd = 0;
                if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
                if (paused) { ui_draw_dynamic(); continue; }
            }
        }

        /* Nothing to decode. poll_input() and the reload handling above still
         * run, so Load MP3 / Load Playlist work from here. */
        if (idle) continue;

        if (paused) {
            st0 |= (1u << 7); REG(R_STAT0) = st0;
            /* Keep drawing. The UI update lives at the BOTTOM of this loop, so
             * simply continuing here meant PAUSED could only ever be drawn on
             * the one frame where the keypress landed mid-push. Force a
             * repaint on the TRANSITION -- ui_pause_next is left over from the
             * last pause and the cycle counter wraps, so the timer alone
             * silently skips that first draw about half the time. */
            if (!ui_was_paused || (int32_t)(cycles() - ui_pause_next) >= 0) {
                if (!ui_was_paused) {
                    ui_wave_force = 1;
                    for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                        wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
                    }
                }
                ui_was_paused = 1;
                ui_pause_next = cycles() + CLK_HZ / 30u;
                if (!ui_dump_mode) ui_draw_dynamic();
            }
            continue;
        }
        st0 &= ~(1u << 7); REG(R_STAT0) = st0;
        if (ui_was_paused) {
            ui_wave_force = 1;              /* recolour back to full on resume */
            for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
            }
            /* The FIFO drained during the pause and its output has glided to
             * zero, so the resume must ramp up from zero like any other
             * discontinuity -- see FADE_SAMPLES. */
            fade_left = FADE_SAMPLES;
        }
        ui_was_paused = 0;

        /* Advance the asynchronous refill: start one if the ring has dropped
         * to half, or collect one that has landed. Never blocks. */
        refill_pump();

        int bytesLeft = (int)(ring_fill - ring_rd);
        if (bytesLeft < 512) {
            /* End of file: everything APF says the file holds has been read
             * and the ring is drained. Repeat -- with a playlist this is where
             * it advances instead, which is why it is a soft restart. */
            if (((slot_size && file_pos >= slot_size) || eof_hit) &&
                !rd_pending && !reload_armed && !reload_pending) {
                /* With a playlist this advances; pl_advance_auto() returns 0
                 * when it deliberately did not (repeat-one, or the end of a
                 * non-repeating list), and the old replay-this-track behaviour
                 * is the fallback -- so a single file still loops as before. */
                if (pl_advance_auto()) { ui_mode_dirty = 1; continue; }
                if (pl_count && rep_mode == REP_OFF &&
                    pl_pos + 1u >= pl_count) {
                    paused |= 1u;              /* end of the list: stop here */
                    ui_toast_msg("END OF PLAYLIST");
                }
                soft_restart_req = 1;
                continue;
            }
            st0 |= (1u << 5); REG(R_STAT0) = st0;
            continue;
        }

        int off = MP3FindSyncWord(&ring[ring_rd], bytesLeft);
        if (off < 0) { ring_rd = ring_fill; continue; }
        ring_rd += (uint32_t)off;
        bytesLeft -= off;

        uint32_t lvl = pcm_level();
        if (lvl < min_level) min_level = lvl;

        unsigned char *inbuf = &ring[ring_rd];
        int before = bytesLeft;
        int err = MP3Decode(dec, &inbuf, &bytesLeft, pcm, 0);
        ring_rd += (uint32_t)(before - bytesLeft);

        if (err) {
            errs++;
            /* -2 MAINDATA_UNDERFLOW is normal for a frame or two while the bit
             * reservoir fills; only a persistent run matters. */
            if (before - bytesLeft <= 0) ring_rd++;
            continue;
        }

        /* No reason to push what we just decoded from the OLD track once we
         * know it is about to be flushed. */
        if (reload_pending) continue;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);

        /* One comparison per frame, and it decides which rate source the seek
         * arithmetic is allowed to trust. */
        if (fi.bitrate && rate_set && fi.bitrate / 8u != bytes_per_sec) vbr_seen = 1u;

        if (!rate_set && fi.samprate) {
            pcm_rate_apply(fi.samprate);
            if (fi.bitrate) bytes_per_sec = fi.bitrate / 8u;
            samprate   = fi.samprate;
            track_hz   = fi.samprate;
            track_kbps = fi.bitrate / 1000u;
            /* Exact when the file declares its frame count; otherwise the
             * size/bitrate fallback, which is only right for CBR. */
            if (track_frames && fi.nChans && fi.samprate) {
                uint32_t spf = (uint32_t)fi.outputSamps / (uint32_t)fi.nChans;
                track_secs = (uint32_t)(((uint64_t)track_frames * spf) / fi.samprate);

            }
            rate_set = 1;
            st0 |= (1u << 2); REG(R_STAT0) = st0;
        }

        /* If the FIFO ran dry, its output has glided toward zero and the
         * samples about to be pushed are mid-waveform: ramp them in. The flag
         * is sticky until the next flush, so this catches the FIRST underrun
         * of an epoch -- the net under the causes removed elsewhere, not a
         * licence to underrun. */
        if (!under_shadow && pcm_underrun()) {
            under_shadow = 1u;
            fade_left    = FADE_SAMPLES;
        }

        int n = fi.outputSamps;                  /* interleaved L,R */
        int stereo = (fi.nChans == 2);

        /* Real amplitude, not a proxy: max |sample| over the frame just
         * decoded, so the meter reflects what is actually playing. */
        {
            int32_t pk = 0, pkl = 0, pkr = 0;
            for (int i = 0; i < n; i += (stereo ? 2 : 1)) {
                int32_t l = pcm[i];        if (l < 0) l = -l;
                int32_t r = stereo ? pcm[i + 1] : l; if (r < 0) r = -r;
                if (l > pkl) pkl = l;
                if (r > pkr) pkr = r;
            }
            pk = (pkl > pkr) ? pkl : pkr;
            peak_amp = (uint32_t)pk;
            peak_l   = (uint32_t)pkl;
            peak_r   = (uint32_t)pkr;

            /* Even spread across the frame, so the trace covers the whole
             * period rather than clustering at its start. */
            uint32_t pairs = (uint32_t)(stereo ? (n / 2) : n);
            uint32_t step  = pairs / SCOPE_N;
            if (step && pk > 0) {
                /* AUTO-GAIN, normalised to this frame's peak.
                 *
                 * A fixed shift was sized for full-scale samples, and real
                 * music sits far below that -- mid/side landed a couple of
                 * pixels from centre and the trace was a smudge. Scaling to the
                 * peak makes the shape readable at any level, which is the
                 * whole point: this mode shows correlation, not loudness. The
                 * L/R bars already show loudness.
                 *
                 * One divide per frame, then a shift per point. Averages rather
                 * than sums, so mid and side each span +-pk and the reciprocal
                 * maps them exactly onto the box. */
                int32_t scale = ((int32_t)SCOPE_UNIT << 15) / (int32_t)pk;
                scope_head = (uint8_t)((scope_head + 1u) % SCOPE_HIST);
                signed char *sx = scope_x[scope_head], *sy = scope_y[scope_head];
                for (uint32_t k = 0; k < SCOPE_N; k++) {
                    uint32_t idx = k * step;
                    int32_t l = pcm[stereo ? idx * 2u : idx];
                    int32_t r = stereo ? pcm[idx * 2u + 1u] : l;
                    int32_t mid  = (l + r) / 2;      /* mono -> x = 0 */
                    int32_t side = (l - r) / 2;
                    sx[k] = (signed char)((side * scale) >> 15);
                    sy[k] = (signed char)((mid  * scale) >> 15);
                }

                /* Oscilloscope columns, same normalisation. Min and max over
                 * each slice rather than a single sample: point-sampling a
                 * waveform at 64 points aliases badly and the trace jumps
                 * about; the envelope is stable and shows the real shape. */
                uint32_t need = WAVE_COLS * WAVE_SPAN;
                if (pairs > need) {
                    /* Trigger: first rising crossing of zero, searched only in
                     * the slack between the window and the frame so there is
                     * always a full window left to draw. */
                    uint32_t trig = 0, limit = pairs - need;
                    int32_t prev = 0;
                    for (uint32_t i2 = 0; i2 < limit; i2++) {
                        int32_t l = pcm[stereo ? i2 * 2u : i2];
                        int32_t rr = stereo ? pcm[i2 * 2u + 1u] : l;
                        int32_t m = (l + rr) / 2;
                        if (prev < 0 && m >= 0) { trig = i2; break; }
                        prev = m;
                    }
                    for (uint32_t c = 0; c < WAVE_COLS; c++) {
                        uint32_t idx = trig + c * WAVE_SPAN;
                        int32_t l = pcm[stereo ? idx * 2u : idx];
                        int32_t rr = stereo ? pcm[idx * 2u + 1u] : l;
                        int32_t m = (l + rr) / 2;
                        wav_v[c] = (signed char)((m * scale) >> 15);
                    }
                }
            }
        }

        for (int i = 0; i < n; i += (stereo ? 2 : 1)) {
            int32_t l = pcm[i];
            int32_t r = stereo ? pcm[i + 1] : l;
            /* Capped at unity, so this only ever attenuates and cannot
             * overflow -- no clamp needed. */
            if (vol_gain != 256) {
                l = (l * vol_gain) >> 8;
                r = (r * vol_gain) >> 8;
            }
            /* Ramp out of a discontinuity: one shift per sample, and only
             * while the fade is live. Grows 0 -> 255/256 across FADE_SAMPLES. */
            if (fade_left) {
                int32_t g = (int32_t)((FADE_SAMPLES - fade_left) >> 3);
                l = (l * g) >> 8;
                r = (r * g) >> 8;
                fade_left--;
            }
            /* Block while the FIFO is full -- pcm_fifo silently DROPS pushes
             * when full, so skipping this corrupts the audio rather than
             * merely delaying it. Being blocked here is the healthy state.
             * This is also where the CPU spends most of its time, so input and
             * I/O are serviced from inside the wait. */
            while (PCM_FULL(REG(R_PCM_ST))) {
                poll_input();
                refill_pump();
                if (reload_pending) goto next_outer;
            }
            REG(R_AUDIO) = ((uint32_t)(uint16_t)(int16_t)r << 16)
                         | (uint32_t)(uint16_t)(int16_t)l;
        }

        frames++;
        st0 |= (1u << 3);
        REG(R_STAT0) = st0;

        if (!ui_dump_mode) ui_draw_dynamic();

next_outer: ;
    }

    return 0;
}
