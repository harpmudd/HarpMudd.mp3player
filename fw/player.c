        /* The playlist-switch fields are gone: that investigation shipped in
         * v1.2.0 and the row's width is needed for the throughput and heap
         * figures the FLAC decision turns on. pl_sw_* and tk_hist are still
         * recorded, one flag away, if it ever reopens. */
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
/* Up here, not down beside the FLAC glue where it used to sit. The diagnostic
 * row is ~800 lines ABOVE that point and reads flac_order, and C would have
 * taken the undeclared name as an error -- the same ordering trap that once
 * dropped the IO_BENCH readout with no warning at all. Nothing in these two
 * headers depends on anything in this file. */
#include <stdlib.h>                /* malloc/free -- fw/alloc.c provides them */
#include "flac.h"

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
extern unsigned int arena_limit(void);
#define ARENA_LIMIT (arena_limit())

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
#define APP_VER "1.3.0"

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
/* As fb_text_clipped, plus a hard right edge the painted CELL may not cross.
 *
 * Needed because the two limits below are genuinely different sizes: max_w
 * budgets ADVANCES, but fb_char paints a whole cell, so the final glyph can
 * reach (cell - advance) px past the budget -- 12px at TS_1X and 24px at
 * TS_2X. On the info card that ran the title through the panel's right border,
 * and left the marquee painting outside the strip it erases, so the overspill
 * was never cleaned up and accumulated as it scrolled.
 *
 * The card has 8px of padding on the left and had none on the right. Passing
 * its inner edge here makes it symmetric. */
static uint32_t fb_text_boxed(uint32_t x, uint32_t y, const char *s,
                              uint32_t sx, uint32_t sy, uint32_t max_w,
                              uint32_t paint_r)
{
    uint32_t cell  = (FONT_CELL_W * ts_half[sx]) / 2u;
    uint32_t limit = x + max_w;
    if (paint_r > FB_W) paint_r = FB_W;
    while (*s) {
        uint32_t a = fb_adv(*s, sx);
        if (x + a > limit)   break;        /* out of layout budget */
        if (x + cell > paint_r) break;     /* would paint past the box */
        fb_char(x, y, *s, sx, sy);
        x += a;
        s++;
    }
    return x;
}

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
static uint32_t fl_first_frame;   /* absolute offset of the first audio frame */
enum { FMT_MP3 = 0, FMT_FLAC };
static uint8_t  track_fmt;
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
/* Peaks ACCUMULATE between display frames instead of overwriting.
 *
 * meters_feed() runs once per decoded chunk and the bar history consumes at
 * half the display rate, so a plain assignment meant the last write before a
 * tick won and everything else was thrown away -- half the chunks on MP3, and
 * on FLAC an arbitrary half, because FLAC's chunks arrive BUNCHED.
 *
 * A FLAC frame is 4608 samples, 104 ms, and subframe() decodes all of channel
 * 0 emitting nothing -- a stereo pair cannot be reconstructed until channel 1
 * arrives. So ~39 ms of every frame produces no meter data at all, then four
 * chunks land close together. Taking the MAX since the last display frame
 * makes what the meter shows independent of when the decoder happened to
 * produce it, and loses no transient. */
static uint32_t peak_acc, peak_acc_l, peak_acc_r;
static uint8_t  peak_acc_any;

/* Meter headroom -- see where it is applied, below. Raise the numerator toward
 * the denominator for taller bars, lower it for shorter ones. This is the only
 * number to touch: it scales what every meter reads, and nothing else. */
#define MTR_HEADROOM_NUM 3u
#define MTR_HEADROOM_DEN 4u
/* The bar history scrolls every SECOND display frame, so it needs a max over
 * its own two-frame period -- reading peak_amp directly would discard the
 * frame in between, which is the same drop this change exists to remove, one
 * consumer further down. */
static uint32_t wave_pend;

enum { ID3_OK, ID3_NO_TAG, ID3_NO_FRAME, ID3_UNSUPPORTED_ENCODING };
static int title_status;
static uint8_t ui_warn_row;   /* draw the album/format row as a warning */
/* WHY a FLAC file was turned away, and the offending value. Mirrored because
 * `fl` is declared with the FLAC glue, ~2000 lines below the card that has to
 * name it.
 *
 * Four reasons, not one. Only the sample-rate gate used to say anything
 * useful; a 32-bit, multichannel or huge-blocksize file fell through to "THE
 * FILE COULD NOT BE READ", which is both wrong and alarming -- those files
 * read perfectly well, this core just cannot play them. flac_open fills
 * STREAMINFO before it rejects, so the real numbers are already in hand. */
enum { FLR_NONE = 0, FLR_RATE, FLR_DEPTH, FLR_CHANS, FLR_BLOCK };
static uint8_t  fl_reject_kind;
static uint32_t fl_reject_val;

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
static uint32_t pcm_under_n;    /* underrun EDGES since boot, for the diag  */
/* Where the CPU's second actually goes, so the hiccup can be ATTRIBUTED
 * instead of guessed at. Three outcomes, three different fixes:
 *
 *   idle high  -- the decoder finishes early and waits on a full FIFO. The
 *                 CPU is fine; a hiccup then is the ring or the SD read.
 *   io high    -- the decoder is blocked in flac_pull waiting for bytes. The
 *                 ring is too small or the reads are too slow.
 *   both ~0    -- the decoder cannot keep up. Only optimisation helps.
 *
 * Cycles, not iterations: a wait iteration and a decode iteration are not the
 * same size, and comparing counts of them would prove nothing. */
static uint32_t fl_idle_cyc, fl_io_cyc;    /* accumulating, this second     */
static uint32_t fl_rate_hz;                /* mirrors fl.rate, declared later */
static uint8_t  fl_idle_pct, fl_io_pct;
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
#define PL_MAX       256u        /* tracks; the index costs 4 bytes each */
/* The .m3u text. 8 KB made PL_MAX unreachable in practice and therefore a lie:
 * a real playlist here averages 110 bytes a line, so the buffer ran out at ~74
 * tracks while the documentation promised 128.
 *
 * RAISED TO 256 TRACKS / 20 KB for v1.3.0, keeping the two in step. 20 KB is
 * ~80 characters a line at 256 tracks -- short of the 128 the old comment
 * promised, and the honest number rather than an aspiration.
 *
 * It took three attempts to size this, because the space it grows into was
 * never free space:
 *
 *   1. Sized against the whole BSS-to-DMA gap. That gap is the HEAP. Left
 *      15680 bytes where newlib needed more, MP3InitDecoder() returned 0, and
 *      every track showed LOAD FAILED. The build passed.
 *   2. Sized against Helix's struct sum of 23816 plus a margin. Also failed,
 *      at 25232 -- newlib's own overhead was the missing term and it was
 *      never visible.
 *   3. Measured. A23824/26624 on hardware, once fw/alloc.c replaced newlib's
 *      allocator with a fixed arena. Helix's true cost is 23824, the arena is
 *      a fixed 26624, and what is left over is genuinely free.
 *
 * So: do not raise either of these against an address gap. Raise them against
 * the leftover the linker reports, and keep 1 KB for the token heap. And do
 * not raise one without the other -- that mismatch has already shipped once,
 * in the direction that made the documented cap a lie. */
/* 12 KB, down from 16, to buy back heap.
 *
 * The 256-track cap still binds first for ordinary filenames -- at 44 bytes a
 * line, which is what a flat list measures, 12 KB holds 279 and the cap stops
 * it at 256. Only playlists whose entries carry a folder path lose anything,
 * and they go from about 180 tracks to about 135. The largest list ever on
 * this card was 240 tracks in 10.6 KB, which still fits.
 *
 * Spent on heap because there was 1600 bytes of it left against a 1024-byte
 * assert -- not enough to add anything at all, including a diagnostic. This is
 * the cheapest 4 KB available and the only one that costs no code risk: no
 * function moves, no static is exported, nothing is reordered. */
#define PL_TEXT_MAX  12288u

static char     pl_text[PL_TEXT_MAX];
/* Set when the .m3u did not fit -- either the text buffer filled or PL_MAX was
 * reached with lines still to read. Without this a clipped playlist is
 * indistinguishable from a short one: the screen just shows a smaller number. */
static uint8_t  pl_truncated;
static uint16_t pl_off[PL_MAX];          /* byte offset of each name in pl_text */
static uint16_t pl_order[PL_MAX];        /* play order -> file index            */
static uint16_t pl_count;                /* 0 = no playlist loaded              */
static uint16_t pl_pos;                  /* index INTO pl_order                 */

/* Overlay state, declared HERE rather than with its drawing code:
 * ui_draw_chrome() repaints the overlay on top of itself and sits a
 * thousand lines earlier in the file. */
static uint8_t  pl_ui_open;        /* overlay is up                          */
static uint8_t  pl_ui_play_req;    /* main loop: start pl_ui_sel             */
static uint8_t  pl_ui_dirty;       /* repaint wanted                         */
static uint8_t  pl_ui_restore;     /* overlay closed: repaint the player      */
static uint16_t pl_ui_drawn_pos = 0xFFFFu;  /* pl_pos as last drawn           */
static uint16_t pl_ui_mq_off;      /* chars scrolled off the selected row     */
static uint32_t pl_ui_mq_next;     /* when it steps again                     */
static uint16_t pl_ui_mq_sel = 0xFFFFu;  /* row the scroll belongs to         */

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
 *   bits  0..6   track index, low 7 bits
 *   bits  7..23  seconds       0-131071, about 36 hours -- audiobook country
 *   bit  24      FROM PLAYLIST -- is the track index above meaningful at all
 *   bit  25      track index, high bit -- together 0-255, matching PL_MAX
 *   bits 26..30  reserved, always zero
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
/* The track index is 8 bits, but NOT contiguous: the low 7 sit where they
 * always did and the 8th lives in bit 25, the first of the reserved run. It
 * was 7 bits under a comment claiming that matched PL_MAX, which is 256 -- so
 * RS_PACK's mask silently folded track 200 of a 240-entry playlist down to
 * track 72, and resume came back at the wrong song with no sign anything was
 * wrong. Splitting the field rather than moving it keeps every previously
 * saved word readable: those have bit 25 clear, which reads back as 0..127
 * exactly as before. Bit 31 stays unset -- see the note above about APF
 * storing these as signed. */
#define RS_TRACK(w)   (((w) & 0x7Fu) | ((((w) >> 25) & 1u) << 7))
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
/* 1.2x speed needs a LONGER hold than everything else.
 *
 * It used to share PL_HOLD_MS with the scrub and the art panel, and 400 ms is
 * short enough to reach by lingering on the play button -- users were ending up
 * in 1.2x without knowing they had done anything, with only the music sounding
 * fast to tell them. Two and a half times the length makes it a gesture rather
 * than a slip, and the on-screen indicator covers the case where it happens
 * anyway. Started at 1200 ms and came down to 1000 on the only authority that
 * counts for a hold, which is a thumb.
 *
 * A modifier was tried instead and does not work here: holding Select is
 * already the album-art panel, so Select+A fights a binding that fires at the
 * same threshold. */
#define SPEED_HOLD_MS 1000u              /* A held this long = 1.2x toggle    */

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
/* The name in FULL, untruncated.
 *
 * pl_name_raw is 24 bytes because that is all the header line can show, and
 * for a long while that was all the core kept -- which is why only twelve
 * characters of a playlist's name could ever be remembered. The datatable
 * hands pl_name_read() the whole path in a 1024-byte buffer, so the
 * information was never missing, only thrown away. */
#define PL_FULL_MAX 96u
static char pl_name_full[PL_FULL_MAX + 1u];
/* Fingerprint of the .m3u TEXT last parsed, so "did the slot actually switch"
 * can be answered from the bytes rather than from the name APF reports. Zero
 * when nothing has parsed. */
static uint32_t pl_sig;
/* The remembered list is reopened at BOOT only. pl_load() also runs whenever
 * the user picks a playlist, and restoring there would override the pick. */
static uint8_t pl_restore_pending = 1u;

/* Gate for a playlist-slot reload, mirroring the one the MP3 slot already has.
 * 008A means the user PICKED a file, not that the slot is readable yet. */
/* Belt and braces for a notification that never arrives.
 *
 * The user reproduced a pick that produced NOTHING -- no LOADING message, no
 * load, however long they waited -- on a build where the transport row can no
 * longer paint over that message. So 008A for slot 3 had not reached firmware
 * at all, and nothing downstream of it can help: every retry, gate and cache
 * flush added so far is waiting on an event that is not coming.
 *
 * So stop depending on it being delivered. The pick can only happen in the
 * Analogue menu, and the core is told when that closes -- so ASK the slot what
 * it holds at exactly that moment. One 0190 per menu close, at a point where
 * playback is already interrupted, against a notification that is sometimes
 * simply absent. */
/* paused is a bitmask: 1 the user, 2 the OS menu, 4 a playlist switch in
 * progress. The switch bit exists for two reasons, and the second is probably
 * the bigger one:
 *
 *   - it takes the core's 0180 refill traffic off the bus while APF is trying
 *     to reassign slot 3, which is the collision the user suspects is behind
 *     the occasional pick that does nothing;
 *   - the old track used to keep playing throughout the wait, which can run to
 *     five seconds. Music continuing while the screen says LOADING PLAYLIST
 *     reads as NOTHING HAPPENING, and that is what makes a user pick again.
 *     Silence reads as working.
 *
 * load_track() ends with paused = 0, so a successful switch clears it; the
 * completion branch clears it too, for the paths that never get that far. */
#define PAUSE_LOAD 4u
static uint8_t  menu_was;         /* the OS menu was open on the last poll      */
static uint32_t pl_poll_at;       /* next periodic slot-3 identity check         */
static uint8_t  pl_check_req;     /* menu just closed: ask slot 3 what it holds */
static uint8_t  pl_skip_gate;     /* 0190 already proved it changed             */
static uint32_t pl_fb_at;         /* when the fallback last loaded              */
static char     pl_cur_name[24];  /* the playlist that is actually loaded       */
static uint16_t pl_notify_n;      /* playlist notifications seen from the RTL */
static uint16_t pl_load_n;        /* times pl_load() actually ran             */
static uint32_t pl_reload_seen;   /* OR of every R_RELOAD word observed       */
/* Last playlist SWITCH, recorded so the failure can be read off the screen
 * instead of reasoned about. Two blind fixes have already been wrong about
 * this one; these five fields separate every remaining explanation.
 *   G  1 the gate saw the name change, 2 it gave up and fired on the cap
 *   R  0 no retry, 1 retried on an unchanged name, 2 on unchanged CONTENT
 *   P  1 pl_play_at(0) ran, 0 it was skipped
 *   F  reload_pending | reload_armed<<1 at the moment P was decided
 *   T  pl_count after the load
 * L not advancing with N means the gate never fired at all; L advancing with
 * P zero means the list loaded and playback simply was not taken, which no
 * amount of retrying the READ would ever fix. */
static uint8_t  pl_sw_ge, pl_sw_rt, pl_sw_pp, pl_sw_fl;
static uint16_t pl_sw_ct;
/* ...and the last THREE of them, because a success overwrites the failure.
 * The first capture showed N8 L8 G1 R0 P1 F0 T9 -- a perfectly healthy
 * switch, which is exactly what the screen shows AFTER the attempt that
 * finally works. Keeping a history means one photograph taken after a triple
 * shows all three attempts, and whether the failures failed the same way.
 * Packed G,R,P,F as hex nibbles, oldest on the left. */
static uint16_t pl_sw_hist[3];
/* Times load_track() came back empty. The playlist path looks healthy, so
 * the next suspect is downstream: the list switches, track 1 is requested,
 * and the TRACK open is what does not land -- which would leave the old song
 * playing and read as "the playlist did not switch". */
static uint16_t pl_sw_tk;
/* The TRACK loads that follow a switch, three deep. The playlist side has now
 * read healthy three times running -- gate confirmed, no retry, playback
 * taken, right list -- so the failure is downstream of it, and X says
 * load_track() never came back empty. But opening SUCCESSFULLY is not the
 * same as opening the RIGHT file: a stale MP3 slot would reopen the previous
 * track, which sounds exactly like the playlist never switching.
 *   G  1 the gate confirmed a new file id, 2 it fired on the cap
 *   O  1 load_track() returned a track, 0 it did not
 *   C  1 the filename CHANGED, 0 the SAME file was reopened
 * C zero is the whole hypothesis. */
static uint16_t tk_hist[3];
static uint32_t tk_prev_name;     /* FNV of track_file before the load */
static uint8_t  pl_reload_armed;
static uint32_t pl_reload_at;        /* hard deadline: act regardless */
static uint32_t pl_probe_at;         /* next slot-changed check */
static char     pl_leaving[24];      /* the name we are switching AWAY from */
static uint8_t  pl_retry;            /* one re-arm if the switch did not take */

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
static char     pl_saved_stem[PL_STEM_MAX + 1u];

/* ------------------------------------------------- remembering a long name
 *
 * Twelve characters is all a settings word can hold, and widening it cannot
 * fix this: eleven of sixteen slots are used, so spending every remaining one
 * reaches thirty-two characters, and "Crash Test Dummies - God Shuffled His
 * Feet.m3u" is forty-two. Even six-bit packing, which would cost the name its
 * case and so its ability to be reopened on anything but FAT, stops at forty.
 * The maximum-cost version of the obvious fix still fails on an ordinary
 * album name.
 *
 * So the name is not stored. A HASH of it is, and the name itself lives in a
 * plain list on the card -- playlists.m3u -- which the core reads at boot and
 * searches for a matching hash. Length stops mattering entirely.
 *
 * A hash rather than a line number: a line number is only correct until the
 * file is edited, and the failure mode of a stale one is loading the WRONG
 * album silently. A hash survives reordering, insertion and deletion, and it
 * is computable at the moment the user picks a list -- where reading a file
 * would mean clobbering the playlist that was just loaded into pl_text.
 *
 * The twelve-character stem is still stored and still works on its own, so a
 * card with no playlists.m3u behaves exactly as before. */
static uint32_t pl_saved_hash;

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
static int      slot_changed(void);    /* defined with the 0190 helpers      */
static uint32_t tk_poll_at;            /* next track-slot identity check     */
static uint32_t force_size_probe;      /* R_SLOT_SZ is stale: measure instead */
static uint8_t  seek_size_tried;       /* backstop size probe already attempted */
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
/* TEMPORARY, with IO_BENCH: shows the throughput figure. Back to 0 before
 * anything ships -- no diagnostic is ever shown to users. */
/* 0 for any build a user sees -- the standing rule is that no diagnostic ever
 * reaches one. Set to 1 to bring the D/O/U/F row back while investigating. */
#define UI_SHOW_SPEED_DIAG 0
/* Resume instrumentation. EXTRA_CFLAGS="-DUI_SHOW_RESUME_DIAG=1 -Os"
 *
 * The -Os is not optional: the normal build has under 1 KB of heap left and
 * will not link with another diagnostic row in it.
 *
 *   Y  resume_dbg   1 armed         2 past the end of the track
 *                   3 not applicable, or the point is under 2 s
 *                   4 not from a playlist        6 REPOSITIONED
 *                   7 idle at deadline           8 no byte rate
 *                   9 no slot_size              10 other at deadline
 *                  11 reload or stop pending at deadline
 *   N  resume_saves points published this session. The saver holds off while
 *                  a resume is pending, so 0 early on is expected.
 *   A  resume_at   the second being aimed at   S ui_sec   T pl_pos
 *
 * This row corrected me twice in one session and both times I was about to fix
 * the wrong half: Y3 A1 was the restore correctly declining a 1 s point, and
 * two small saved values looked like a broken saver that was working fine. It
 * stays. */
#ifndef UI_SHOW_RESUME_DIAG
#define UI_SHOW_RESUME_DIAG 0
#endif
/* Seek instrumentation. Build with EXTRA_CFLAGS=-DUI_SHOW_SEEK_DIAG=1.
 *
 * This build CHANGES NO BEHAVIOUR. Two reasoned fixes for the post-seek clock
 * both failed on hardware, and both were backed by offline models that said
 * they would work -- the models were of the wrong thing twice over. So this
 * only shows what a correction WOULD have computed, while seeking keeps
 * working exactly as it does today. Never shipped: the flag defaults to 0. */
#ifndef UI_SHOW_SEEK_DIAG
#define UI_SHOW_SEEK_DIAG 0
#endif
#if UI_SHOW_SEEK_DIAG
static uint32_t dg_tgt, dg_at, dg_pos, dg_ui, dg_blk, dg_rate;
/* The load-path fields. Copied rather than read where the rows are drawn:
 * ui_draw_dynamic() sits above every FLAC declaration in this file. */
static uint32_t dg_size, dg_dur, dg_first, dg_pts, dg_len, dg_intent;
/* Probe telemetry. Distinguishes "the search ran and could not get close"
 * from "no probe ever came back", which are different faults with different
 * fixes -- and the second is what a file opened BY NAME rather than mounted
 * as a sized slot would produce if random access behaves differently there.
 * Playback only ever reads forward, so it would never reveal that. */
static uint8_t  dg_pn, dg_pfail, dg_prej;
static uint32_t dg_num[3];
static uint8_t  dg_n, dg_samp, dg_fe, dg_live;
#endif

/* Load-phase breakdown as a toast after every load: H head, S size probe,
 * A artwork, P prefill, T total, in ms. 1 only while investigating load time;
 * it must be 0 in anything a user sees.
 *
 * #ifndef so it can be turned on WITHOUT editing this file:
 *     EXTRA_CFLAGS=-DUI_SHOW_LOAD_TIMES=1 bash fw/build.sh
 * which is the whole point of a measurement flag -- an investigation should
 * not need a source edit that then has to be remembered and reverted. */
#ifndef UI_SHOW_LOAD_TIMES
#define UI_SHOW_LOAD_TIMES 0
#endif

/* ---- TEMPORARY: sequential SD throughput benchmark ------------------------
 *
 * The ONE measurement the FLAC decision turns on. Everything else about FLAC
 * is understood; whether the card can sustain ~112 KB/s is not, and 40 KB/s at
 * MP3's 320 kbps is the most this core has ever had to hold.
 *
 * It has to be a BURST, not an observation of normal playback: during playback
 * the refill rate is limited by DEMAND, so timing it would measure the bitrate
 * of the file and report it as a capacity figure. This reads N chunks
 * back-to-back with no decoding in between, which is the ceiling.
 *
 * Sequential on purpose. The ~480 ms the old size probe spent on ~20 reads is
 * not representative -- those were random offsets that made APF re-walk the
 * cluster chain each time.
 *
 * Lands in the tag scratch buffer, never the ring, so it cannot disturb audio.
 * Runs ONCE a session. MUST go back to 0 before shipping. */
#define IO_BENCH 0
/* Defined HERE, above every user. It lived next to REFILL_CHUNK, ~90 lines
 * BELOW the diagnostic row that tests it -- so `#if IO_BENCH` in the row saw
 * an undefined name, evaluated it as 0, and dropped the readout with no
 * warning. The benchmark ran; its result was simply never printed. */
static uint16_t io_kbps;          /* measured sustained sequential read rate */
static uint32_t io_bench_bytes;   /* ...and how much it managed to read      */

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
/* Right edge a painted glyph CELL may not cross on the info card. The card
 * spans UI_MARGIN-8 .. UI_MARGIN-8+UI_INNER_W+16, so this leaves 8px of
 * padding on the right to match the 8px already on the left. */
#define UI_CARD_TEXT_R (UI_MARGIN + UI_INNER_W)

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
/* APPEND ONLY -- never reorder, never insert.
 *
 * viz_mode is persisted as an INDEX, so moving an entry silently repoints
 * every user's saved meter at a different one. Same rule as the interact.json
 * ids. New modes go immediately before VIZ_COUNT. */
enum { VIZ_BARS = 0, VIZ_WATER, VIZ_LEVELS, VIZ_SCOPE, VIZ_WAVE, VIZ_VU,
       VIZ_SCROLL, VIZ_MIRROR, VIZ_DOTS, VIZ_EYE,
       /* APPENDED, and it must stay that way: viz_mode persists as an INDEX,
        * so inserting a meter anywhere but the end silently repoints every
        * saved preference at a different one. Adding this also required the
        * Meter slider's max in interact.json to go from 9 to 10 -- without
        * that the setting stops persisting and the firmware looks correct
        * throughout while doing it. */
       VIZ_LED,
       VIZ_COUNT };

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

/* ---- Magic eye (EM84 indicator tube) ----------------------------------
 *
 * The BAR type: two glass tubes side by side, each with a vertical
 * fluorescent strip whose LENGTH follows its channel.
 *
 * The first attempt was a rounded rect with a solid bar in it and read as
 * generic, correctly -- that describes a bar meter, and this screen has
 * two of those already. What makes a tube look like a tube is not its
 * outline, it is the GLASS and the BLOOM:
 *
 *   - the envelope is shaded PER COLUMN, bright near the left and falling
 *     off both ways, so it reads as a cylinder rather than a slab;
 *   - the top is DOMED, by a per-column offset off the same circle search
 *     fb_round_rect uses, with a pip above it;
 *   - a getter flash sits inside the crown, the silvery patch every real
 *     tube carries;
 *   - the phosphor has a bright core, two halo layers either side, and a
 *     bloom that spills ABOVE the tip -- a hard-edged bar is the single
 *     biggest reason a glow reads as a rectangle;
 *   - the glow reflects in the base plate, the way the photographed pair
 *     reflects in its acrylic.
 *
 * Nearly all of that is in the CACHED pass, so the per-frame cost is the
 * strip and its reflection -- about eleven rects a channel.
 *
 * CYAN-GREEN, not the accent. Everything else here is a tone of ui_accent
 * on purpose (see the VU face); this breaks it knowingly, because the glow
 * IS the instrument. The base plate is accent-tinted instead -- a chassis
 * can follow the theme where the phosphor cannot. */
#define EYE_GLASS_D 0x18E4u       /* envelope, in shadow                   */
#define EYE_GLASS_L 0x530Du       /* envelope, on the specular streak      */
#define EYE_GETTER  0x6BD0u       /* getter flash inside the crown         */
#define EYE_SOCKET  0x1082u       /* base of the envelope, in the socket   */
/* The scale ticks and the plinth are the ONLY parts of this meter that follow
 * the accent, and they have to, for the reason written on the VU face: with a
 * fixed palette throughout, cycling the colour moved nothing and the meter
 * looked broken. The phosphor cannot take the accent -- the glow is the
 * instrument -- so the etched scale carries it instead. */
#define EYE_DARK    0x0082u       /* strip window, unexcited               */
#define EYE_H2      0x0A89u       /* outer halo                            */
#define EYE_H1      0x1DC3u       /* inner halo                            */
#define EYE_LIT     0x57FCu       /* the phosphor itself                   */
#define EYE_TUBE_W  38u
#define EYE_TUBE_G  10u           /* gap between the pair                  */
#define EYE_BAR_W    6u           /* the strip core; halos add 4 each side */
#define EYE_DOME     9u           /* rows the crown curves through         */
/* Sized to the last pixel of the meter box: pip on row 0, plinth ending on
 * row 71 of 72. Growing either dimension again means taking it from the
 * plinth or the dome, not from spare space -- there is none. */
#define EYE_TUBE_H  62u
/* The plinth. Overlaps the tube's last row on purpose -- those rows are socket
 * shadow, so the glass reads as seated IN the base rather than balanced on it,
 * which is how the photographed pair sits. Wider than the tubes now that the
 * glow stops above it. */
#define EYE_BASE_H   8u
#define EYE_BASE_PAD 8u           /* overhang each side                     */
static uint32_t eye_l, eye_r;     /* Q8 deflection, 0..255                 */
static uint8_t  eye_face;         /* envelopes and dark strips are drawn   */
static uint16_t eye_face_w;       /* ...and the width they were drawn for  */
static uint8_t  eye_shown_l, eye_shown_r;  /* strip height currently lit   */

/* Light thrown onto the panel beside each tube. The pair occupies 78 px of a
 * box nearly four times that, and spill is what a bright tube in a dark case
 * does with the space.
 *
 * Anchored to the MIDDLE of the tube, not to the tip of its strip. Tracking
 * the tip was the first attempt and looked wrong for a reason worth keeping:
 * the strip grows upward from the bottom, so at low level the tip -- and with
 * it the whole pool of light -- sat down at the tube's base, as though the
 * glow came from the socket. A tube lights the room from where the tube is.
 *
 * Elliptical falloff, so it reads as a pool rather than a wedge, and it
 * reaches further out than the cone did.
 *
 * Driven by a SEPARATE, heavily smoothed level rather than by the strip.
 * Light in a room does not snap, and this is a wash behind an instrument that
 * is already showing the fast movement -- the smoothing is what makes it
 * atmosphere instead of a second meter. It also pays for itself: quantised to
 * a few steps, most frames leave it alone entirely.
 *
 * Self-erasing: bands past the current reach are painted with the background
 * itself, so there is no separate erase pass. Quantised into 4-row strips to
 * keep the rect count down; the ramp moves well under one level across four
 * rows, so stepping it there is not visible where a flat fill was. */
#define EYE_GLOW_RX    72u        /* how far the light reaches outward    */
#define EYE_GLOW_RY    20u        /* ...and half how tall the pool is     */
#define EYE_GLOW_NB     9u        /* bands across that reach -- 8px each  */
#define EYE_GLOW_S      4u        /* rows per quantised strip             */
#define EYE_GLOW_STEPS  6u        /* intensity steps                      */
#define EYE_GLOW_PEAK  26u        /* strongest mix, out of 64             */
#define EYE_GLOW_POS    8u        /* vertical positions across the travel */
/* The band the pool is repainted over, FIXED, covering every position it can
 * take. It has to be: the pool moves, and a redraw that only covered its own
 * span left up to 54 rows of the previous position on screen -- seen as a
 * faint line above the light. Self-erasing only works if the repainted area
 * does not move, so the area is the union and the rest is painted bed. */
#define EYE_GLOW_TOP   16u        /* first row of that band, from the box  */
/* Spans the whole box. Shortening it to clear the plinth was tried and looked
 * worse: the pools reach 72px past each tube while the base is 102px wide, so
 * the light was chopped flat in mid-air either side of it rather than stopping
 * at anything. On the base's OWN rows the pools skip its 8px of overhang
 * instead, which is the only part they would otherwise paint over. */
#define EYE_GLOW_ROWS  56u        /* rows 16..71                            */
/* The GAP between the tubes is lit by BOTH of them, and leaving it dark was
 * the one place the illusion broke: two lamps 10px apart cannot leave the
 * space between them the darkest thing on the panel.
 *
 * It is a separate pass because it depends on both channels at once, where
 * everything else here is per tube. Cheap -- one rect per row-strip, because
 * across 10px the two falloffs very nearly cancel and the sum is flat, so a
 * gradient across it would be invisible.
 *
 * It stops ABOVE the plinth. The gap sits over the middle of the base plate,
 * and painting the bed there would chew a notch out of it -- the same fault
 * the overhanging plinth had at its ends. */
#define EYE_GAP_ROWS   48u        /* rows of the gap that are lit          */
/* POSITION and BRIGHTNESS come from different places, and that split is the
 * point.
 *
 * The pool sits on the MIDDLE OF THE LIT STRIP, taken from the same fast
 * value the strip itself is drawn from, so the two cannot drift apart --
 * driving the position from the slow follower is what made the light lag
 * visibly behind the bars. As the strip grows upward its midpoint rises, so
 * the light rises with it; at rest it sits low, where the lit part actually
 * is. That is also the earlier "it comes from the socket" complaint answered
 * properly: the light was following the TIP, which is the one part of the
 * strip that is nowhere near the middle of the glow.
 *
 * Brightness keeps the slow follower. Light in a room does not snap, and this
 * is a wash behind an instrument already showing the fast movement.
 *
 * Both are quantised -- eight positions, six intensities -- so a redraw costs
 * only when one of them actually steps. */
static uint32_t eye_gl_l, eye_gl_r;        /* slow-smoothed level         */
static uint8_t  eye_cast_l = 0xFFu, eye_cast_r = 0xFFu;   /* step drawn   */
static uint8_t  eye_gap_l  = 0xFFu, eye_gap_r  = 0xFFu;   /* ...for the gap */

/* Half-width of the glow ellipse at a given distance from its centre row.
 * Three callers now -- both pools and the gap -- so it stops being copied. */
static uint32_t eye_glow_rx(uint32_t dy)
{
    if (dy >= EYE_GLOW_RY) return 0;
    uint32_t q = 0;
    while ((q + 1u) * (q + 1u) + dy * dy
           <= EYE_GLOW_RY * EYE_GLOW_RY) q++;
    return (EYE_GLOW_RX * q) / EYE_GLOW_RY;
}



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

/* ---- LED LADDER -------------------------------------------------------
 * Two channels, nine rows, three blocks across each row.
 *
 * The row count is vertical so it does not change with the panel, but the
 * WIDTH does -- 360 px with the art hidden, 252 with it showing. One block per
 * row would be 25:1 hidden, a stack of thin lines rather than LEDs; splitting
 * each row three ways gives 7.9:1 and 5.3:1, so it reads as the same
 * instrument either way. True square LEDs across a 176 px column would need
 * roughly 340 rects a frame, which is not worth it on the audio budget; this
 * costs 54.
 *
 * Nine rows also divides the 72 px band exactly at 7 px plus a 1 px gap, and
 * gives 11% steps -- with the 3/4 meter headroom, real music sits at 4..7 of
 * 9 and never pegs. */
#define LED_ROWS 12u
#define LED_GAPV 1u
#define LED_BLKH (UI_WAVE_H / LED_ROWS - LED_GAPV)     /* 5 px */
#define SPEC_GAPX 3u                                   /* between columns */

/* ---- OCTAVE FILTER BANK -----------------------------------------------
 *
 * Eight bands of real frequency content, so the columns move independently and
 * against each other instead of all reporting one loudness.
 *
 * NOT an FFT, and not a bank of parallel band-passes -- both are far too
 * expensive here. Costed against one 26 ms meter window at 44.1 kHz:
 *
 *     FFT, 1024-point                  13.1% CPU
 *     12 parallel biquad bands         31.9%
 *     the same, subsampled 1-in-4       8.0%
 *     OCTAVE cascade, 8 bands           1.5%
 *
 * The cascade is cheap because each stage runs at HALF the rate of the one
 * before it. A one-pole low-pass splits the signal in two: what it rejects is
 * that stage's band, and what it passes is halved in rate and handed down. The
 * whole ladder costs about twice the first stage, not eight times.
 *
 * `lp += (x - lp) >> SPEC_SH` is that filter -- a subtract, a shift, an add.
 * The bands land at roughly 3.5k+, 1.7k, 880, 440, 220, 110, 55 and below,
 * which is octave spacing and what a spectrum display is meant to show.
 *
 * RUN ONLY WHILE THIS METER IS ON SCREEN. That is the whole safety argument:
 * 24-bit FLAC measures ~0% idle CPU, and the roadmap has long flagged a filter
 * bank as the one addition that could bring audio tics back. Gated on
 * viz_mode, the cost exists only while it is being looked at, and switching
 * meters is an instant way out. */
#define SPEC_OCT   8u                      /* cascade stages = octaves      */
#define SPEC_BANDS (SPEC_OCT * 2u)         /* each octave split in half     */
#define SPEC_SH    1u                      /* octave split                  */
#define SPEC_SH2   2u                      /* the half-octave split within  */

static int32_t  spec_lp[SPEC_OCT];        /* the cascade's filter state      */
static int32_t  spec_slp[SPEC_OCT];       /* the half-octave splitter        */
static uint32_t spec_cnt[SPEC_OCT];       /* per-stage rate dividers         */
static uint32_t spec_acc[SPEC_BANDS];     /* |band| summed over the window   */
static uint32_t spec_n;                   /* samples in the window           */
static unsigned char spec_lvl[SPEC_BANDS];    /* published, 0..255           */
/* Last drawn as a ROW COUNT, not as a level.
 *
 * This is what stopped the flicker. Comparing the 0..255 level means something
 * differs on virtually every update -- 255 steps against 12 rows -- so all 96
 * blocks were repainted continuously, overdrawing colours that had not
 * changed. Comparing rows means a band only redraws when it crosses a
 * boundary, and only the rows between the old count and the new get touched:
 * typically one or two rects instead of ninety-six.
 *
 * 0xFF is the sentinel for "the chrome repainted underneath you", as
 * everywhere else here. */
static unsigned char spec_drawn[SPEC_BANDS];

/* Per-band gain, Q4, low band first.
 *
 * MEASURED, not chosen. The first table was set by eye and every value was
 * roughly twenty times too small -- band means come out at 250..1950 in sample
 * units, which those gains turned into a `v` of 4..13 out of 255, so the meter
 * lit its bottom row and nothing else.
 *
 * tools/host/spectrum_harness.c runs this exact cascade over real music
 * decoded by the real fw/flac.c and reports each band's mean magnitude. Two
 * tracks, averaged, with each band's gain set to put that average around 150
 * of 255 -- roughly seven of twelve rows, high enough to see and with room to
 * move in both directions:
 *
 *     band        0    1    2    3    4    5    6    7
 *     mean      412  644  937 1272 1571 1684 1398  894
 *
 * uint16_t, not unsigned char: the top band needs 749 and the old type
 * silently caps at 255, which would have quietly flattened the treble end
 * while looking like a working table. */
/* Level to a logarithmic scale: 16 units per octave, so one unit is about
 * 0.4 dB. A loop rather than a clz builtin -- it runs sixteen times per meter
 * update, not per sample, so the cost is nothing and it cannot surprise the
 * linker. */
static uint32_t spec_log(uint32_t v)
{
    if (!v) return 0;
    uint32_t e = 0u, t = v;
    while (t > 1u) { t >>= 1; e++; }
    uint32_t m = (e >= 4u) ? ((v >> (e - 4u)) & 0xFu) : ((v << (4u - e)) & 0xFu);
    return (e << 4) | m;
}

/* The display window, in spec_log units.
 *
 * 30 dB of range killed the pegging but spent too much of the meter doing it:
 * at 80 units a 6 dB swing moved the bars two rows, so everything sat mid-scale
 * and the meter stopped reacting to the music. 56 units is 21 dB, which is
 * three rows per 6 dB -- half again as much movement.
 *
 * What that costs is headroom, and the number is worth writing down: measured
 * across both tracks the loudest band reaches 238, so the top of this window
 * sits 14 units -- about 5 dB -- above it. A master 5 dB hotter than these will
 * touch the top again. On the linear scale it was 1.5 dB, so the pegging that
 * started this is still fixed; there is just no free lunch between the two.
 *
 * Narrow the span for more movement, widen it for more headroom. Those are the
 * only two things this trades. */
#define SPEC_FLOOR 196u
#define SPEC_SPAN   56u

static const uint16_t spec_gain[SPEC_BANDS] = {
    /* MEASURED for the HALF-OCTAVE cascade, low band first. Re-measured rather
     * than carried over: splitting each octave in two changes every level, and
     * the two halves are not equal -- the upper half of an octave carries less
     * than the lower in most music, so a table that reused the octave figures
     * would have leant the whole display one way.
     *
     * Two tracks averaged, each band set to land near 150 of 255. Predicted
     * from those same measurements: the busy track lights 6..11 rows across
     * the sixteen, the sparse one 2..7. */
    1428u, 411u, 788u, 266u, 506u, 228u, 411u, 255u,
    430u, 345u, 521u, 496u, 699u, 764u, 1010u, 1312u
};

/* The ladder's own palette, RGB565. Green low, amber through the middle, red
 * at the top -- fixed rather than accent-derived, because on this meter the
 * colour is information. */
#define LED_LO   0x0600u      /* green  */
#define LED_MIDC 0xFE60u      /* amber  */
#define LED_HI   0xF9C0u      /* red    */

/* Last drawn, so a still passage costs nothing. 0xFF is the sentinel every
 * other meter here uses for "the chrome repainted underneath you". */

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
static uint32_t art_sig;          /* fingerprint of the IMAGE the stash holds  */
/* An image already known to be undecodable. Without this, a cover the core
 * cannot read is re-attempted on every track of the album -- and, worse, the
 * message explaining why appears on every one of them. */
static uint32_t art_bad_sig;
static uint8_t  art_bad;          /* a cover IS present and cannot be decoded */
static uint8_t  art_bad_code;     /* which picojpeg complaint, for the label  */

/* The panel is shown for a cover that exists but cannot be read, as well as
 * for one that decoded. Those two are worth telling apart and were not:
 * has_art went to 0 either way, art_shown followed it, and the panel vanished
 * entirely -- so a file WITH artwork the core cannot read looked exactly like
 * a file with none. That is why "no art, and no message either" was the
 * report: there was no frame to put a message in. */
#define ART_PANEL_WANTED (art_have || art_bad)
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
/* Set where ui_grad_set() can reach it; the stash itself is built further
 * down, once UI_WAVE_* are in scope. */
static uint8_t ui_bg_ready;

static void ui_grad_set(uint16_t accent)
{
    ui_bg_ready = 0;            /* the stashed background is now stale */
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

/* Same shape, but with the corners cut to a colour the caller names.
 *
 * fb_round_rect above cuts to ui_grad_at(), which is right for anything
 * sitting directly on the background and wrong for anything sitting on a
 * PANEL -- the corners would be four holes showing the gradient through it.
 * The playlist overlay is the case: a rounded row inside a rounded panel. */
static void fb_round_rect_on(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t r, uint16_t color, uint16_t bg)
{
    fb_rect(x, y, w, h, color);
    for (uint32_t i = 0; i < r; i++) {
        uint32_t dy = r - i;
        uint32_t inner = 0;
        while ((inner + 1u) * (inner + 1u) + dy * dy <= r * r) inner++;
        uint32_t cut = r - inner;
        if (!cut) continue;
        fb_rect(x, y + i, cut, 1, bg);
        fb_rect(x + w - cut, y + i, cut, 1, bg);
        fb_rect(x, y + h - 1u - i, cut, 1, bg);
        fb_rect(x + w - cut, y + h - 1u - i, cut, 1, bg);
    }
}

/* The waveform gives up its right-hand end to the art panel, which occupies
 * the same rows. */
static uint32_t ui_wave_w(void)
{
    return art_shown ? (ART_X - UI_MARGIN - 4u) : UI_INNER_W;
}

/* A copy of the meter box's BACKGROUND, parked in the columns the scanout
 * never reads: the framebuffer's stride is 512 and only 400 is displayed, so
 * x 400..511 exists on every row and is invisible.
 *
 * Every meter erases part of the box before redrawing its content, and they
 * all did it with `bed` -- the gradient sampled once at the box's top row.
 * That is a flat slab on a ramp that falls to 62% of that value by the bottom
 * of the box, which is what showed behind the magic eye and the VU needles.
 *
 * Per-row erases would be correct and unaffordable: the mirrored bars and the
 * peak dots erase a full-height column PER BAR, 36 times a frame, so 72 rows
 * each is 2592 commands. Copying from a prepared strip is ONE command per
 * erase -- exactly what they cost today.
 *
 * Built lazily and dropped whenever the gradient changes. */
#define UI_BG_X  FB_W                  /* first off-screen column */
#define UI_BG_W  (FB_STRIDE - FB_W)    /* 112 px of invisible stride */

static void ui_bg_restore(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!w || !h) return;
    if (!ui_bg_ready) {
        for (uint32_t yy = UI_WAVE_Y; yy < UI_WAVE_Y + UI_WAVE_H; yy++)
            fb_rect(UI_BG_X, yy, UI_BG_W, 1, ui_grad_at(yy));
        ui_bg_ready = 1;
    }
    /* Source and destination share rows, so the ramp lines up by
     * construction and the copy is purely horizontal. */
    while (w) {
        uint32_t n = (w < UI_BG_W) ? w : UI_BG_W;
        fb_copy(UI_BG_X, y, x, y, n, h);
        x += n; w -= n;
    }
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

/* Every meter that CACHES part of itself on screen has to be listed here, and
 * anything that erases the meter band must call this.
 *
 * Twice now a cached face has been wiped and never come back while the moving
 * part carried on repainting itself: the VU's arc when the album art slid over
 * it, and the magic eye's envelope on a track change. Both were one missing
 * assignment at a site that already cleared the OTHER meter. A list of one
 * invites that; a list with a name is at least the place to look. */
static void ui_meter_faces_invalidate(void)
{
    vu_face  = 0;
    eye_face = 0;
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
    ui_meter_faces_invalidate();

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
#if UI_SHOW_SEEK_DIAG
/* Suppressed in the seek-diagnostic build: the toast band sits on diag row A,
 * and a toast landing mid-reading would corrupt the one thing this build
 * exists to show. */
static void ui_toast_msg(const char *msg) { (void)msg; }
#else
static void ui_toast_msg(const char *msg) { ui_toast_set(msg, 0xFFFFFFFFu, 0); }
#endif


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

/* The same frame, captioned, for a cover that is present and unreadable.
 *
 * Two short lines because the panel is 92 px wide and the words are not:
 * "PROGRESSIVE" alone is 121 px. "PROG." and "JPEG" are 54 and 44, which fit
 * with room to centre them. Terse, but it is the difference between "this
 * player is broken" and "this file's cover is in a format it cannot read",
 * and the toast carries the full sentence for anyone who catches it.
 *
 * Persistent, unlike the toast. That is the point: a cover being unreadable is
 * a standing fact about the file, not an event. */
static void ui_art_reason(int progressive)
{
    /* A boolean, not the picojpeg status: this lives well above the point
     * where art.inc pulls picojpeg.h in, so the enum is not in scope here.
     * The caller does the comparison, where it is. */
    const char *a = progressive ? "PROG." : "COVER";
    const char *b = progressive ? "JPEG"  : "ERROR";
    ui_art_placeholder();
    fb_set_color(UI_FAINT, UI_TRACK);
    uint32_t wa = fb_text_width(a, TS_1X), wb = fb_text_width(b, TS_1X);
    uint32_t cx = ART_PAD + ART_IMG / 2u;
    uint32_t cy = ART_STASH_Y + ART_PAD + ART_IMG / 2u;
    fb_text_clipped(cx - wa / 2u, cy - FB_CELL(TS_1X), a, TS_1X, TS_1X, wa + 2u);
    fb_text_clipped(cx - wb / 2u, cy + 2u,             b, TS_1X, TS_1X, wb + 2u);
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
    ui_meter_faces_invalidate();

    for (uint32_t y = UI_WAVE_Y; y < UI_WAVE_Y + UI_WAVE_H && y < FB_H; y++)
        fb_rect(UI_MARGIN, y, UI_INNER_W, 1, ui_grad_at(y));
    for (uint32_t i = 0; i < UI_WAVE_N; i++) { wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
            for (uint32_t z = 0; z < SPEC_BANDS; z++) spec_drawn[z] = 0xFFu; }
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

/* Speaker, with 0..3 waves. The volume is a MODE like repeat and shuffle --
 * it persists, it changes what you hear, and until now the only sign of it was
 * a toast that had already gone by the time you wondered why the music was
 * quiet. Same 11x9 box and same 1 px construction as its neighbours.
 *
 * Drawn rather than typed because the font is ASCII 0x20..0x7E and has no
 * speaker in it.
 *
 * The waves are arcs, not bars: the tall ones have their ends pulled back a
 * pixel so they curve. The first is left straight -- it is three pixels tall,
 * where a curve is indistinguishable from a wobble, and pulling its end back
 * would put it against the cone and read as attached to it.
 *
 * Mute is an X rather than a slash across the whole icon. A slash would cross
 * the cone, and at this size that turns two recognisable shapes into one
 * unrecognisable one. */
static void ui_icon_speaker(uint32_t x, uint32_t y, uint32_t lvl, uint16_t c)
{
    fb_rect(x, y + 3u, 2u, 3u, c);                   /* the box */
    for (uint32_t i = 0; i < 3u; i++)                /* the cone, opening out */
        fb_rect(x + 2u + i, y + 2u - i, 1u, 5u + 2u * i, c);

    if (!lvl) {
        for (uint32_t i = 0; i < 5u; i++) {
            fb_rect(x + 6u + i,  y + 2u + i, 1u, 1u, c);
            fb_rect(x + 10u - i, y + 2u + i, 1u, 1u, c);
        }
        return;
    }
    for (uint32_t k = 0; k < lvl && k < 3u; k++) {
        uint32_t wx = x + 6u + 2u * k;
        uint32_t top = y + 3u - k, h = 3u + 2u * k;
        if (!k) {
            fb_rect(wx, top, 1u, h, c);
        } else {
            fb_rect(wx, top + 1u, 1u, h - 2u, c);
            fb_rect(wx - 1u, top, 1u, 1u, c);
            fb_rect(wx - 1u, top + h - 1u, 1u, 1u, c);
        }
    }
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

    /* Scroll when the PAINTED text overruns, not when its advances do.
     *
     * fb_text_width sums advances, but fb_char paints a whole CELL -- 32 px at
     * 2x against an average advance near 22 -- so the final glyph reaches up to
     * a cell past where the advance total says the text ends. A title could
     * therefore pass the fits-check, have its last cell clipped at
     * UI_CARD_TEXT_R, and never scroll because nothing thought it overflowed.
     *
     * Measured on real titles: "Come All Ye Faithful" is 348 advance-pixels
     * against a 352 budget -- inside it by four -- yet paints to 390 against a
     * right edge of 380. "Alabama Getaway" is 312 and paints to 342, which
     * genuinely fits and correctly stays still. */
    uint32_t adv_w = fb_text_width(m->text, scale);
    uint32_t last  = i ? fb_adv(m->text[i - 1u], scale) : 0u;
    uint32_t painted = (adv_w > last) ? (adv_w - last + FB_CELL(scale))
                                      : FB_CELL(scale);
    m->on = (adv_w > ui_text_w) ||
            (UI_MARGIN + painted > UI_CARD_TEXT_R);
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

    /* Erase the full paintable width, not just the layout budget: a glyph
     * cell reaches past the budget, and anything painted outside the erased
     * strip is never cleaned up -- it accumulated as the text scrolled. */
    fb_rect(UI_MARGIN, m->y, UI_CARD_TEXT_R - UI_MARGIN,
            FB_CELL(m->scale), UI_PANEL);
    fb_set_color(fg, UI_PANEL);
    fb_text_boxed(UI_MARGIN, m->y, m->text + m->pos,
                  m->scale, m->scale, ui_text_w, UI_CARD_TEXT_R);
}

static void pl_ui_draw(void);   /* defined with the overlay, below */

static void ui_draw_chrome(void)
{
    /* Draw nothing at all while blanked -- a track change must not light the
     * screen back up. ui_blank_wake() calls this again on the way out, so the
     * skipped work is simply deferred rather than lost. */
    if (screen_blank) return;
    ui_gradient();

    /* When there's no usable title, show the FILENAME.
     *
     * It is almost always the song name, so an untagged file reads as itself.
     * What used to be here was a DIAGNOSTIC -- "NOTAG FFFB9064 R04", the head
     * bytes and the reload status. It told three failure modes apart during
     * the reload hunt and earned its place then, but a user is not debugging
     * this core: a file that plays perfectly well was announcing itself as a
     * hex dump. Nothing internal goes on this screen any more.
     *
     * That also retires the UNICODE TAG case, which was the same mistake in
     * words -- a tag encoding the parser declines to handle is our limitation
     * to state in the README, not a caption for someone's music. The filename
     * is the better answer there too, and those files always have one. */
    char namebuf[48];
    const char *title = track_title;
    if (!track_title[0]) {
        /* Last path component, extension dropped: the slot holds a full path
         * ("/Assets/mp3player/common/Flodown.mp3"). */
        uint32_t start = 0;
        for (uint32_t i = 0; track_file[i]; i++)
            if (track_file[i] == '/' || track_file[i] == 0x5Cu) start = i + 1u;

        uint32_t n = 0, dot = 0;
        for (uint32_t i = start; track_file[i] && n < sizeof(namebuf) - 1u; i++) {
            if (track_file[i] == '.') dot = n;      /* LAST dot, not the first */
            namebuf[n++] = track_file[i];
        }
        /* Only trim at a dot that actually looks like an extension -- a name
         * such as "Blur - 13.mp3" must not lose its number, and a leading dot
         * is not an extension at all. */
        if (dot && n - dot <= 5u) n = dot;
        namebuf[n] = 0;

        /* No tag and no name means APF told us nothing about the slot. Rare,
         * and still not the user's problem to diagnose. */
        title = n ? namebuf : "UNKNOWN TRACK";
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
    fb_text_boxed(UI_MARGIN, UI_TITLE_Y, ui_mq_title.text, ts, ts,
                  ui_text_w, UI_CARD_TEXT_R);

    /* Artist one step down from the title, never below 1.5x -- that step only
     * exists because the engine can scale fractionally now. */
    uint32_t as = (ts > TS_15X) ? (ts - 1u) : TS_15X;
    ui_mq_artist.on = 0;      /* no artist -> no leftover scroll from the last track */
    uint32_t y = UI_TITLE_Y + FB_CELL(ts) + 6u;
    if (track_artist[0]) {
        fb_set_color(UI_DIM, UI_PANEL);
        ui_marq_init(&ui_mq_artist, track_artist, y, as);
        fb_text_boxed(UI_MARGIN, y, ui_mq_artist.text, as, as,
                      ui_text_w, UI_CARD_TEXT_R);
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
        /* This row doubles as the warning line for a file the core will not
         * play. Grey reads as another piece of metadata; red reads as a
         * problem, which is the entire point of putting it there. */
        fb_set_color(ui_warn_row ? UI_RED : UI_DIM, UI_PANEL);
        fb_text_boxed(UI_MARGIN, y, b, TS_1X, TS_1X,
                      ui_text_w, UI_CARD_TEXT_R);
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
    /* The elapsed time is NOT reset here, and used to be.
     *
     * A repaint destroys what is DRAWN, not what has elapsed -- but this zeroed
     * ui_sec itself, so every caller that repaints mid-track silently threw the
     * clock away. load_track was the only caller where that looked correct, and
     * it hid the fault from the others: an accent change, a blank wake, and now
     * closing the playlist overlay all restarted the timer at 0:00.
     *
     * Resetting a new track's clock belongs to load_track, which is the only
     * place that knows a new track started. It does it now. */
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
    /* The overlay sits ON TOP of everything this function just painted. Any
     * caller that repaints mid-browse -- a track auto-advancing, a blank wake,
     * an accent change -- would otherwise leave the player showing until the
     * next main-loop pass noticed and redrew the list. That gap is the "it
     * flips to the player" flicker.
     *
     * Putting it here rather than in the main loop means EVERY repaint route
     * is covered by construction, including ones added later. */
    if (pl_ui_open) pl_ui_draw();

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
/* The parts of the splash card that never change: the panel itself and the
 * version. Split out because the fade redraws the TITLE 33 times, and this
 * used to be redrawn with it.
 *
 * That was the rest of the boot flicker. Capping the fade at 33 steps stopped
 * the title flashing thousands of times a second, but each of those 33 still
 * refilled the whole card first -- wiping the title AND the version and
 * writing them back, 33 times, with scanout free to catch either gap. The
 * version never changes at all, so it was pure churn.
 *
 * Glyph cells paint their own background, so the title can be redrawn in
 * place over itself without the panel underneath being cleared first. */
static void ui_splash_bg(void)
{
    /* Identical geometry to ui_draw_chrome()'s card, at full width since the
     * splash has no art panel to make room for. */
    fb_round_rect(UI_MARGIN - 8u, UI_TITLE_Y - 14u,
                  UI_INNER_W + 16u, UI_CARD_H, 8u, UI_PANEL);

    fb_set_color(UI_DIM, UI_PANEL);
    fb_text_clipped(UI_MARGIN, UI_SPL_VER_Y, "v" APP_VER, TS_1X, TS_1X,
                    UI_INNER_W);
}

static void ui_splash_title(uint32_t f, uint32_t den)
{
    uint32_t sc = fb_text_fit("MP3 PLAYER", UI_INNER_W, TS_2X);
    fb_set_color(ui_mix(UI_PANEL, ui_accent, f, den), UI_PANEL);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y, "MP3 PLAYER", sc, sc, UI_INNER_W);
}

static void ui_splash_card(uint32_t f, uint32_t den)
{
    ui_splash_bg();
    ui_splash_title(f, den);
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
    /* cycles(), not 0 -- ui_wave_anim_tick() compares
     * (int32_t)(cycles() - wv_next) < 0, which against 0 is just the sign of
     * the counter, so this animation was dead for the same half of every
     * 71.6 s wrap as the loading dots beside it. */
    wv_on = 1u; wv_next = cycles(); wv_rng = cycles() | 1u;
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
    ui_splash_bg();                  /* ONCE -- see ui_splash_bg() */
    for (;;) {
        uint32_t el = cycles() - t0;
        if (el >= CLK_HZ / 1000u * INTRO_MS) break;
        uint32_t f = (el < fade_end) ? (el * STEP_DEN / fade_end) : STEP_DEN;
        if (f != last_f) { last_f = f; ui_splash_title(f, STEP_DEN); }
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
    /* cycles(), not 0. The tick tests (int32_t)(cycles() - ui_boot_next) < 0,
     * which with 0 reduces to the SIGN OF THE COUNTER -- so for the half of
     * every 71.6 s wrap where cycles() is above 2^31 the tick returned early,
     * never painted, and never updated the deadline either. The dots were dead
     * for the whole of roughly every other load, which is why they were
     * "rarely seen". Same fault as fl_ui_next and pl_poll_at in c501764. */
    ui_boot_next = cycles();                /* first tick paints immediately */
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
/* 1 KB, and ONLY the Select+A dump ever reads it -- which is behind
 * DEBUG_DIAG. In a release build this was a kilobyte of BSS taken from
 * the heap Helix mallocs its decoder out of, to feed a screen that
 * cannot be reached. */
#if DEBUG_DIAG
static uint32_t dt_snap[256];
#endif

static void dt_snapshot(void)
{
#if DEBUG_DIAG
    for (uint32_t w = 0; w < 256u; w++) dt_snap[w] = dt_read(w);
#endif
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
#if DEBUG_DIAG
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
#endif

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

/* The failure screen, with the two explanatory lines supplied by the caller.
 *
 * "THE FILE COULD NOT BE READ" is right for a file that genuinely would not
 * read, and wrong for one this core simply cannot decode fast enough -- that
 * is not a broken file, and telling someone their music is corrupt when it is
 * not is worse than saying nothing. */
static void ui_failed_msg(const char *l1, const char *l2)
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

    /* This line used to print the file's first four bytes as hex. That is a
     * debugging aid, and it was on the ONE screen a user is most likely to see
     * when something has gone wrong -- the moment they least need a hex dump.
     * Say what happened in words instead. */
    fb_set_color(UI_DIM, UI_BG);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y + FB_CELL(TS_2X) + 14u,
                    l1, TS_1X, TS_1X, UI_INNER_W);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y + FB_CELL(2u) + 40u,
                    l2, TS_1X, TS_1X, UI_INNER_W);

    /* Stay alive so a reload can rescue us.
     *
     * poll_input() only sees the 008A NOTIFICATION, so a missed one left this
     * screen with no way out and the next pick appeared to do nothing -- pick
     * again and it works. Ask the slot directly as well. No audio is playing
     * here, so the query costs nothing that matters and can run often. */
    tk_poll_at = cycles() + CLK_HZ;
    for (;;) {
        poll_input();
        if (reload_pending) return;
        if ((int32_t)(cycles() - tk_poll_at) >= 0) {
            tk_poll_at = cycles() + CLK_HZ;
            if (slot_changed()) reload_pending = 1u;
        }
    }
}

static void ui_load_failed(void)
{
    ui_failed_msg("THE FILE COULD NOT BE READ", "USE CORE MENU TO PICK");
}

/* Hi-res FLAC this core cannot decode in realtime.
 *
 * The cutoff is 48 kHz, and it is measured rather than chosen. Decode cost as
 * a percent of realtime, with I/O already at zero: 16/44.1 = 74, 24/44.1 = 80,
 * 24/88.2 = 150, 24/96 = 180. Cost tracks sample rate almost exactly, so 48 kHz
 * lands near 87 for 24-bit and everything above it is beyond reach -- 88.2 kHz
 * would need the decoder to be half again faster, and the largest single
 * optimisation available bought 1.3x on one of its two passes.
 *
 * Refusing is the kind thing to do. These files DO decode, just not fast
 * enough, so without this they play through to the end sounding broken -- and
 * a listener has no way to tell that from a damaged file or a broken core. */
/* 48000, from measurement: decode is 74% of realtime at 16/44.1 and 80% at
 * 24/44.1, rising almost exactly with sample rate to 150% at 88.2 kHz. 48 kHz
 * lands near 87% for 24-bit, which fits; nothing above it does. */
#define FLAC_MAX_RATE 48000u
static uint8_t rate_unsupported;   /* set at load, consumed by the main loop */

/* Shown ON THE TRACK CARD rather than as a takeover screen. The card is
 * already where this player explains what is loaded, the filename still reads
 * as the title, and the layout does not jump -- a full-screen LOAD FAILED for
 * a file that is perfectly good is a heavier response than the situation
 * deserves. */
static void ui_rate_unsupported(void)
{
    /* KEEP the title and artist when we have them. For a rate rejection the
     * Vorbis comments were already parsed -- the gate runs after flac_open --
     * so the card reads "Jerry Garcia / Alabama Getaway" with the reason under
     * it. The other three reasons fire inside flac_open, before the comment
     * block is reached, so those fall back to the filename. */
    track_year[0] = 0;
    track_trk[0]  = 0;

    {
        char *q = track_album;
        uint32_t v = fl_reject_val;

        if (fl_reject_kind == FLR_RATE) {
            const char *p1 = "HI-RES ";
            while (*p1) *q++ = *p1++;
            q = ui_dec(q, v / 1000u);
            uint32_t frac = (v % 1000u) / 100u;
            if (frac) { *q++ = '.'; *q++ = (char)('0' + frac); }
            const char *p2 = "kHz - PLAYS 48kHz MAX";
            while (*p2) *q++ = *p2++;
        } else if (fl_reject_kind == FLR_DEPTH) {
            q = ui_dec(q, v);
            const char *p2 = "-BIT FLAC - PLAYS 24-BIT MAX";
            while (*p2) *q++ = *p2++;
        } else if (fl_reject_kind == FLR_CHANS) {
            q = ui_dec(q, v);
            const char *p2 = " CHANNELS - PLAYS STEREO MAX";
            while (*p2) *q++ = *p2++;
        } else {
            const char *p1 = "BLOCK ";
            while (*p1) *q++ = *p1++;
            q = ui_dec(q, v);
            const char *p2 = " - PLAYS 6144 MAX";
            while (*p2) *q++ = *p2++;
        }
        *q = 0;
    }
    ui_warn_row = 1u;

    ui_blank_wake();

    /* Repaint the whole frame before the card.
     *
     * ui_draw_chrome() draws the card and nothing behind it, so the previous
     * track's ALBUM ART stayed on screen next to a message about a file that
     * has none -- it read as though the refused file had that artwork. The
     * gradient clears it.
     *
     * The art panel is also parked off-screen, so nothing slides it back: a
     * refused file has no art of its own, and art_have/art_file_id still
     * describe the previous track, which is what should happen if the user
     * goes back to it. */
    art_shown = 0;
    art_x     = FB_W;
    ui_gradient();
    ui_draw_chrome();

    /* Stay alive so a reload can rescue us, exactly as the failure screen
     * does -- the difference is only what the user is looking at. */
    /* Same fallback as the failure screen: this is exactly where it was seen
     * -- refuse a hi-res file, pick a playable one, nothing happens. */
    tk_poll_at = cycles() + CLK_HZ;
    for (;;) {
        poll_input();
        if (reload_pending) { ui_warn_row = 0u; return; }
        if ((int32_t)(cycles() - tk_poll_at) >= 0) {
            tk_poll_at = cycles() + CLK_HZ;
            if (slot_changed()) reload_pending = 1u;
        }
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
/* ---- playlist overlay -----------------------------------------------------
 *
 * Select TAPS open a scrollable list of the playlist; Select HELD keeps its old
 * job of toggling the art panel. D-pad moves the selection, A plays it.
 *
 * Rows are FILENAMES, not tags. pl_text holds the .m3u text and that is all the
 * core has: a tag lives inside its file, so titling 256 rows would mean 256
 * opens. The currently playing row is the one exception -- its tag is already
 * in track_title -- but showing it differently from its neighbours reads as a
 * bug, so it gets the same treatment and a marker instead.
 *
 * Order follows pl_order, so with shuffle on the list is the QUEUE: scrolling
 * down previews what is actually coming.
 */
/* 9, not 11. The transport row -- PLAYING/PAUSED, the repeat and shuffle
 * arrows, the EQ name -- sits at UI_TRANSPORT_Y 262 and keeps animating while
 * the overlay is up, so an 11-row panel ending at 284 had it drawing straight
 * through. Nine rows end the panel at 244 and leave that row, the clock and
 * the progress bar all visible below the list, which is more useful than the
 * two rows it costs. */
#define PL_UI_ROWS   9u
#define PL_UI_X      12u
#define PL_UI_W      (FB_W - 2u * PL_UI_X)
#define PL_UI_Y      18u
#define PL_UI_ROW_H  20u
#define PL_UI_LIST_Y 52u
#define PL_UI_TEXT_X (PL_UI_X + 10u)
/* Bottom padding, and it is load-bearing rather than cosmetic. The art frame
 * reaches y 249 and the meter 245; at 12 the panel ended at 244 and left a
 * sliver of both showing under it. 22 puts the bottom at 254 -- clear of both,
 * and still 8 px above the transport row at 262, which stays visible on
 * purpose. Named so tools/overlay_preview.py reads it instead of repeating
 * the number. */
#define PL_UI_PAD_B  22u

static uint16_t pl_ui_sel;         /* selection, an index into pl_order      */
static uint16_t pl_ui_top;         /* first visible row                      */

/* Last path component with the extension trimmed -- the same shape the title
 * row falls back to, so a file reads the same in both places. */
static void pl_ui_label(uint16_t pos, char *out, uint32_t cap)
{
    out[0] = 0;
    if (pos >= pl_count) return;
    const char *nm = &pl_text[pl_off[pl_order[pos]]];

    uint32_t start = 0;
    for (uint32_t i = 0; nm[i]; i++)
        if (nm[i] == '/' || nm[i] == 0x5Cu) start = i + 1u;

    uint32_t n = 0, dot = 0;
    for (uint32_t i = start; nm[i] && n < cap - 1u; i++) {
        if (nm[i] == '.') dot = n;
        out[n++] = nm[i];
    }
    if (dot && n - dot <= 5u) n = dot;      /* ".mp3"/".flac", not "Blur - 13" */
    out[n] = 0;
}

/* Keeps the selection on screen after any move. */
static void pl_ui_follow(void)
{
    if (pl_ui_sel < pl_ui_top) pl_ui_top = pl_ui_sel;
    else if (pl_ui_sel >= pl_ui_top + PL_UI_ROWS)
        pl_ui_top = (uint16_t)(pl_ui_sel - PL_UI_ROWS + 1u);
    if (pl_count > PL_UI_ROWS && pl_ui_top > pl_count - PL_UI_ROWS)
        pl_ui_top = (uint16_t)(pl_count - PL_UI_ROWS);
    if (pl_count <= PL_UI_ROWS) pl_ui_top = 0;
}

/* One row. Split out so the marquee can repaint just the selected line
 * without redrawing the whole list four times a second. */
static void pl_ui_row(uint32_t i)
{
    uint32_t y   = PL_UI_LIST_Y + i * PL_UI_ROW_H;
    uint16_t pos = (uint16_t)(pl_ui_top + i);

    if (pos >= pl_count) {                       /* short list: clear the row */
        fb_rect(PL_UI_X + 4u, y - 2u, PL_UI_W - 8u, PL_UI_ROW_H, UI_PANEL);
        return;
    }

    /* Selected row is a filled bar, drawn first so the text paints onto it --
     * the engine writes a glyph's background with every character.
     *
     * Rounded, because the panel it sits in is: a square highlight inside an
     * 8 px rounded panel reads as a different piece of furniture. Corners cut
     * to UI_PANEL rather than the screen gradient, which is what
     * fb_round_rect_on() exists for. Unselected rows stay square -- they are
     * the panel colour, so there is no shape to see either way, and drawing
     * the corner cuts on every row would be work for nothing. */
    uint16_t bg = (pos == pl_ui_sel) ? ui_accent : UI_PANEL;
    if (pos == pl_ui_sel)
        fb_round_rect_on(PL_UI_X + 4u, y - 2u, PL_UI_W - 8u, PL_UI_ROW_H,
                         5u, bg, UI_PANEL);
    else
        fb_rect(PL_UI_X + 4u, y - 2u, PL_UI_W - 8u, PL_UI_ROW_H, bg);

    char nm[64];
    pl_ui_label(pos, nm, sizeof(nm));

    /* The selected row scrolls when it does not fit. Steps by whole characters
     * because the engine cannot clip a glyph partly off the left edge -- the
     * same constraint the title marquee works under. */
    const char *txt = nm;
    if (pos == pl_ui_sel && pl_ui_mq_off) {
        uint32_t len = 0;
        while (nm[len]) len++;
        txt = nm + (pl_ui_mq_off < len ? pl_ui_mq_off : 0u);
    }

    /* '>' rather than an arrow glyph: the font is ASCII 0x20..0x7E. */
    fb_set_color(pos == pl_ui_sel ? UI_PANEL
               : pos == pl_pos    ? UI_WHITE : UI_DIM, bg);
    if (pos == pl_pos)
        fb_text_clipped(PL_UI_X + 8u, y, ">", TS_1X, TS_1X, 12u);
    fb_text_boxed(PL_UI_TEXT_X + 8u, y, txt, TS_1X, TS_1X,
                  PL_UI_W - 40u, PL_UI_X + PL_UI_W - 16u);
}

static void pl_ui_draw(void)
{
    /* The list can change under an open overlay -- a reload, or a pick the
     * core noticed late. Clamp rather than trusting the stored indices. */
    if (!pl_count) { pl_ui_open = 0u; pl_ui_restore = 1u; return; }
    if (pl_ui_sel >= pl_count) pl_ui_sel = (uint16_t)(pl_count - 1u);
    pl_ui_follow();
    pl_ui_drawn_pos = pl_pos;

    uint32_t h = PL_UI_LIST_Y + PL_UI_ROWS * PL_UI_ROW_H + PL_UI_PAD_B - PL_UI_Y;
    fb_round_rect(PL_UI_X, PL_UI_Y, PL_UI_W, h, 8u, UI_PANEL);

    /* Header: where you are in the list, which is the thing a long list hides. */
    char hdr[40], *q = hdr;
    const char *t = "PLAYLIST";
    while (*t) *q++ = *t++;
    if (pl_count) {
        *q++ = ' '; *q++ = ' ';
        q = ui_dec(q, (uint32_t)pl_ui_sel + 1u);
        *q++ = ' '; *q++ = '/'; *q++ = ' ';
        q = ui_dec(q, pl_count);
    }
    *q = 0;
    fb_set_color(ui_accent, UI_PANEL);
    fb_text_clipped(PL_UI_TEXT_X, PL_UI_Y + 10u, hdr, TS_1X, TS_1X, PL_UI_W - 20u);

    /* Scroll position, for lists too long to hold in your head. The header
     * counter says WHERE you are; this says how far that is through the list,
     * which at 240 entries is the question actually being asked. Two rects,
     * drawn only when the list overflows the window. */
    if (pl_count > PL_UI_ROWS) {
        uint32_t track_x = PL_UI_X + PL_UI_W - 11u;
        uint32_t track_y = PL_UI_LIST_Y - 2u;
        uint32_t track_h = PL_UI_ROWS * PL_UI_ROW_H;
        fb_rect(track_x, track_y, 3u, track_h, ui_mix(UI_PANEL, UI_DIM, 1u, 3u));

        uint32_t span = pl_count - PL_UI_ROWS;          /* max value of _top */
        uint32_t th   = track_h * PL_UI_ROWS / pl_count;
        if (th < 8u) th = 8u;                           /* stays grabbable   */
        uint32_t ty   = track_y + (track_h - th) * pl_ui_top / span;
        fb_rect(track_x, ty, 3u, th, ui_accent);
    }

    for (uint32_t i = 0; i < PL_UI_ROWS; i++) pl_ui_row(i);
}


static void ui_draw_dynamic(void)
{
    if (screen_blank) return;
    /* The overlay covers the meters, the card and the transport row. Letting
     * the player keep drawing underneath would punch holes straight through
     * it, once per frame. */
    /* Jump the UPPER screen, not the whole function.
     *
     * The overlay panel is y 18..284; the clock (288) and the progress bar
     * (334) sit BELOW it and stay visible. Returning early here froze both,
     * and worse: the elapsed-time accumulator lives further down this
     * function, so time itself stopped advancing while the list was open and
     * the clock came back stale.
     *
     * A goto over the drawing rather than a wrapped block -- the skipped
     * region is several hundred lines and every declaration in it is scoped
     * inside the sections being skipped. `viz_done` already sets the
     * precedent. */
    if (pl_ui_open) goto ui_tail;

    /* Publish the peaks once per display frame, so every meter below reads a
     * value covering exactly the audio since the last frame. Nothing new means
     * hold the last -- correct during FLAC's channel-0 window, where there
     * genuinely is no new audio to show. */
    if (peak_acc_any) {
        /* Headroom. Nothing about HOW the meters respond changes here -- they
         * are still peak-driven, with the same ballistics and the same
         * peak-hold fall -- every bar simply sits lower.
         *
         * That distinction was learned the hard way: the first attempt at this
         * report replaced peak with a mean, which fixed the height and altered
         * the character of all ten meters at once. The complaint was the
         * level, not the response.
         *
         * 3/4 is measured, not chosen. On 56 windows of 1152 pairs from a real
         * track, decoded through the real decoder under tools/rv32sim.py, the
         * bar ran 33..71 of 72 pixels and sat at or above 95% in 8 of those
         * windows -- pegged, which is what "many of the meters are peaked out"
         * was. At 3/4 the same material runs 25..53 with nothing pegged, and
         * the loudest moment reaches 74% of the bar, so a track a third louder
         * than this one still has somewhere to go. 7/8 also clears this track
         * but leaves only 14% of headroom, which is thin when the report said
         * MANY songs.
         *
         * One constant, applied in one place, so all ten meters keep agreeing.
         * tools/meter_preview.py renders the effect from real audio. */
        peak_amp     = (peak_acc   * MTR_HEADROOM_NUM) / MTR_HEADROOM_DEN;
        peak_l       = (peak_acc_l * MTR_HEADROOM_NUM) / MTR_HEADROOM_DEN;
        peak_r       = (peak_acc_r * MTR_HEADROOM_NUM) / MTR_HEADROOM_DEN;
        peak_acc     = peak_acc_l = peak_acc_r = 0;
        peak_acc_any = 0;

        /* Each band's mean magnitude over the window, tilted, scaled to
         * 0..255. Stage b was fed at 1/2^b of the rate, so the mean divides by
         * ITS OWN sample count and not the window's -- dividing everything by
         * the window would make every band below the first read low by exactly
         * the factor it was downsampled by, which is a convincing-looking
         * wrong answer. */
        if (spec_n) {
            for (uint32_t b = 0; b < SPEC_BANDS; b++) {
                /* Both halves of an octave were fed at that OCTAVE's rate, so
                 * the divisor is per stage, not per band. */
                uint32_t cnt  = spec_n >> (b / 2u);
                uint32_t mean = cnt ? (spec_acc[b] / cnt) : 0u;
                uint32_t v    = (mean * spec_gain[SPEC_BANDS - 1u - b]) >> 4;

                /* LOGARITHMIC, because loudness is.
                 *
                 * A linear meter spends nearly all its range on the top 6 dB
                 * and almost none on everything below, so anything mastered
                 * louder than the two tracks these gains were measured on
                 * simply pegs -- which is what "the spectrum maxes out" was.
                 * It was not clipping; it was the scale.
                 *
                 * spec_log gives 16 units per octave, so the window below is
                 * 80 units = 30 dB of display range. Measured across both
                 * tracks the bands span 202..238, so the floor sits below the
                 * quietest and there are 32 units -- 12 dB -- of headroom
                 * above the loudest before anything reaches the top. On a
                 * linear scale that headroom was about 1.5 dB.
                 *
                 * Nothing about the per-band calibration changes: the gains
                 * still multiply first, and the log is taken of the result. */
                uint32_t lg = spec_log(v);
                v = (lg > SPEC_FLOOR)
                  ? ((lg - SPEC_FLOOR) * 255u) / SPEC_SPAN : 0u;
                if (v > 255u) v = 255u;
                /* Same ballistics the level ladder used: catch the transient,
                 * fall back smoothly. */
                if (v >= spec_lvl[b]) spec_lvl[b] = (unsigned char)v;
                else spec_lvl[b] -= (unsigned char)((spec_lvl[b] - v) / 4u + 1u);
                spec_acc[b] = 0;
            }
            spec_n = 0;
        }
        if (peak_amp > wave_pend) wave_pend = peak_amp;
    }
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
    /* The eye settles for the same reason the needles do: its shadow OPENING
     * back to rest is the movement that makes it look like a tube rather than
     * a graphic, and freezing it half-shut looks broken. eye_v counts DOWN to
     * rest, so "not yet settled" is a non-zero deflection, same as the VU. */
    uint32_t vu_settling = ((viz_mode == VIZ_VU)  && (vu_l || vu_r)) ||
                           ((viz_mode == VIZ_EYE) && (eye_l || eye_r));
    if ((!paused || ui_wave_force || vu_settling) && ++ui_last_vu >= 2u) {
        ui_last_vu = 0;

        uint32_t wf = ui_wave_force; ui_wave_force = 0;
        /* The accumulated maximum, not the instantaneous value: this tick
         * covers two display frames and both should count. */
        uint32_t src = wave_pend ? wave_pend : peak_amp;
        wave_pend = 0;
        uint32_t amp = (src * UI_WAVE_H) / 32768u;
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
                ui_bg_restore(cx, UI_WAVE_Y, 1, UI_WAVE_H);     /* clear column */
                if (a) fb_rect(cx, cy - a, 1, a * 2u + 1u,
                               ui_mix(UI_TRACK, ui_accent, a, half));
                else   fb_rect(cx, cy, 1, 1, UI_TRACK);         /* silence line */
            }
            goto viz_done;
        }

        /* ---- MIRRORED BARS ------------------------------------------------
         * The same wave[] history the bars use, grown up AND down from a
         * centre line. Same cost as the bars; different shape entirely. */
        /* ---- SPECTRUM ------------------------------------------------
         *
         * Eight columns of real frequency content from the octave cascade --
         * see SPEC_BANDS. Bass on the left, treble on the right, each moving
         * on its own.
         *
         * This replaced a two-channel level ladder. Six blocks drawn from one
         * number will always rise and fall together however they are styled;
         * "make them move independently" is not a tuning request, it needs
         * frequency data, and the cascade is what provides it. */
        if (viz_mode == VIZ_LED) {
            if (paused)
                for (uint32_t b = 0; b < SPEC_BANDS; b++) spec_lvl[b] = 0;

            /* The gaps between blocks show background, and the background is
             * a per-row ramp -- a flat fill is the mistake the magic eye made.
             * Only on a repaint: the gaps never move. */
            int repaint = (spec_drawn[0] == 0xFFu);
            if (repaint) ui_bg_restore(UI_MARGIN, UI_WAVE_Y, ww, UI_WAVE_H);

            uint32_t colw  = ww / SPEC_BANDS;
            uint32_t bw    = (colw > SPEC_GAPX) ? colw - SPEC_GAPX : 1u;
            uint32_t pitch = LED_BLKH + LED_GAPV;

            for (uint32_t b = 0; b < SPEC_BANDS; b++) {
                uint32_t lit  = ((uint32_t)spec_lvl[b] * LED_ROWS) / 256u;
                uint32_t prev = spec_drawn[b];

                /* Nothing crossed a row boundary: draw nothing at all. In
                 * ordinary music most bands are in this state on most
                 * updates, which is the whole saving. */
                if (!repaint && lit == prev) continue;

                uint32_t lo = repaint ? 0u : (lit < prev ? lit : prev);
                uint32_t hi = repaint ? LED_ROWS : (lit > prev ? lit : prev);
                spec_drawn[b] = (unsigned char)lit;

                uint32_t x0 = UI_MARGIN + b * colw;
                for (uint32_t r = lo; r < hi; r++) {
                    uint32_t y = UI_WAVE_Y + UI_WAVE_H - (r + 1u) * pitch;
                    uint16_t c;
                    if (r < lit) {
                        uint32_t half = LED_ROWS / 2u;
                        c = (r < half)
                          ? ui_mix(LED_LO, LED_MIDC, r, half)
                          : ui_mix(LED_MIDC, LED_HI, r - half,
                                   LED_ROWS - half);
                    } else {
                        c = UI_TRACK;
                    }
                    fb_rect(x0, y, bw, LED_BLKH, c);
                }
            }
            goto viz_done;
        }

        if (viz_mode == VIZ_MIRROR) {
            const uint32_t cy = UI_WAVE_Y + UI_WAVE_H / 2u;
            const uint32_t half = UI_WAVE_H / 2u - 1u;
            for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                uint32_t x   = UI_MARGIN + (i * ww) / UI_WAVE_N;
                uint32_t xn  = UI_MARGIN + ((i + 1u) * ww) / UI_WAVE_N;
                uint32_t lit = (xn - x > UI_WAVE_GAP) ? (xn - x - UI_WAVE_GAP) : 1u;
                uint32_t h   = (wave[i] * half) / UI_WAVE_H;
                if (!h) h = 1u;

                /* Skip a column whose height has not moved, exactly as the
                 * plain bars do. On a scroll most columns land on the height
                 * already drawn there, and each skip saves the erase and the
                 * fill both. */
                if (h == wave_drawn[i]) continue;
                wave_drawn[i] = (unsigned char)h;

                uint16_t lc  = paused ? ui_mix(UI_TRACK, ui_accent, 1u, 3u) : ui_accent;
                uint16_t c   = ui_mix(UI_TRACK, lc, i + 1u, UI_WAVE_N);

                /* Restore ONLY around the bar, never underneath it.
                 *
                 * This used to blank the whole column and then paint the bar
                 * back into it, so every column was briefly empty every frame.
                 * The panel scans out asynchronously, so some of those gaps
                 * were visible -- the faint artifacting on fast movement, and
                 * a side effect of moving these meters onto the gradient
                 * (a flat `bed` fill used to double as the erase).
                 *
                 * The two spans plus the bar always sum to exactly UI_WAVE_H,
                 * and the bar's own pixels are now written once, not twice. */
                uint32_t top = cy - h;                      /* first bar row */
                uint32_t bot = cy + h;                      /* last bar row  */
                uint32_t end = UI_WAVE_Y + UI_WAVE_H;       /* one past box   */
                if (top > UI_WAVE_Y)
                    ui_bg_restore(x, UI_WAVE_Y, lit, top - UI_WAVE_Y);
                if (bot + 1u < end)
                    ui_bg_restore(x, bot + 1u, lit, end - (bot + 1u));
                fb_rect(x, top, lit, h * 2u + 1u, c);
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

                /* Same treatment as the mirrored bars: skip an unmoved column,
                 * and restore around the dot rather than through it. */
                if (pk == wave_pk_drawn[i]) continue;
                wave_pk_drawn[i] = (unsigned char)pk;

                uint16_t c   = ui_mix(UI_TRACK, ui_accent, i + 1u, UI_WAVE_N);
                uint32_t top = UI_WAVE_Y + UI_WAVE_H - pk;   /* first dot row */
                uint32_t end = UI_WAVE_Y + UI_WAVE_H;        /* one past box  */
                if (top > UI_WAVE_Y)
                    ui_bg_restore(x, UI_WAVE_Y, lit, top - UI_WAVE_Y);
                if (top + 2u < end)
                    ui_bg_restore(x, top + 2u, lit, end - (top + 2u));
                fb_rect(x, top, lit, 2u, c);
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
                ui_bg_restore(cx, UI_WAVE_Y, 1, UI_WAVE_H - a);
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

            /* Per ROW. Same fault the magic eye exposed: the box was filled
             * with `bed`, the gradient sampled once at its top row, which is a
             * flat slab on a ramp that falls to 62% of that value by the
             * bottom. The needles leave most of the box empty, so it shows. */
            if (!vu_face)
                for (uint32_t yy = UI_WAVE_Y; yy < UI_WAVE_Y + UI_WAVE_H; yy++)
                    fb_rect(UI_MARGIN, yy, ww, 1, ui_grad_at(yy));

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
                                           : ui_mix(ui_grad_at((uint32_t)ay),
                                                    ui_accent, 2u, 5u));
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
                                              : ui_mix(ui_grad_at((uint32_t)ay),
                                                       ui_accent, 3u, 5u));
                        }
                    }
                    fb_set_color(ui_accent, ui_grad_at(UI_WAVE_Y + 2u));
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
                    /* The erase pass repaints the needle's own footprint in
                     * the BACKGROUND colour, so with a ramp behind it that
                     * colour has to be sampled per segment -- a flat `bed`
                     * would leave a lighter trail down the lower half of the
                     * sweep, exactly where the needle spends most of its
                     * time. */
                    for (uint32_t k = 2; k <= VU_STEPS; k++) {
                        int32_t rr = ((int32_t)nlen * (int32_t)k) / (int32_t)VU_STEPS;
                        int32_t nx = (int32_t)pivx + (rr * sn) / 4096;
                        int32_t ny = (int32_t)pivy - (rr * cs) / 4096;
                        if (nx < (int32_t)ox || nx >= (int32_t)(ox + half)) continue;
                        if (ny < (int32_t)UI_WAVE_Y) continue;
                        uint32_t th = (k > VU_STEPS - 6u) ? 1u : 2u;
                        fb_rect((uint32_t)nx, (uint32_t)ny, th, th,
                                pass ? ui_accent
                                     : ui_grad_at((uint32_t)ny));
                    }
                }
                fb_rect(pivx - 2u, pivy - 2u, 5, 5, ui_accent);
                for (uint32_t r = 0; r < 3u; r++)
                    fb_rect(pivx - 1u, pivy - 1u + r, 3, 1,
                            ui_grad_at(pivy - 1u + r));

                if (ch) vu_shown_r = now; else vu_shown_l = now;
            }
            vu_face = 1;
            vu_face_w = (uint16_t)ww;
            goto viz_done;
        }

        /* ---- MAGIC EYE (EM84) ---------------------------------------------
         * Two tubes, one per channel. See the EYE_* block for why the glass
         * is shaded per column and why the glow blooms. */
        if (viz_mode == VIZ_EYE) {
            if (wf || ww != eye_face_w) eye_face = 0;

            uint32_t pair = 2u * EYE_TUBE_W + EYE_TUBE_G;
            uint32_t ty   = UI_WAVE_Y + 3u;
            uint32_t x0   = UI_MARGIN + ((ww > pair) ? (ww - pair) / 2u : 0u);
            uint32_t by   = ty + EYE_DOME + 7u;      /* strip window top    */
            /* Grew with the envelope, so the taller tube is also a longer
              * throw for the strip rather than just more glass. */
             uint32_t bh   = 38u;                     /* ...and its height   */
            uint32_t basy = ty + EYE_TUBE_H - 1u;    /* plinth              */
            uint16_t basc = ui_mix(ui_grad_at(ty + EYE_TUBE_H - 1u),
                                   ui_accent, 1u, 3u);

            if (!eye_face) {
                /* Per ROW, not one flat fill.
                 *
                 * Every other meter fills this box with `bed` -- the gradient
                 * sampled once at the box's top row -- and gets away with it
                 * because its content covers the box. This meter is the first
                 * with large EMPTY areas, so it is the first to show that a
                 * flat slab on a per-row gradient is a visible rectangle: the
                 * ramp falls from 16.1/31 at the top of the box to 9.9/31 at
                 * the bottom, and the slab holds the top value throughout.
                 *
                 * Painting the real ramp means the meter has no background of
                 * its own -- it sits on the screen's, and the glow fans into
                 * it with nothing to fan against. */
                for (uint32_t y = UI_WAVE_Y; y < UI_WAVE_Y + UI_WAVE_H; y++)
                    fb_rect(UI_MARGIN, y, ww, 1, ui_grad_at(y));

                for (uint32_t ch = 0; ch < 2u; ch++) {
                    uint32_t tx = x0 + ch * (EYE_TUBE_W + EYE_TUBE_G);
                    uint32_t hw = EYE_TUBE_W / 2u;

                    /* One vertical rect per column: the shade makes it a
                     * cylinder, and the crown's curve is a per-column top
                     * offset, so both come out of the same pass. */
                    for (uint32_t i = 0; i < EYE_TUBE_W; i++) {
                        uint32_t dx = (i < hw) ? (hw - i) : (i - hw);
                        uint32_t yc = 0;             /* circle, as elsewhere */
                        while ((yc + 1u) * (yc + 1u) + dx * dx <= hw * hw) yc++;
                        uint32_t off = EYE_DOME - (EYE_DOME * yc) / hw;

                        /* Specular streak left of centre, falling off faster
                         * to the right -- lit from the upper left, like the
                         * rest of the screen's shading. */
                        uint32_t dl = (i < 9u) ? (9u - i) * 2u
                                               : ((i - 9u) * 5u) / 4u;
                        uint32_t lv = (dl < 28u) ? (31u - dl) : 3u;
                        fb_rect(tx + i, ty + off, 1, EYE_TUBE_H - off,
                                ui_mix(EYE_GLASS_D, EYE_GLASS_L, lv, 31u));
                    }

                    /* Pip on the crown, and the getter flash under it. */
                    fb_rect(tx + hw - 2u, ty - 3u, 4, 4, EYE_GLASS_D);
                    fb_rect(tx + 6u, ty + EYE_DOME + 1u,
                            EYE_TUBE_W - 12u, 3, EYE_GETTER);
                    /* Socket end: the glass goes into a base, so the bottom
                     * rows are shadow rather than more cylinder. */
                    fb_rect(tx + 1u, ty + EYE_TUBE_H - 6u,
                            EYE_TUBE_W - 2u, 6, EYE_SOCKET);

                    uint32_t bx = tx + (EYE_TUBE_W - EYE_BAR_W) / 2u;
                    fb_rect(bx - 4u, by, EYE_BAR_W + 8u, bh, EYE_DARK);

                    /* Scale ticks etched beside the strip, as on the real
                     * tube's faceplate, in the accent. Mixed toward the glass
                     * rather than laid on pure, so they read as printed ON it
                     * instead of floating above it -- and the TOP tick goes on
                     * full, marking the loud end the way the VU's face marks
                     * its peak zone. */
                    for (uint32_t t = 1; t < 4u; t++)
                        fb_rect(bx + EYE_BAR_W + 5u, by + (bh * t) / 4u,
                                2, 1,
                                (t == 1u) ? ui_accent
                                          : ui_mix(EYE_GLASS_L, ui_accent,
                                                   3u, 4u));
                }

                /* Exactly the width of the pair, not wider. Overhanging it
                  * put 8px of plinth under each pool, and the pool repaints
                  * that span with the bed -- which chewed the ends off the
                  * base plate and read as a gap in the light. It cannot gain
                  * substance by growing sideways, so it gains it by being
                  * shaded: a lit top edge and a shadowed underside turn a flat
                  * bar into a slab with thickness. Six rows, which is every
                  * one left between the socket and the bottom of the box. */
                fb_round_rect(x0 - EYE_BASE_PAD, basy,
                              pair + 2u * EYE_BASE_PAD, EYE_BASE_H, 2u, basc);
                fb_rect(x0 - EYE_BASE_PAD + 2u, basy,
                        pair + 2u * EYE_BASE_PAD - 4u, 1,
                        ui_mix(basc, UI_WHITE, 1u, 4u));
                fb_rect(x0 - EYE_BASE_PAD + 2u, basy + EYE_BASE_H - 1u,
                        pair + 2u * EYE_BASE_PAD - 4u, 1,
                        ui_mix(basc, 0x0000u, 1u, 2u));
                eye_shown_l = eye_shown_r = 0xFFu;   /* force both strips */
                /* The box was just cleared, so there is no old cone to
                 * erase -- and after a width change its coordinates would
                 * point at the previous layout. */
                eye_cast_l  = eye_cast_r  = 0xFFu;
                eye_gap_l   = eye_gap_r   = 0xFFu;
            }

            /* Kept per channel so the gap pass below can evaluate BOTH
             * pools at a row without recomputing either. */
            uint32_t gcy_of[2], amt_of[2];
            uint8_t  key_of[2];

            for (uint32_t ch = 0; ch < 2u; ch++) {
                /* VU ballistics per channel -- the tubes are imitating the
                 * same era of gear as the needles, and two different feels
                 * would read as two different instruments. */
                uint32_t pk  = ch ? peak_r : peak_l;
                uint32_t tgt = (pk * 255u) / 32768u;
                if (tgt > 255u) tgt = 255u;
                if (paused) tgt = 0;
                uint32_t *v = ch ? &eye_r : &eye_l;
                if (tgt > *v) { *v += VU_ATT; if (*v > tgt) *v = tgt; }
                else          { *v = (*v > VU_DEC) ? (*v - VU_DEC) : 0u;
                                if (*v < tgt) *v = tgt; }

                /* A second, slower follower -- for the cast light's
                 * BRIGHTNESS only. Its position comes off the strip. */
                uint32_t *g = ch ? &eye_gl_r : &eye_gl_l;
                if (*v > *g) *g += (*v - *g + 7u) / 8u;
                else         *g -= (*g - *v) / 8u;

                uint32_t tx = x0 + ch * (EYE_TUBE_W + EYE_TUBE_G);
                uint32_t bx = tx + (EYE_TUBE_W - EYE_BAR_W) / 2u;

                uint8_t  lit   = (uint8_t)((*v * bh) / 255u);
                /* A real tube is never fully dark -- the strip sits at a
                 * short resting length with no signal, because the heater is
                 * on. Going to zero made the pair look switched OFF during
                 * quiet passages, which is the opposite of the impression a
                 * glowing tube is here to give. */
                if (lit < 3u) lit = 3u;
                uint8_t *shown = ch ? &eye_shown_r : &eye_shown_l;

                if (!eye_face || lit != *shown) {
                    uint32_t ly = by + bh - lit;      /* the strip grows UP */

                    /* Dark first, then the glow over it, so the window is
                     * fully covered whichever way the strip moved. */
                    if (lit < bh) fb_rect(bx - 4u, by, EYE_BAR_W + 8u,
                                          bh - lit, EYE_DARK);
                    fb_rect(bx - 4u, ly, 2, lit, EYE_H2);
                    fb_rect(bx - 2u, ly, 2, lit, EYE_H1);
                    fb_rect(bx, ly, EYE_BAR_W, lit, EYE_LIT);
                    fb_rect(bx + EYE_BAR_W, ly, 2, lit, EYE_H1);
                    fb_rect(bx + EYE_BAR_W + 2u, ly, 2, lit, EYE_H2);
                    /* Bloom ABOVE the tip. Without it the strip ends on a
                     * hard edge and the whole thing reads as a rectangle
                     * rather than as something glowing. */
                    if (ly >= by + 2u) {
                        fb_rect(bx - 2u, ly - 1u, EYE_BAR_W + 4u, 1, EYE_H1);
                        fb_rect(bx - 1u, ly - 2u, EYE_BAR_W + 2u, 1, EYE_H2);
                    }

                    /* ...and reflected in the plinth, the way the
                     * photographed pair reflects in its acrylic. */
                    uint32_t rf = (lit * 4u) / bh + 1u;
                    for (uint32_t k = 0; k < 3u; k++)
                        fb_rect(bx - 3u, basy + 1u + k, EYE_BAR_W + 6u, 1,
                                (k < rf) ? ui_mix(basc, EYE_LIT, 3u - k, 9u)
                                         : basc);
                    *shown = lit;
                }

                /* The pool of light beside the tube. See the EYE_GLOW_*
                 * block: fixed centre, elliptical, slow, and only touched
                 * when its quantised level actually changes. */
                uint8_t  step = (uint8_t)((*g * EYE_GLOW_STEPS) / 256u);
                uint32_t pos  = ((uint32_t)lit * EYE_GLOW_POS) / bh;
                uint8_t  key  = (uint8_t)(step * 16u + pos);
                uint8_t *cast = ch ? &eye_cast_r : &eye_cast_l;

                /* Midpoint of the lit strip, held so the pool stays inside
                 * its repaint band. Computed whether or not the pool is
                 * redrawn -- the gap pass needs it either way. */
                uint32_t gcy = by + bh - (pos * bh) / (2u * EYE_GLOW_POS);
                uint32_t top = UI_WAVE_Y + EYE_GLOW_TOP;
                /* Both ends. The upper clamp was dropped while the band was
                 * temporarily shortened and not restored with it, which let
                 * the pool sit five rows lower than intended and run into the
                 * bottom of the box. */
                if (gcy < top + EYE_GLOW_RY) gcy = top + EYE_GLOW_RY;
                if (gcy + EYE_GLOW_RY > top + EYE_GLOW_ROWS)
                    gcy = top + EYE_GLOW_ROWS - EYE_GLOW_RY;
                uint32_t amt = (step * EYE_GLOW_PEAK) / EYE_GLOW_STEPS;

                gcy_of[ch] = gcy; amt_of[ch] = amt; key_of[ch] = key;

                if (!eye_face || key != *cast) {
                    uint32_t bw   = EYE_GLOW_RX / EYE_GLOW_NB;
                    uint32_t base = ch ? (tx + EYE_TUBE_W)
                                       : (tx - EYE_GLOW_RX);

                    for (uint32_t k = 0; k < EYE_GLOW_ROWS; k += EYE_GLOW_S) {
                        uint32_t y   = top + k;
                        uint32_t mid = y + EYE_GLOW_S / 2u;
                        uint32_t dy  = (mid > gcy) ? (mid - gcy) : (gcy - mid);
                        /* The real ramp for this row, matching what the box
                         * is now painted with. An earlier attempt used the
                         * flat `bed` instead: that removed the dark band, and
                         * replaced it with a lighter slab that did not match
                         * the gradient either. The band was never the bug --
                         * the flat box fill was. */
                        uint16_t bg  = ui_grad_at(y);

                        /* Rows outside the pool get rx 0 and fall straight
                         * through to the bed fill, which is what erases the
                         * old position. */
                        uint32_t rx = eye_glow_rx(dy);

                        uint32_t nb = 0;
                        while (nb < EYE_GLOW_NB
                               && (nb * bw + bw / 2u) < rx) nb++;

                        /* The base overhangs the tubes by exactly one band,
                         * so on its rows the innermost band belongs to it and
                         * the pool must not touch it -- neither to light it
                         * nor to erase it. */
                        uint32_t b0 = (y >= basy) ? 1u : 0u;

                        for (uint32_t b = b0; b < nb; b++) {
                            uint32_t d   = b * bw + bw / 2u;   /* from tube */
                            uint32_t wgt = (amt * (rx - d)) / EYE_GLOW_RX;
                            uint32_t gx  = ch
                                         ? (base + b * bw)
                                         : (base + (EYE_GLOW_NB - 1u - b) * bw);
                            fb_rect(gx, y, bw, EYE_GLOW_S,
                                    wgt ? ui_mix(bg, EYE_LIT, wgt, 64u) : bg);
                        }

                        /* Everything the pool does not reach, in ONE rect --
                         * so the whole band is repainted every time and the
                         * pool cleans up after its own previous position,
                         * without a separate erase pass. Bands are 8px and
                         * tile the reach exactly, so the two tubes always
                         * light identical areas. */
                        /* PER ROW, unlike the lit bands above.
                         *
                         * This span is pure background -- it is what makes the
                         * meter continuous with the screen -- so it has to be
                         * the exact ramp, dither and all. Painting it in 4-row
                         * strips like the lit bands gave every strip its top
                         * row's colour, which reads as horizontal stepping
                         * against the smoothly dithered box around it, worst
                         * near the bottom where the ramp is darkest.
                         *
                         * The lit bands can stay quantised: the cyan mixed
                         * into them dominates, and any step is invisible under
                         * it. Costs ~42 extra rects a channel on a redraw. */
                        uint32_t r0 = (nb > b0) ? nb : b0;
                        if (r0 < EYE_GLOW_NB) {
                            uint32_t rx0 = ch ? (base + r0 * bw) : base;
                            uint32_t rw  = (EYE_GLOW_NB - r0) * bw;
                            for (uint32_t r = 0; r < EYE_GLOW_S; r++)
                                fb_rect(rx0, y + r, rw, 1, ui_grad_at(y + r));
                        }
                    }
                    *cast = key;
                }
            }

            /* The gap, lit by BOTH tubes. See EYE_GAP_ROWS. */
            if (!eye_face || key_of[0] != eye_gap_l
                          || key_of[1] != eye_gap_r) {
                uint32_t gx  = x0 + EYE_TUBE_W;
                uint32_t top = UI_WAVE_Y + EYE_GLOW_TOP;
                /* Distance from each tube's inner face to the middle of the
                 * gap -- the same for both, which is why the sum is flat. */
                uint32_t d = EYE_TUBE_G / 2u;

                for (uint32_t k = 0; k < EYE_GAP_ROWS; k += EYE_GLOW_S) {
                    uint32_t y = top + k;
                    uint32_t h = EYE_GAP_ROWS - k;
                    if (h > EYE_GLOW_S) h = EYE_GLOW_S;

                    uint32_t wgt = 0;
                    for (uint32_t ch = 0; ch < 2u; ch++) {
                        uint32_t mid = y + h / 2u;
                        uint32_t dy  = (mid > gcy_of[ch])
                                     ? (mid - gcy_of[ch])
                                     : (gcy_of[ch] - mid);
                        uint32_t rx  = eye_glow_rx(dy);
                        if (d < rx)
                            wgt += (amt_of[ch] * (rx - d)) / EYE_GLOW_RX;
                    }
                    /* Two sources add, but not without limit -- past this the
                     * gap stops reading as lit glass and starts reading as a
                     * solid block between the tubes. */
                    if (wgt > 40u) wgt = 40u;

                    uint16_t bg = ui_grad_at(y);
                    fb_rect(gx, y, EYE_TUBE_G, h,
                            wgt ? ui_mix(bg, EYE_LIT, wgt, 64u) : bg);
                }
                eye_gap_l = key_of[0];
                eye_gap_r = key_of[1];
            }

            eye_face   = 1;
            eye_face_w = (uint16_t)ww;
            goto viz_done;
        }

        /* ---- OSCILLOSCOPE -------------------------------------------------
         * One clear, then one vertical rect per column: ~65 commands, fewer
         * than the bars. */
        if (viz_mode == VIZ_WAVE) {
            const int32_t ey = (int32_t)(UI_WAVE_H / 2u) - 1;
            const uint32_t cy = UI_WAVE_Y + UI_WAVE_H / 2u;

            ui_bg_restore(UI_MARGIN, UI_WAVE_Y, ww, UI_WAVE_H);
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

            ui_bg_restore(UI_MARGIN, UI_WAVE_Y, ww, UI_WAVE_H);

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
                    /* Blended from the background, so it has to be the
                     * background near where the trace actually sits -- the
                     * dots cluster around the centre line. Per-dot would cost
                     * a call for each of 48 x 4. */
                    uint16_t c  = ui_mix(ui_grad_at(cy), ui_accent,
                                         SCOPE_HIST - age, SCOPE_HIST);
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

ui_tail:
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

        /* 1.2x, while it is on. Right-aligned on this row, which puts it
         * directly under the track counter -- that is drawn right-aligned on
         * the transport row above, and the elapsed time only reaches about
         * halfway across, so the space is free.
         *
         * Drawn HERE rather than anywhere else because the clear above spans
         * the full inner width every second; anything painted on this row
         * outside this block is erased within a second of appearing. A toggle
         * forces ui_last_sec, so it shows up on the press rather than on the
         * next tick.
         *
         * Accent, not white: it is a state the user chose, and the same colour
         * every other active mode indicator uses. */
        if (speed_fast) {
            const char *sp = "1.2x";
            uint32_t sw = fb_text_width(sp, TS_1X);
            uint32_t sx = FB_W - UI_MARGIN - sw;
            uint32_t sy = UI_TIME_Y + (FB_CELL(TS_15X) > FB_CELL(TS_1X)
                                     ? (FB_CELL(TS_15X) - FB_CELL(TS_1X)) / 2u : 0u);
            fb_set_color(ui_accent, cbg);
            fb_text_clipped(sx, sy, sp, TS_1X, TS_1X, sw + 2u);
        }
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
            for (uint32_t z = 0; z < SPEC_BANDS; z++) spec_drawn[z] = 0xFFu;
                }
            ui_art_draw();
        }
    }

    /* Marquee. Steps by whole characters rather than pixels: the engine draws
     * a glyph at any x but does not clip one partially off the left edge, so a
     * pixel scroll would need clipping support that does not exist. One step
     * every ~350 ms reads as a scroll without being distracting. */
    if (!pl_ui_open) {          /* title/artist rows are under the overlay */
        ui_marq_step(&ui_mq_title,  UI_WHITE);
        ui_marq_step(&ui_mq_artist, UI_DIM);
    }

    /* Loud, and it stays: a wrong file size means the card's directory is
     * damaged, which will not fix itself and puts every file in that folder in
     * question -- not something to mention in a toast that scrolls away. */
    if (size_suspect && !ui_size_warned && !pl_ui_open) {
        ui_size_warned = 1;
        fb_set_color(UI_RED, UI_PANEL);
        fb_text_clipped(UI_MARGIN, ui_info_y, "! FILE SIZE WRONG - CHECK SD CARD",
                        TS_1X, TS_1X, ui_text_w);
    }

    /* Format line, drawn once the decoder has told us what the stream is. */
    uint32_t info = track_kbps * 1000u + track_hz / 100u
                  + (track_encoder[0] ? (uint32_t)track_encoder[0] << 24 : 0u);
    /* Work it out HERE if it is still unknown.
     *
     * track_kbps is derived from slot_size, and slot_size is no longer known at
     * load time -- the size search runs during playback now. Computing it only
     * where the search happens to finish made the row's appearance depend on
     * WHEN that was, and it showed up unreliably. This asks the question every
     * time the row is considered instead, so the answer appears the moment it
     * exists, whichever path produced it.
     *
     * Cheap: one compare while it is unknown, nothing at all afterwards. */
    if (!track_kbps && track_fmt == FMT_FLAC) {
        uint32_t kb = 0;
        if (track_secs && slot_size > fl_first_frame) {
            /* Best answer: the whole file over its whole duration. */
            uint64_t bits = (uint64_t)(slot_size - fl_first_frame) * 8u;
            kb = (uint32_t)(bits / (uint64_t)track_secs / 1000u);
        } else if (ui_sec >= 3u && file_pos > fl_first_frame) {
            /* Otherwise ask the DECODER, which has been counting all along:
             * bytes consumed over seconds played is the average bitrate of
             * what has been heard, and it converges on the file's own.
             *
             * This exists because waiting for slot_size kept failing in ways
             * that were invisible -- the row simply did not appear. The size
             * arrives from a probe that has been moved twice, gated twice and
             * broken twice; file_pos and ui_sec are both already true by the
             * time anyone can read this row. Deriving from what is known beats
             * waiting for what might arrive.
             *
             * Survives a seek: both jump together, so the ratio still measures
             * from the start of the file. */
            uint64_t bits = (uint64_t)(file_pos - fl_first_frame) * 8u;
            kb = (uint32_t)(bits / (uint64_t)ui_sec / 1000u);
        }
        if (kb) {
            track_kbps = kb;
            info = track_kbps * 1000u + track_hz / 100u
                 + (track_encoder[0] ? (uint32_t)track_encoder[0] << 24 : 0u);
        }
    }

    if (track_kbps && info != ui_last_info && !size_suspect && !pl_ui_open) {
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
            /* Bound by the BUFFER, not by a number that happened to work.
             * This was `q - b < 30`, and the prefix "430 kbps - 44.1 kHz - "
             * is 22 characters, so the codec field got exactly 8 -- which is
             * the length of "LAME3.99", so MP3 fit by luck and nothing ever
             * looked wrong. FLAC writes "FLAC 16-bit" and it showed as
             * "FLAC 16-" on every lossless track. b is char[40] and the whole
             * line is 34, so the space was always there. */
            for (const char *t = track_encoder;
                 *t && q < b + sizeof(b) - 1u; ) *q++ = *t++;
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
        if (done) {
            fb_rect(UI_MARGIN, UI_PROG_Y, done, UI_PROG_H, ui_accent);
            /* One lit row along the top of the filled part, so the bar has a
             * direction to it instead of reading as a flat block. A third of
             * the way to white -- enough to catch the eye at 5 px tall,
             * little enough that it still reads as the accent colour.
             *
             * Free in practice: this band is only redrawn when `done` moves,
             * which is about once a second. */
            fb_rect(UI_MARGIN, UI_PROG_Y, done, 1u,
                    ui_mix(ui_accent, UI_WHITE, 1u, 3u));
        }
        if (UI_INNER_W > done)
            fb_rect(UI_MARGIN + done, UI_PROG_Y, UI_INNER_W - done,
                    UI_PROG_H, UI_TRACK);

        /* Round the OUTER ends of the whole bar, after both segments are down.
         * Cutting them here rather than drawing two rounded rects is what
         * keeps the join invisible: the filled part must meet the unfilled
         * part square in the middle, and only the two ends of the assembly are
         * ever a shape. At 5 px tall a radius of 2 is a 2 px bite from the top
         * and bottom rows and 1 px from the next -- a soft end rather than a
         * pill, which is all there is room for. */
        {
            const uint32_t r = UI_PROG_H / 2u;
            uint16_t bg = ui_grad_at(UI_PROG_Y);
            for (uint32_t i = 0; i < r; i++) {
                uint32_t dy = r - i, inner = 0;
                while ((inner + 1u) * (inner + 1u) + dy * dy <= r * r) inner++;
                uint32_t cut = r - inner;
                if (!cut) continue;
                fb_rect(UI_MARGIN, UI_PROG_Y + i, cut, 1, bg);
                fb_rect(UI_MARGIN + UI_INNER_W - cut, UI_PROG_Y + i, cut, 1, bg);
                fb_rect(UI_MARGIN, UI_PROG_Y + UI_PROG_H - 1u - i, cut, 1, bg);
                fb_rect(UI_MARGIN + UI_INNER_W - cut,
                        UI_PROG_Y + UI_PROG_H - 1u - i, cut, 1, bg);
            }
        }

        /* Position knob -- also the visual handle the seek controls move.
         *
         * A vertical marker, not a disc. A round handle was tried and looked
         * wrong here: at 5 px of bar it has to overhang so far to read as
         * round that it stops being a mark on a track and becomes a blob
         * sitting over one. Taller than it is wide is what makes it read as a
         * position rather than an object. Drawn last, so the end rounding
         * above cannot bite it. */
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
    /* NOT while a boot note is up. UI_BOOT_Y and UI_TRANSPORT_Y are the SAME
     * ROW -- both 262 -- so during a switch this repainted PLAYING, the
     * arrows and the EQ name straight over "LOADING PLAYLIST" while
     * ui_boot_tick() animated its dots on top of both. The user saw the
     * transport line garbled, assumed the pick had failed, and picked again:
     * that is a large part of what "I have to load it twice" has been.
     *
     * The note owns the row until ui_boot_cancel(), whose callers already
     * force a full transport repaint after it. */
    if (!ui_boot_msg) {
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
            fb_rect(mx, iy, (UI_MODE_W + 10u) * 3u + 106u, UI_ICONBOX_H, tbg);
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
            /* Volume, as a level rather than a number. Faint at mute, the
             * same "off but still visible" treatment repeat and shuffle use
             * when they are off. */
            {
                uint32_t vl = (volume == 0u)   ? 0u
                            : (volume <= 33u)  ? 1u
                            : (volume <= 66u)  ? 2u : 3u;
                ui_icon_speaker(mx + (UI_MODE_W + 10u) * 2u, my, vl,
                                vl ? ui_accent : UI_FAINT);
            }

            fb_set_color(eq_idx ? ui_accent : UI_FAINT, tbg);
            fb_text_clipped(mx + (UI_MODE_W + 10u) * 3u, my - 2u,
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
        /* Latch and reset the attribution for the second just finished. */
        fl_idle_pct = (uint8_t)(fl_idle_cyc / (CLK_HZ / 100u));
        fl_io_pct   = (uint8_t)(fl_io_cyc   / (CLK_HZ / 100u));
        if (fl_idle_pct > 99u) fl_idle_pct = 99u;
        if (fl_io_pct   > 99u) fl_io_pct   = 99u;
        fl_idle_cyc = fl_io_cyc = 0u;
        char b[64], *q = b;
        /* The speed prefix this row was built for is dropped while the
         * playlist switch is under investigation: measured against the real
         * font, the full line clips past 296px once the counters reach two
         * digits, and N is already at 8. */
        /* The playlist fields are gone: that investigation closed with the
         * poll fix confirmed on hardware, and this row is now the only way to
         * tell WHY a FLAC file hiccups.
         *
         * D = idle, waiting on a full FIFO -- spare CPU.
         * O = blocked in flac_pull waiting for bytes -- starved of input.
         * U = sticky underruns, the audible fault itself.
         *
         * Read D and O together. High D with underruns means the decoder is
         * fast enough and the ring is the problem; both near zero means the
         * decoder is not fast enough. Those need opposite fixes, and without
         * this row the two are indistinguishable from the couch. */
        /* L/R/P are retired: they did their job -- the 48 kHz gate is set
         * from the numbers they produced -- and FLAC_PROFILE is now 0. What
         * remains is what the one OPEN defect needs: F names why a load
         * failed. */
        *q++ = 'D'; q = ui_dec(q, fl_idle_pct);
        *q++ = ' '; *q++ = 'O'; q = ui_dec(q, fl_io_pct);
        *q++ = ' '; *q++ = 'U'; q = ui_dec(q, pcm_under_n);
        *q++ = ' '; *q++ = 'F'; q = ui_dec(q, fl_open_err);
        *q++ = '/'; q = ui_dec(q, fl_open_fails);
        *q = 0;
        uint16_t sbg = ui_grad_at((FB_H - 24u));
        fb_rect(UI_MARGIN, FB_H - 24u, UI_INNER_W, FB_CELL(TS_1X), sbg);
        fb_set_color(UI_RED, sbg);
        fb_text_clipped(UI_MARGIN, FB_H - 24u, b, TS_1X, TS_1X, UI_INNER_W);
    }
#endif

#if UI_SHOW_RESUME_DIAG
    {
        char b[40], *q = b;
        *q++ = 'Y'; q = ui_dec(q, resume_dbg);
        *q++ = ' '; *q++ = 'N'; q = ui_dec(q, resume_saves);
        *q++ = ' '; *q++ = 'A'; q = ui_dec(q, resume_at);
        *q++ = ' '; *q++ = 'S'; q = ui_dec(q, ui_sec);
        *q++ = ' '; *q++ = 'T'; q = ui_dec(q, pl_pos);
        *q = 0;
        uint16_t g = ui_grad_at((FB_H - 24u));
        fb_rect(UI_MARGIN, FB_H - 24u, UI_INNER_W, FB_CELL(TS_1X), g);
        fb_set_color(UI_RED, g);
        fb_text_clipped(UI_MARGIN, FB_H - 24u, b, TS_1X, TS_1X, UI_INNER_W);

    }
#endif

#if UI_SHOW_SEEK_DIAG
    /* Two rows, redrawn every pass so nothing erases them, and frozen on the
     * last seek so there is time to read them. Toasts are suppressed in this
     * build (see ui_toast_msg) because the toast band would sit on row A.
     *
     *   row A   T target second the seek asked for
     *           A byte it jumped to, in KB
     *           B max_blocksize      R sample rate / 100
     *           S blocking strategy: 0 fixed (frame numbers), 1 variable
     *   row B   N the coded number of the first three frames decoded after
     *           P second those imply for the FIRST of them
     *           U ui_sec as it stands now
     *           E decoder result on the first frame, 0 = OK
     *
     * How to read it: P should be close to T. If P is wildly off, the frame
     * the scan landed on is not the one the seek aimed at. If the three N
     * values are not consecutive, the scan is landing on false syncs. If B or
     * R read zero, the position arithmetic never had valid inputs -- which
     * would explain both failed fixes at a stroke. */
    {
        /* Row A is everything a seek DEPENDS ON, so the same file loaded two
         * ways can be compared field by field. Reported: seeking works after
         * Load MP3 and not from a .m3u, which means one of these differs. */
        char b[64], *q = b;
        *q++ = 'Z'; q = ui_dec(q, dg_size >> 10);        /* file size, KB   */
        *q++ = ' '; *q++ = 'D'; q = ui_dec(q, dg_dur);   /* track_secs      */
        *q++ = ' '; *q++ = 'F'; q = ui_dec(q, dg_first >> 10);
        *q++ = ' '; *q++ = 'K'; q = ui_dec(q, dg_pts);   /* seek points     */
        *q++ = ' '; *q++ = 'L'; q = ui_dec(q, dg_len);   /* STREAMINFO secs */
        *q = 0;
        uint16_t g = ui_grad_at((FB_H - 40u));
        fb_rect(UI_MARGIN, FB_H - 40u, UI_INNER_W, FB_CELL(TS_1X), g);
        fb_set_color(UI_RED, g);
        fb_text_clipped(UI_MARGIN, FB_H - 40u, b, TS_1X, TS_1X, UI_INNER_W);

        q = b;
        *q++ = 'T'; q = ui_dec(q, dg_tgt);               /* asked for       */
        *q++ = ' '; *q++ = 'P'; q = ui_dec(q, dg_pos);   /* measured landing*/
        *q++ = ' '; *q++ = 'U'; q = ui_dec(q, ui_sec);
        *q++ = ' '; *q++ = 'I'; q = ui_dec(q, dg_intent);
        *q++ = ' '; *q++ = 'p'; q = ui_dec(q, dg_pn);      /* probes tried  */
        *q++ = '/'; q = ui_dec(q, dg_pfail);               /* read/parse bad */
        *q++ = '/'; q = ui_dec(q, dg_prej);                /* false syncs    */
        *q++ = ' '; *q++ = 'E'; q = ui_dec(q, dg_fe == 0xFFu ? 99u : dg_fe);
        *q = 0;
        g = ui_grad_at((FB_H - 24u));
        fb_rect(UI_MARGIN, FB_H - 24u, UI_INNER_W, FB_CELL(TS_1X), g);
        fb_set_color(UI_RED, g);
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

/* Feeds every meter from one frame of interleaved PCM.
 *
 * Lifted out of the MP3 loop so the FLAC path drives the SAME nine meters
 * rather than growing a second, subtly different implementation of them. Takes
 * the buffer explicitly: MP3 hands it Helix's output, FLAC hands it a window
 * captured in flac_emit. */
static void meters_feed(const short *pcm, int n, int stereo)
{
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
        /* Accumulate rather than assign -- see peak_acc. ui_draw_dynamic()
         * publishes the maximum once per display frame, so a chunk that lands
         * between frames still counts instead of being overwritten. */
        if ((uint32_t)pk  > peak_acc)   peak_acc   = (uint32_t)pk;
        if ((uint32_t)pkl > peak_acc_l) peak_acc_l = (uint32_t)pkl;
        if ((uint32_t)pkr > peak_acc_r) peak_acc_r = (uint32_t)pkr;
        peak_acc_any = 1u;

        /* The octave cascade, only while its meter is showing -- see
         * SPEC_BANDS. One pass down the ladder per sample, and most samples
         * stop after a stage or two, because the lower stages run at a
         * fraction of the rate. */
        if (viz_mode == VIZ_LED) {
            for (int i = 0; i < n; i += (stereo ? 2 : 1)) {
                int32_t x = stereo ? (((int32_t)pcm[i] + (int32_t)pcm[i + 1]) >> 1)
                                   : (int32_t)pcm[i];
                for (uint32_t o = 0; o < SPEC_OCT; o++) {
                    spec_lp[o] += (x - spec_lp[o]) >> SPEC_SH;
                    int32_t hp = x - spec_lp[o];

                    /* Split the octave in two. Extending the cascade instead
                     * does NOT give more bands worth having: every extra stage
                     * halves the frequency AND the rate, so stage 8 is already
                     * at 86 Hz with four samples per window and stage 12 is at
                     * 5 Hz with a quarter of one. There are only about eight
                     * audible octaves; resolution has to come from inside them
                     * rather than below them. */
                    spec_slp[o] += (hp - spec_slp[o]) >> SPEC_SH2;
                    int32_t sl = spec_slp[o];
                    int32_t sh = hp - sl;

                    spec_acc[o * 2u]      += (uint32_t)(sh < 0 ? -sh : sh);
                    spec_acc[o * 2u + 1u] += (uint32_t)(sl < 0 ? -sl : sl);

                    if (++spec_cnt[o] & 1u) break;   /* half rate below here */
                    x = spec_lp[o];
                }
            }
            spec_n += (uint32_t)(stereo ? (n / 2) : n);
        }

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

}


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
    /* ui_draw_chrome just painted the PLAYER. If the overlay was up when the
     * screen blanked it is still logically open and still eating the d-pad,
     * so without this the user would be left driving an invisible list. */
    if (pl_ui_open) pl_ui_dirty = 1u;
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
    static uint32_t sel_t0;              /* when Select went down              */
    static uint8_t  sel_held;            /* the hold action already ran        */
    static uint32_t pl_rep_at;           /* next overlay scroll repeat         */
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

    /* ---- the overlay owns most of the pad while it is up -------------------
     * Handled BEFORE every normal binding, then the consumed bits are cleared
     * so nothing downstream also acts on them. Select is deliberately left in
     * `keys`/`fall`: its tap-to-close is the same code that opened it, and its
     * hold timer reads `keys` directly. */
    if (pl_ui_open && !pl_count) {      /* list emptied underneath it */
        pl_ui_open = 0u; pl_ui_restore = 1u;
    }
    if (pl_ui_open) {
        if (edge & KEY_UP) {
            pl_ui_sel = pl_ui_sel ? (uint16_t)(pl_ui_sel - 1u)
                                  : (uint16_t)(pl_count - 1u);
            pl_ui_follow(); pl_ui_dirty = 1u;
        }
        if (edge & KEY_DOWN) {
            pl_ui_sel = (uint16_t)((pl_ui_sel + 1u) % pl_count);
            pl_ui_follow(); pl_ui_dirty = 1u;
        }
        /* Auto-repeat while held. A 256-entry list is unusable at one row per
         * press, and the d-pad has no other job here. First step on the edge,
         * then a hold delay, then steady -- the same shape as the seek. */
        if (edge & (KEY_UP | KEY_DOWN))
            pl_rep_at = cycles() + CLK_HZ / 1000u * PL_HOLD_MS;
        if ((keys & (KEY_UP | KEY_DOWN)) &&
            (int32_t)(cycles() - pl_rep_at) >= 0) {
            pl_rep_at = cycles() + CLK_HZ / 16u;          /* ~16 rows a second */
            if (keys & KEY_UP)
                pl_ui_sel = pl_ui_sel ? (uint16_t)(pl_ui_sel - 1u)
                                      : (uint16_t)(pl_count - 1u);
            else
                pl_ui_sel = (uint16_t)((pl_ui_sel + 1u) % pl_count);
            pl_ui_follow(); pl_ui_dirty = 1u;
        }

        /* Left and Right on the D-PAD page by a screenful -- more intuitive
         * than the shoulder buttons, and they have no other job while the
         * overlay is up: everything except Select is masked off at the end of
         * this block, so the seek scrub never sees them. 240 entries is 27
         * pages against 240 steps.
         *
         * This is the second time it has been written. The first was undone by
         * `git checkout 1c1d55a -- fw/player.c` while reverting an unrelated
         * seek change, and went unnoticed because the changelog already
         * described the intended behaviour rather than the shipped one. */
        if (edge & KEY_LEFT) {
            pl_ui_sel = (pl_ui_sel > PL_UI_ROWS) ? (uint16_t)(pl_ui_sel - PL_UI_ROWS) : 0u;
            pl_ui_follow(); pl_ui_dirty = 1u;
        }
        if (edge & KEY_RIGHT) {
            uint32_t n = (uint32_t)pl_ui_sel + PL_UI_ROWS;
            pl_ui_sel = (n >= pl_count) ? (uint16_t)(pl_count - 1u) : (uint16_t)n;
            pl_ui_follow(); pl_ui_dirty = 1u;
        }
        /* Y snaps back to what is playing. Scrolling a long list loses the one
         * row you can always name, and hunting for it defeats the point. */
        if (edge & KEY_Y) { pl_ui_sel = pl_pos; pl_ui_follow(); pl_ui_dirty = 1u; }

        if (edge & KEY_A) { pl_ui_play_req = 1u; pl_ui_open = 0u; pl_ui_restore = 1u; }
        if (edge & KEY_B) { pl_ui_open = 0u; pl_ui_restore = 1u; }
        edge &= KEY_SELECT;
        fall &= KEY_SELECT;
        keys &= KEY_SELECT;
    }

    /* A plays and pauses. Start only ever STOPS -- pressing it again does
     * nothing, which is what separates it from pause: stop is a state you
     * leave with play, not a toggle. Stopping also returns to 0:00. */
    /* A: TAP plays/pauses, LONG HOLD toggles 1.2x speed.
     *
     * 1.2x is a MODE -- it has always toggled rather than being momentary. The
     * problem was never that it toggled, it was how easy the gesture was to
     * perform by accident: the hold shared PL_HOLD_MS with the scrub, so
     * lingering on the play button put people into 1.2x with nothing on screen
     * to say so. SPEED_HOLD_MS is three times as long, and the indicator on the
     * time row makes the mode visible while it is on.
     *
     * Resolves on RELEASE, the same discipline Left/Right and Select already
     * use. Firing the tap action on the press instead would mean a long press
     * pauses AND changes speed -- it is on the way into every hold. */
    {
        static uint32_t a_t0;
        static uint8_t  a_fired;      /* the hold action already ran this press */
        const uint32_t  a_hold_cy = CLK_HZ / 1000u * SPEED_HOLD_MS;

        if (edge & KEY_A) { a_t0 = cycles(); a_fired = 0; }

        if ((keys & KEY_A) && !a_fired &&
            (int32_t)(cycles() - a_t0) >= (int32_t)a_hold_cy) {
            a_fired = 1;
            speed_fast ^= 1u;
            pcm_rate_apply(track_hz);
            /* Name the speed. An unlabelled 1.2x just sounds like a bad rip,
             * and the only other clue is the elapsed clock running fast. */
            ui_toast_msg(speed_fast ? "SPEED 1.2x" : "SPEED NORMAL");
            /* Repaint the indicator now rather than at the next second tick:
             * it is drawn with the elapsed time, which only redraws when the
             * seconds change. */
            ui_last_sec = 0xFFFFFFFFu;
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
         * modes wrap in a handful of taps, and every Select combo the user has to
         * remember costs more than it saves. */
        viz_mode = (uint8_t)((viz_mode + 1u) % VIZ_COUNT);
        ui_wave_clear();                 /* modes do not share a screen layout */
        ui_wave_force = 1u;
        for (uint32_t i = 0; i < UI_WAVE_N; i++) {
            wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
            for (uint32_t z = 0; z < SPEC_BANDS; z++) spec_drawn[z] = 0xFFu;
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
                   : viz_mode == VIZ_DOTS   ? "METER: PEAK DOTS"
                   : viz_mode == VIZ_EYE    ? "METER: MAGIC EYE"
                                            : "METER: SPECTRUM");
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
                if (k < 3u) b[i++] = ' ';
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
            const char *lbl = "HSAT";
            const uint16_t v[4] = { ld_head, ld_size, ld_art, ld_total };
            for (uint32_t k = 0; k < 4u && i + 6u < sizeof(b); k++) {
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
                               ui_mode_dirty = 1u;   /* the speaker icon */
                               settings_mark_dirty(); }
        if (edge & KEY_DOWN) { volume = (volume < VOL_STEP) ? 0u : volume - VOL_STEP;
                               vol_apply(); ui_toast_set("VOLUME", volume, "%");
                               ui_mode_dirty = 1u;   /* the speaker icon */
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
    if (edge & KEY_SELECT) { sel_used = 0; sel_t0 = cycles(); sel_held = 0; }

    /* HOLD toggles the art panel, which is what a TAP used to do. The tap now
     * opens the playlist, and this fires on the threshold rather than on
     * release so the panel moves while the button is still down -- otherwise
     * a hold and a tap feel identical until you let go. */
    if ((keys & KEY_SELECT) && !sel_used && !sel_held &&
        (int32_t)(cycles() - sel_t0) >= (int32_t)(CLK_HZ / 1000u * PL_HOLD_MS)) {
        sel_held    = 1u;
        art_toggle  = 1u;
    }

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

    /* A Select that was neither a modifier nor a hold is a TAP: the playlist.
     * Opening lands the cursor on what is playing, which is the row a user
     * wants nine times in ten. */
    if ((fall & KEY_SELECT) && !sel_used && !sel_held) {
        if (pl_ui_open) { pl_ui_open = 0u; pl_ui_restore = 1u; }
        else if (pl_count) {
            pl_ui_open = 1u;
            pl_ui_sel  = pl_pos;
            pl_ui_follow();
            pl_ui_dirty = 1u;
        } else {
            ui_toast_msg("NO PLAYLIST LOADED");
        }
    }

    /* Pause while the OS menu ("Load MP3" etc) is open, without clobbering the
     * user's own A/Start pause -- bit 1 is the menu's, bit 0 is theirs. */
    /* Opening the OS menu is the strongest signal that the core is about to
     * be left, so a pending settings write goes out now rather than waiting
     * out its quiet window that may never elapse. */
    /* The open/closed memory is its OWN flag, not paused's menu bit.
     * load_track() ends with `paused = 0`, which clears that bit wholesale --
     * so any load running while the menu was open erased the evidence and the
     * closing edge below never fired. */
    if (in & IN_MENU) {
        if (!menu_was) { set_flush_now = 1u; menu_was = 1u; }
        paused |= 2u;
    } else {
        /* CLOSING edge: the moment a Load Playlist pick has just been made. */
        if (menu_was) { pl_check_req = 1u; menu_was = 0u; }
        paused &= ~2u;
    }

    /* Only SET the flag here. This runs from inside the sample-push wait,
     * which is where the CPU spends most of its time when keeping up, so a
     * reload is noticed immediately instead of after the current frame. */
    /* An MP3-slot notification also re-checks the PLAYLIST slot. The RTL
     * decides which slot an 008A belongs to by comparing a slot id that
     * crosses clock domains beside the toggle carrying the event, so a
     * mis-attributed update would be delivered as the wrong slot's and lost. */
    if (REG(R_RELOAD) & RL_PENDING) { reload_pending = 1u; pl_check_req = 1u; }
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


/* ------------------------------------------------- the same search, in steps
 *
 * probe_file_size() below is ~20 blocking reads, measured at 480 ms, and it
 * runs inside load_track(). That is 480 ms of every load of a file that does
 * not declare its own length -- which is every FLAC, since only MP3 carries a
 * Xing header -- and until this it was ALSO paid again on the first seek of a
 * track, because the load-time answer is wrong for a file opened by name.
 *
 * Same binary search, one read per call, driven from the main loop while the
 * ring is full. The size is not needed immediately: it feeds the progress bar,
 * the bitrate readout and the seek bracket, none of which matter in the first
 * second of playback. It is needed CORRECTLY, though, and a probe that runs a
 * little later is the one that gets the right answer -- a file opened with
 * 0192 has only just been opened at load, and a random read far into it still
 * fails, which is how a 30 MB track measured 5 MB.
 *
 * Phases: 0 idle, 1 the clamp reference, 2 doubling to bracket the end,
 * 3 bisecting, 4 done. */
static uint8_t  szp_phase;
static uint32_t szp_lo, szp_hi;

static void size_probe_arm(void)
{
    szp_phase = 1u;
    szp_lo = 0; szp_hi = 1u << 22;      /* start at 4 MB, as the blocking one does */
}

/* How far playback must have got before the search may start.
 *
 * This is the whole reason the measurement was wrong before. A file opened by
 * name with 0192 does not answer a random read far into it straight away, so a
 * probe at load time stops early and reports a fraction of the true size -- 5
 * MB of a 30 MB track, measured. Waiting for a quarter of a megabyte to have
 * been READ is direct evidence the slot is streaming properly, and it is a
 * better gate than a timer because it is the same thing the probe depends on.
 * A file smaller than this reaches eof_hit instead, and refill_pump() records
 * the true end there for free. */
#define SZP_START_AFTER (256u * 1024u)

/* One step. Returns 1 when a size has just been established. */
static int size_probe_step(void)
{
    /* Measured from the first AUDIO byte, not from the start of the file.
     *
     * It was `file_pos < SZP_START_AFTER`, which stopped working the moment
     * flac_open learned to skip metadata: on these files the cover art is
     * 642 KB, so the skip lands file_pos past a 256 KB gate during the LOAD --
     * before a single audio byte has been read. The probe then ran immediately,
     * which is exactly the too-early case the gate exists to prevent, and a
     * short answer followed. Below fl_first_frame it makes the bitrate
     * uncomputable and the format row stays blank; above it, the row shows a
     * wrong number. That is the "works on some FLACs" report.
     *
     * Against fl_first_frame the gate means what it always meant: a quarter of
     * a megabyte of AUDIO has been streamed, so the slot has settled. */
    if (szp_phase == 1u && !eof_hit &&
        file_pos < fl_first_frame + SZP_START_AFTER) return 0;

    switch (szp_phase) {
    case 1:
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
        if (!probe_readable(0)) { szp_phase = 4u; return 0; }
        szp_phase = 2u;
        return 0;

    case 2:
        if (szp_hi < (1u << 26) && probe_readable(szp_hi)) {
            szp_lo = szp_hi; szp_hi <<= 1;
            return 0;
        }
        if (szp_hi >= (1u << 26)) { szp_phase = 4u; return 0; }   /* runaway */
        szp_phase = 3u;
        return 0;

    case 3:
        if (szp_hi - szp_lo > 4096u) {
            uint32_t mid = szp_lo + (szp_hi - szp_lo) / 2u;
            if (probe_readable(mid)) szp_lo = mid; else szp_hi = mid;
            return 0;
        }
        szp_phase = 4u;
        /* Only ever grows it. An early probe stops where a read first fails,
         * so it can fall short of the true end but never run past it, and a
         * size that came from APF directly is better than any measurement. */
        if (szp_lo + 4096u > slot_size) {
            slot_size = szp_lo + 4096u;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

/* Measure the file. APF only reports a size with a RELOAD notification, so a
 * track loaded at boot has none -- which is why the progress bar and the
 * end-of-track check need this. Binary-searching the last readable offset
 * needs no notification, no struct layout and no status semantics.
 *
 * Kept blocking for the one caller that cannot wait: the seek backstop, where
 * the answer is needed before the seek it was asked for. */
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

/* Where APF's 0190 response lands in the datatable.
 *
 * These used to live in playlist.inc, which is included further down -- so
 * slot_file_id() and slot_filename() below could not see them and were still
 * written against the ORIGINAL layout, with the response at word 0. It was
 * moved to word 64 because APF's dataslot ID/size table occupies the start of
 * this BRAM and every 0190 was overwriting it; these two never followed.
 *
 * They have therefore been reading the ID/SIZE TABLE this whole time: binary
 * pairs with no text in them, so slot_filename() found no printable run and
 * track_file came back EMPTY on every load. That is why an untagged file
 * showed UNKNOWN TRACK instead of its name. */
#define DT_RESP_W   64u     /* response struct  (APF writes) */
#define DT_PARAM_W  128u    /* parameter struct (APF reads)  */
#define DT_WORDS    64u     /* 256 bytes, matching slot_filename()'s window */

static void slot_filename(char *out, uint32_t out_size);

/* Ask APF which file is CURRENTLY in the slot (0190) and reduce its response
 * to one number. This is the authoritative answer to "has the slot changed
 * yet?" -- every content-based guess at that was defeated by a read that was
 * wrong but stable. HASHES the struct rather than parsing it, so nothing here
 * depends on a field offset that would otherwise be guesswork. */
/* `want_name` distinguishes the two callers. A LOAD wants the filename as a
 * side effect; the periodic identity poll must not have it, or it would
 * overwrite the playing track's name with the slot's before the load that
 * makes it true. */
static uint32_t slot_id_query(int want_name)
{
    /* refill_drain() waits out an in-flight read and DISCARDS it -- see its own
     * comment. That is right for a caller about to move the ring, and wrong for
     * a poll that runs every couple of seconds during playback: it would throw
     * away a 4 KB read each time and force it to be fetched again.
     *
     * Both callers now gate on !rd_pending, so this is a no-op in the steady
     * state. Kept because the command layer requires exactly one in flight, and
     * a future caller must not have to know that. */
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

    if (want_name) slot_filename(track_file, sizeof(track_file));

    uint32_t h = 2166136261u;                   /* FNV-1a over the struct */
    for (uint32_t w = 0; w < DT_WORDS; w++) {
        uint32_t v = dt_read(DT_RESP_W + w);
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (b * 8)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;                          /* keep 0 for "unknown" */
}

static uint32_t slot_file_id(void) { return slot_id_query(1); }

/* Is the slot pointing somewhere other than what is loaded?
 *
 * The playlist slot has had a periodic identity poll since the double-load bug
 * (pl_poll_at); the TRACK slot never got one, and relies entirely on the 008A
 * notification. When that notification goes missing the load simply does not
 * happen and nothing recovers it -- which is the "picked a song, waited, had
 * to pick it again" fault, the same shape as the playlist bug on the path that
 * was never fixed. */
static int slot_changed(void)
{
    uint32_t id = slot_id_query(0);
    return id && cur_file_id && id != cur_file_id;
}

/* Pull the filename out of the 0190 response WITHOUT knowing its layout: the
 * longest run of printable ASCII in the struct IS the name. */
static void slot_filename(char *out, uint32_t out_size)
{
    uint8_t raw[DT_WORDS * 4u];
    for (uint32_t w = 0; w < DT_WORDS; w++) {
        uint32_t v = dt_read(DT_RESP_W + w);
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


/* Which decoder the current track needs. Detected from the file's first four
 * bytes, not its extension -- a .flac that is really an MP3 should play, and a
 * mislabelled file should not silently fail. */
/* Declared up with the other track state -- see fl_first_frame. */
static flac_t   fl;
static int32_t *fl_buf;            /* one blocksize of int32, from the arena */

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


#if IO_BENCH
static void io_bench(uint32_t from)
{
    static uint8_t done;
    if (done) return;
    done = 1;

    const uint32_t N = 32u;                 /* 128 KB, ~1 s at the low end */
    uint32_t t0 = cycles(), got = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (!target_read_slot(MP3_SLOT_ID, from + i * REFILL_CHUNK,
                              TAG_OFF, REFILL_CHUNK)) break;
        got += REFILL_CHUNK;
    }
    uint32_t dt = cycles() - t0;
    io_bench_bytes = got;
    io_kbps = (dt && got)
            ? (uint16_t)(((uint64_t)got * (uint64_t)CLK_HZ)
                         / ((uint64_t)dt * 1024u))
            : 0u;
}
#endif

static void refill_pump(void);     /* defined below; the glue needs it */
static int  prefill(void);         /* likewise, for flac_restart()     */
static int  refill_one(void);      /* blocking read, for flac_pull()   */

/* ---------------------------------------------------------- FLAC glue ---
 *
 * Two adapters, and nothing else: the decoder is format logic, the firmware
 * owns the ring and the FIFO, and neither should know about the other.
 *
 * The shapes already exist in the MP3 path. flac_pull() consumes the ring the
 * way MP3Decode does; flac_emit() pushes samples the way the frame loop does,
 * including servicing input and refills from inside the full-FIFO wait --
 * which is where this CPU spends most of its time and the only reason the
 * decoder keeps up. */

static uint32_t flac_stall;        /* refills that came back empty          */

/* UI cadence, decoupled from the decode frame.
 *
 * ui_draw_dynamic() is driven once per decoded frame, which is right for MP3
 * -- 1152 samples is 26 ms, so ~38 a second. A FLAC frame is 4608 samples,
 * 104 ms, so the SAME loop refreshes the whole UI at ~9.6 fps: every meter,
 * marquee and clock at a quarter speed. That is what "sluggish" was, and it
 * is structural rather than anything to do with the meters.
 *
 * Driven on elapsed time instead, matching MP3's rate. The check runs once per
 * flac_emit call -- every 64 samples, ~1.45 ms -- so it costs one counter read
 * per call and cannot be late by more than that. */
#define FL_UI_PERIOD (CLK_HZ / 38u)
static uint32_t fl_ui_next;

static int flac_pull(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    int got = 0;
    while (got < n) {
        /* Collect any finished read and let the next one start, BEFORE asking
         * whether the ring is dry.
         *
         * Reads used to be started only inside the full-FIFO wait in
         * flac_emit, which works while there is idle time to wait in and does
         * nothing once there is not. Measured: Pink Floyd has D27 of idle and
         * shows O0, while every 24-bit file has D0 and pays O27..O39. Demand
         * against the measured 736 KB/s is 28/42/53%, against measured O of
         * 27/35/39 -- so the card was never slow. The reads were simply never
         * STARTED until the ring had already run dry, and by then the only
         * option left is a blocking one. O is a CONSEQUENCE of D reaching
         * zero, and it compounds.
         *
         * This is safe against the blocking path: target_read_start_slot()
         * already calls refill_drain(), so there is exactly one command in
         * flight ever. An earlier build blamed corruption here on a race and
         * added a drain loop of its own -- the race did not exist, the
         * corruption was the frame-header bug fixed in f0f6bac, and the loop
         * turned every failed read into a ~35-second retry chain. */
        refill_pump();

        /* UI refresh from HERE as well as from flac_emit, sharing one deadline
         * so the rate is unchanged.
         *
         * flac_emit only runs while the SECOND channel is being decoded --
         * subframe() decodes all of channel 0 first and emits nothing, which
         * is ~half of a 104 ms frame with no refresh at all, then a burst.
         * That bunching is the "tad of sluggishness" left after the rate fix:
         * the average was right, the spacing was not. flac_pull is called
         * every 512 bytes, right through both channels, so it fills the gap.
         *
         * Gated on fl_buf, which is null until flac_open has returned and the
         * block buffer is allocated -- so this never fires while metadata is
         * still being walked and the card is half-populated. */
        if (fl_buf && (int32_t)(cycles() - fl_ui_next) >= 0) {
            fl_ui_next = cycles() + FL_UI_PERIOD;
            if (!ui_dump_mode) ui_draw_dynamic();
        }

        if (ring_rd >= ring_fill) {
            /* Dry. Pump and wait, the same as a full FIFO -- the decoder
             * cannot proceed and the CPU has nothing better to do. */
            /* refill_one, not refill_pump: this is a BLOCKING read, and it
             * has to be. refill_pump only issues a request and returns, so
             * spinning on it here would depend on the main loop collecting --
             * which cannot happen, because the main loop is inside this call.
             * Metadata skipping needs real progress: the hi-res file on the
             * test card carries 59 KB of picture and 151 KB of padding before
             * its first audio frame. */
            uint32_t t0 = cycles();
            int ok = refill_one();
            fl_io_cyc += cycles() - t0;
            if (!ok || ring_rd >= ring_fill) {
                if (++flac_stall > 8u) break;       /* genuine end of file */
                continue;
            }
        }
        flac_stall = 0;
        uint32_t avail = ring_fill - ring_rd;
        uint32_t take  = ((uint32_t)(n - got) < avail) ? (uint32_t)(n - got)
                                                       : avail;
        for (uint32_t i = 0; i < take; i++) dst[got + i] = ring[ring_rd + i];
        ring_rd += take;
        got     += (int)take;
    }
    return got;
}

static uint32_t fl_cap;            /* max_blocksize, kept across reopens */

/* Rewinds a FLAC stream to its first audio frame.
 *
 * Needed because every reposition -- track start, restart, stop -- resets the
 * ring to file_pos and the decoder's own state would be left mid-stream. MP3
 * survives that by re-finding a sync word; FLAC cannot, because its metadata
 * was consumed once at open and the bit reader holds partial bytes.
 *
 * Reopening from byte 0 re-reads the metadata, which is a few hundred bytes
 * and only happens on a reposition. Simpler and surer than trying to record
 * where the audio began and restore the reader's bit position to match. */
/* ---- FLAC seek support -------------------------------------------------
 *
 * Byte-proportional seeking cannot work on FLAC, and the test files say so
 * numerically: between two seek points of the Psychedelic Furs file the rate
 * is 164 KB/s, and between the next two it is 213 KB/s. A single average
 * applied across a track that varies by 30% lands somewhere different every
 * time -- "jumps all over the place" is exactly what that produces.
 *
 * The SEEKTABLE gives sample -> byte pairs, so the fix is to INTERPOLATE
 * between the two points bracketing the target rather than extrapolate one
 * global rate across the whole file. Within a ~10 second interval the rate is
 * near enough constant for the landing to be accurate.
 *
 * Seeking straight to a seek point would be exact but useless: the points are
 * ~10 s apart on every one of these files, so a 5 s seek would frequently not
 * move at all.
 *
 * The table is left in the FILE and read into tagbuf at seek time -- 227
 * points fit a 4 KB read, and holding it in BSS would want ~1.8 KB of the
 * ~1.8 KB of link slack that remains.
 */
/* Where a run of seek presses has ASKED to be, as opposed to where playback
 * actually is.
 *
 * The transport must advance by the step on every press. Computing the next
 * target from the clock alone cannot guarantee that: if a landing falls short,
 * the next target is computed from the short position and the errors compound
 * until the seek stops moving. Holding the intent separately means a run of
 * presses advances monotonically whatever the landings do, while the clock
 * stays truthful about where the audio is. The guard band drops the intent
 * once ordinary playback has caught up, so it never lingers. */
static uint32_t fl_seek_intent;

static uint32_t fl_seek_off;      /* absolute offset of the SEEKTABLE body  */

static uint32_t fl_seek_pts;      /* 18-byte points; 0 = no table           */
/* Declared up with the other track state, not down with the FLAC seek code
 * where it used to live: the format row needs it, and that is drawn well above
 * here. A tentative definition either way -- it is the same object. */

static uint32_t fl_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Walks the metadata blocks by absolute file offset, recording the seek table
 * and where the audio actually begins. Same shape as art_find_flac_picture();
 * both read through tagbuf rather than the ring, so neither disturbs playback. */
static void flac_scan_metadata(void)
{
    fl_seek_off = fl_seek_pts = fl_first_frame = 0;

    if (!target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, 4u)) return;
    if (tagbuf[0] != 'f' || tagbuf[1] != 'L' ||
        tagbuf[2] != 'a' || tagbuf[3] != 'C') return;

    uint32_t off = 4u;
    for (uint32_t guard = 0; guard < 64u; guard++) {
        if (!target_read_slot(MP3_SLOT_ID, off, TAG_OFF, 4u)) return;
        uint32_t last = (uint32_t)(tagbuf[0] >> 7);
        uint32_t type = (uint32_t)(tagbuf[0] & 0x7Fu);
        uint32_t len  = ((uint32_t)tagbuf[1] << 16)
                      | ((uint32_t)tagbuf[2] << 8) | (uint32_t)tagbuf[3];
        uint32_t body = off + 4u;

        if (type == 3u && len >= 18u) {
            uint32_t pts = len / 18u;
            if (pts > TAG_SIZE / 18u) pts = TAG_SIZE / 18u;
            fl_seek_off = body;
            fl_seek_pts = pts;
        }
        off = body + len;
        if (last) { fl_first_frame = off; return; }
    }
}


/* ------------------------------------------------------- accurate seeking
 *
 * A byte offset interpolated from a time is a GUESS, and on material whose
 * bitrate varies it is a poor one -- measured on a file whose quiet half
 * occupies a four-hundredth of the bytes of its loud half, asking for 15s
 * landed at 20s. Two attempts were made to fix the resulting clock error by
 * correcting the clock AFTERWARDS, from the frame the decoder resynced on.
 * Both failed, the second leaving the transport unusable, and the reason is
 * structural rather than a coding slip: the next seek target is computed FROM
 * the clock, so a truthful clock plus an inaccurate landing means each press
 * moves by the step MINUS the landing error. Reproduced under tools/rv32sim.py
 * with the real decoder: presses advanced +22, +11, +5, then +0, +0, +0 --
 * stuck, exactly as reported.
 *
 * So the landing is made accurate instead. Every FLAC frame header states its
 * own position, and flac_probe_frame() reads one without decoding audio, so
 * the offset can be measured and refined until it is right. Then the clock and
 * the transport agree because there is nothing left to disagree about.
 *
 * Probing reads the card directly rather than through the ring: the ring is
 * the playback path and refilling it for a measurement that is about to be
 * thrown away would cost far more than the 512 bytes a probe needs. */
static uint32_t fl_probe_pos;

static int flac_probe_pull(void *ctx, uint8_t *dst, int n)
{
    (void)ctx;
    if (n <= 0 || fl_probe_pos >= slot_size) return 0;
    uint32_t want = (uint32_t)n;
    if (want > 512u) want = 512u;
    if (fl_probe_pos + want > slot_size) want = slot_size - fl_probe_pos;
    if (!want) return 0;
    if (!target_read_slot(MP3_SLOT_ID, fl_probe_pos, TAG_OFF, want)) return 0;
    for (uint32_t i = 0; i < want; i++) dst[i] = tagbuf[i];
    fl_probe_pos += want;
    return (int)want;
}

static uint64_t fl_sample_of(void)
{
    return fl.number_is_sample ? fl.frame_number
                               : fl.frame_number * (uint64_t)fl.max_blocksize;
}

/* Finds the byte offset whose frame starts closest to `want` samples, and
 * reports the sample position it actually found there.
 *
 * False position between a bracketing pair. Each probe replaces one end, so
 * nothing is assumed about how bytes map to time -- a badly nonlinear file
 * simply takes another step. Measured: one probe for an ordinary file, and
 * convergence on the pathological one.
 *
 * A probe reporting a position outside the bracket cannot be real. That is a
 * false sync whose arbitrary frame number happened to survive the CRC-8 --
 * there are 22 of them in one file on the test card, and trusting one is what
 * broke the previous attempt. Here it is simply discarded, and because the
 * bracket only ever narrows, a rejected probe costs an iteration rather than
 * the answer. */
static uint32_t flac_seek_locate(uint64_t want, uint64_t *landed)
{
    *landed = 0;
    if (!fl_first_frame || !fl.rate || !fl.max_blocksize) return 0;
    if (slot_size <= fl_first_frame) return 0;

    uint64_t total = fl.total_samples;
    if (!total && track_secs) total = (uint64_t)track_secs * (uint64_t)fl.rate;
    if (!total) return 0;
    if (want > total) want = total;

    uint32_t lo_b = fl_first_frame, hi_b = slot_size;
    uint64_t lo_s = 0, hi_s = total;

    /* Seed from the seek table when there is one: it brackets the target
     * exactly, which is why an ordinary file converges on the first probe. */
    if (fl_seek_pts) {
        uint32_t n = fl_seek_pts * 18u;
        if (target_read_slot(MP3_SLOT_ID, fl_seek_off, TAG_OFF, n)) {
            for (uint32_t i = 0; i < fl_seek_pts; i++) {
                const uint8_t *e = &tagbuf[i * 18u];
                if (fl_be32(e) == 0xFFFFFFFFu || fl_be32(e) != 0u) continue;
                uint64_t smp = (uint64_t)fl_be32(e + 4u);
                uint32_t byt = fl_first_frame + fl_be32(e + 12u);
                if (byt <= lo_b || byt >= hi_b) continue;
                if (smp <= want && smp >= lo_s) { lo_s = smp; lo_b = byt; }
                else if (smp > want && smp <= hi_s) { hi_s = smp; hi_b = byt; }
            }
        }
    }

    flac_read_fn  saved_read = fl.read;
    void         *saved_ctx  = fl.ctx;
    uint32_t      best_b     = lo_b;
    uint64_t      best_s     = lo_s;
    uint64_t      best_d     = (want > lo_s) ? want - lo_s : lo_s - want;
    /* Whether anything here is MEASURED. A seek table seeds a real pair, and
     * a successful probe produces one. With neither, best_s is still the
     * initial 0 and reporting it would send the clock -- and playback -- to
     * the start of the track on a file the probes could not read at all.
     * Refusing the seek leaves playback untouched, and the intent still
     * advances so a second press tries again further on. */
    int           measured  = fl_seek_pts ? 1 : 0;
    fl.read = flac_probe_pull;
    fl.ctx  = 0;
#if UI_SHOW_SEEK_DIAG
    dg_pn = dg_pfail = dg_prej = 0;
#endif

    for (uint32_t it = 0; it < 12u; it++) {
        if (hi_s <= lo_s || hi_b <= lo_b + 1u) break;
        uint32_t at = lo_b + (uint32_t)(((uint64_t)(hi_b - lo_b) *
                                         (want - lo_s)) / (hi_s - lo_s));
        if (at <= lo_b) at = lo_b + 1u;
        if (at >= hi_b) at = hi_b - 1u;

        fl_probe_pos = at;
        flac_flush_input(&fl);
#if UI_SHOW_SEEK_DIAG
        if (dg_pn < 200u) dg_pn++;
#endif
        if (flac_probe_frame(&fl) != FLAC_OK) {
#if UI_SHOW_SEEK_DIAG
            if (dg_pfail < 200u) dg_pfail++;
#endif
            hi_b = at; continue;
        }

        uint64_t got = fl_sample_of();
        if (got < lo_s || got > hi_s) {                        /* false sync */
#if UI_SHOW_SEEK_DIAG
            if (dg_prej < 200u) dg_prej++;
#endif
            hi_b = at; continue;
        }

        measured = 1;
        uint64_t d = (got > want) ? got - want : want - got;
        if (d < best_d) { best_d = d; best_b = at; best_s = got; }
        if (d <= (uint64_t)fl.max_blocksize) break;

        if (got < want) { lo_b = at; lo_s = got; }
        else            { hi_b = at; hi_s = got; }
    }

    fl.read = saved_read;
    fl.ctx  = saved_ctx;
    flac_flush_input(&fl);

    if (!measured) return 0;
    *landed = best_s;
    return best_b;
}

/* Move the stream forward without delivering the bytes -- flac_open()'s way of
 * walking past a metadata block it does not want.
 *
 * Almost always the cover art. Measured on this hardware: 259318 bytes of
 * PICTURE and 75214 of PADDING, read through the bit reader one byte at a time,
 * 628 ms of a FLAC load -- for data load_track() then reads AGAIN by itself to
 * decode the artwork. flac_open was not gathering it, only walking past it.
 *
 * Two cases. Bytes already in the ring are simply consumed, which is exactly
 * what flac_pull does with them. Beyond that the ring is dropped and file_pos
 * moves, so the next refill fetches from the new place -- the same manoeuvre a
 * seek performs, minus the decoder reset, because flac_open is between blocks
 * here and has no decoder state to lose.
 *
 * Refuses rather than guesses when the move would leave the file: returning 0
 * puts flac_open back on its read-and-discard path, which is slow but always
 * correct. */
static int flac_skip_bytes(void *ctx, uint32_t n)
{
    (void)ctx;
    if (!n) return 1;

    uint32_t avail = ring_fill - ring_rd;
    if (n <= avail) { ring_rd += n; return 1; }
    n -= avail;

    if (n > 0xFFFFFFFFu - file_pos) return 0;
    if (slot_size && file_pos + n > slot_size) return 0;

    refill_drain();                 /* no read may be in flight across this */
    ring_fill = 0; ring_rd = 0;
    file_pos += n;
    return 1;
}

static int flac_restart(void)
{
    ring_fill = 0; ring_rd = 0; file_pos = 0;
    flac_stall = 0;
    if (!prefill()) return 0;
    fl.skip = flac_skip_bytes;      /* set BEFORE open: it survives the zeroing */
    if (flac_open(&fl, flac_pull, 0, fl_buf, fl_cap) != FLAC_OK) return 0;
    pcm_rate_apply(fl.rate);
    return 1;
}

/* Stereo pairs of the current FLAC frame captured for the meters, and how
 * many. The buffer is Helix's output array, which is idle for the whole of a
 * FLAC track -- the decoder is freed at load. Reusing it costs nothing and
 * keeps the meters on one code path; a buffer of its own would not fit
 * anyway, with ~1.8 KB of link slack left.
 *
 * The first 1152 pairs of a 4608-sample frame, which is 26 ms of every 104 ms.
 * They have to be CONTIGUOUS rather than spread across the frame: the
 * oscilloscope searches for a zero crossing to trigger on and then walks
 * WAVE_COLS * WAVE_SPAN consecutive samples. */
#define FL_METER_PAIRS 1152u
static uint32_t fl_meter_n;


/* `src`, not `pcm`: the file-scope pcm[] is the meter capture buffer, and a
 * parameter of that name would shadow it. */
static void flac_emit(void *ctx, const int16_t *src, uint32_t frames)
{
    (void)ctx;
    /* Safe here: ui_draw_dynamic() performs no I/O and cannot re-enter the
     * decoder. It reads position from `frames`, which has not been advanced
     * for the frame in flight, so the clock trails by at most one frame.
     *
     * Reads the counter itself now the profiling timestamp it used to borrow
     * is gone -- one MMIO read per 64 samples. */
    uint32_t t_now = cycles();
    if ((int32_t)(t_now - fl_ui_next) >= 0) {
        fl_ui_next = t_now + FL_UI_PERIOD;
        if (!ui_dump_mode) ui_draw_dynamic();
    }

    for (uint32_t i = 0; i < frames; i++) {
        /* Fill CONTINUOUSLY and flush every FL_METER_PAIRS, rather than
         * grabbing the head of each frame and dropping the rest.
         *
         * This was the real "delayed, not realtime" fault, and it was in the
         * DATA, not the redraw. meters_feed() ran once per FLAC frame -- 9.6 a
         * second against MP3's 38 -- from the first 1152 of 4608 pairs, so the
         * peaks changed nine times a second and three quarters of the audio
         * was never looked at. Repainting faster cannot help a number that is
         * not moving.
         *
         * At 1152 pairs the flush interval is 26 ms, which is exactly MP3's
         * frame, so both formats now drive the meters at the same rate through
         * the same code. Ignoring frame boundaries keeps the interval even. */
        pcm[fl_meter_n * 2u]      = src[i * 2];
        pcm[fl_meter_n * 2u + 1u] = src[i * 2 + 1];
        if (++fl_meter_n == FL_METER_PAIRS) {
            meters_feed(pcm, (int)(FL_METER_PAIRS * 2u), 1);
            fl_meter_n = 0;
        }
        int32_t l = src[i * 2], r = src[i * 2 + 1];
        /* Volume, which this path did not apply at all -- the d-pad moved the
         * number on screen while FLAC played at full scale, and volume 0 was
         * not silent. The MP3 loop has had this the whole time; adding FLAC
         * added a second push path and only one of them was volume-aware.
         * Same order as MP3: volume first, then the fade, so a fade-in at low
         * volume stays at low volume. Capped at unity, so it only ever
         * attenuates and cannot overflow. */
        if (vol_gain != 256) {
            l = (l * vol_gain) >> 8;
            r = (r * vol_gain) >> 8;
        }
        if (fade_left) {
            int32_t g = (int32_t)((FADE_SAMPLES - fade_left) >> 3);
            l = (l * g) >> 8;
            r = (r * g) >> 8;
            fade_left--;
        }
        if (PCM_FULL(REG(R_PCM_ST))) {
            uint32_t t0 = cycles();
            do {
                poll_input();
                refill_pump();
            } while (PCM_FULL(REG(R_PCM_ST)));
            fl_idle_cyc += cycles() - t0;
        }
        REG(R_AUDIO) = ((uint32_t)(uint16_t)(int16_t)r << 16)
                     | (uint32_t)(uint16_t)(int16_t)l;
    }
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
/* Step the incremental search, and pick up what becomes knowable when it
 * finishes. Returns 1 on the step that completed it.
 *
 * The bitrate is derived from the size, so it arrives at the same instant --
 * and that is the FLAC format row, which stayed blank because track_kbps is
 * computed once at load and slot_size is no longer known by then. Anything
 * else that waits on the size belongs here too rather than in another copy of
 * this. */
static int size_probe_pump(void)
{
    if (!szp_phase || szp_phase >= 4u) return 0;
    if (!size_probe_step()) return 0;

    if (track_fmt == FMT_FLAC && track_secs && slot_size > fl_first_frame) {
        uint64_t bits = (uint64_t)(slot_size - fl_first_frame) * 8u;
        track_kbps = (uint32_t)(bits / (uint64_t)track_secs / 1000u);
    }
    return 1;
}

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

    /* Playing PAST the measured end proves the measurement was short, which
     * is exactly what an early probe on a 0192-opened file produces. Measure
     * again rather than carrying a number the file has already disproved --
     * the seek bracket, the progress bar and the bitrate all read it. */
    if (szp_phase == 4u && slot_size && file_pos > slot_size && !eof_hit)
        size_probe_arm();

    /* Ring first, always -- audio starvation beats a late tag update. */
    if (ring_fill - ring_rd >= RING_SIZE / 2u) {
        /* The ring is at least half ahead, so this pass has an I/O slot going
         * spare: spend it measuring the file, one 512-byte probe at a time.
         * This is the "one read per pass while the buffer is full" the size
         * probe was always meant to use -- it stalls nothing, and it is why
         * load_track() no longer blocks for 480 ms. */
        size_probe_pump();
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

    /* FLAC or MP3, decided by the file's first four bytes rather than its
     * extension -- a mislabelled file should play, and a .flac that is really
     * an MP3 should not fail mysteriously.
     *
     * Detected here because everything below is ID3-shaped: tag length, the
     * settle-compare on the tag bytes, audio_start. None of it applies to a
     * FLAC, whose metadata the decoder consumes itself. */
    if (ring[0] == 'f' && ring[1] == 'L' && ring[2] == 'a' && ring[3] == 'C') {
        track_fmt   = FMT_FLAC;
        audio_start = 0;
        ring_rd     = 0;
        /* The head read above put bytes [0, REFILL_CHUNK) in the ring, so the
         * next refill must continue from there. Without this file_pos keeps
         * whatever the previous track left and every refill reads the wrong
         * part of the file. */
        file_pos    = REFILL_CHUNK;
        track_title[0] = 0; track_artist[0] = 0;
        track_album[0] = 0; track_year[0]   = 0; track_trk[0] = 0;
        title_status   = ID3_NO_TAG;      /* filename fallback shows the name */
        cur_file_id    = slot_file_id();
        return 1;
    }
    track_fmt = FMT_MP3;

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
    /* Release the FLAC buffer FIRST. This runs before the format is known --
     * detection needs the file's head, which read_track_head() has not fetched
     * yet -- so Helix is rebuilt on every load and handed back below if the
     * track turns out to be FLAC.
     *
     * Order is load-bearing. Rebuilding Helix while an 18 KB FLAC buffer was
     * still live asked the arena for 42 KB of 24, MP3InitDecoder() returned 0,
     * and EVERY load failed from the first FLAC attempt onwards -- including
     * MP3s, which is how it presented. */
    if (fl_buf) { free(fl_buf); fl_buf = 0; }

    /* Helix carries bit-reservoir state internally, so a fresh instance is
     * needed rather than just resetting our own bookkeeping. */
    if (dec) MP3FreeDecoder(dec);
    dec = MP3InitDecoder();
    if (!dec) { REG(R_STAT2) = 0xF0000000u; return 0; }

    /* Silence the old track's tail before anything else touches the ring. */
    pcm_flush();

    frames = 0; errs = 0; rate_set = 0; min_level = 0xFFFFFFFFu;
    /* A NEW track starts at 0:00, and this is the one place that knows one
     * started. ui_draw_chrome used to do it, which caught every repaint too. */
    ui_sec = 0; ui_sec_acc = 0; ui_last_frames = 0xFFFFFFFFu;
    track_kbps = 0; track_hz = 0; samp_per_frame = 1152u;
    bytes_per_sec = 16000u;
    paused = 0;
    /* Arm the free-running-counter deadlines from NOW.
     *
     * Both are compared as `(int32_t)(cycles() - deadline) >= 0`, which is the
     * right way to handle a 32-bit counter that wraps every 71.6 s -- but only
     * once the deadline holds a real timestamp. Left at 0, the comparison
     * reduces to the sign of cycles() itself, so a track loaded while the
     * counter sits in its upper half reads NEGATIVE and the timer does not
     * fire until the counter wraps: up to 35.8 seconds, ~18 on average.
     *
     * That is not theoretical. It was reported as the FLAC meters flowing
     * slowly and out of time for about 17 seconds and then snapping into
     * place -- the wrap. With fl_ui_next dead, ui_draw_dynamic() ran only once
     * per FLAC frame, 9.6 Hz, scrolling the bars at 4.8. */
    fl_ui_next = cycles();
    tk_poll_at = cycles() + CLK_HZ * 2u;
    /* pl_poll_at had the same fault, and it matters more than the meters: it
     * is only ever assigned INSIDE the `if` that tests it, so from boot it sat
     * at 0 and the playlist identity poll -- the recovery for a pick the core
     * misses -- was dead for up to 35.8 seconds. That is exactly the window in
     * which someone is choosing playlists. */
    pl_poll_at = cycles() + CLK_HZ * 3u;
    /* `stopped` is sticky and was cleared in exactly ONE place -- the A handler,
     * on un-pause. A new track starts PLAYING, so leaving it set meant the
     * first A press paused while the transport still read STOPPED, and it took
     * a second play/pause to clear. Clearing it here is what `paused = 0`
     * already means: this track is running, not parked at 0:00. */
    stopped = 0;
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
    seek_size_tried  = 0;

    uint32_t t0 = cycles(), tphase = t0;
    if (!read_track_head()) { REG(R_STAT2) = 0xE0000000u; return 0; }
    ld_head = LD_MS(cycles() - tphase); tphase = cycles();

    if (track_fmt == FMT_FLAC) {
        /* Hand the arena to FLAC. Helix has to go first -- the two decoders
         * swap the same space, and Helix's 23824 leaves no room beside a
         * blocksize buffer. */
        if (dec) { MP3FreeDecoder(dec); dec = 0; }
        fl_buf = 0;

        /* Open with a provisional cap so STREAMINFO can be read; the real
         * buffer is sized from max_blocksize once it is known. */
        static int32_t probe_cap;
        probe_cap = (int32_t)(ARENA_LIMIT / sizeof(int32_t));

        /* Tags come out of the Vorbis comment block during the metadata walk,
         * straight into the same fields the ID3 path fills -- so the card,
         * the marquees and the idle screen need to know nothing about format.
         * Set BEFORE flac_open, which is where the walk happens. */
        fl.tag_title  = track_title;
        fl.tag_artist = track_artist;
        fl.tag_album  = track_album;
        fl.tag_year   = track_year;
        fl.tag_trk    = track_trk;
        fl.tag_cap    = sizeof(track_title);

        fl.skip = flac_skip_bytes;  /* set BEFORE open: it survives the zeroing */
        flac_err fe = flac_open(&fl, flac_pull, 0, 0, (uint32_t)probe_cap);
        if (fe == FLAC_ERR_UNSUPPORTED) {
            /* Mirrors the order of the checks inside flac_open, so the reason
             * reported is the one that actually fired. */
            if (fl.channels < 1u || fl.channels > 2u) {
                fl_reject_kind = FLR_CHANS; fl_reject_val = fl.channels;
            } else if (fl.bps != 8u && fl.bps != 16u &&
                       fl.bps != 20u && fl.bps != 24u) {
                fl_reject_kind = FLR_DEPTH; fl_reject_val = fl.bps;
            } else {
                fl_reject_kind = FLR_BLOCK; fl_reject_val = fl.max_blocksize;
            }
            REG(R_STAT2) = 0xC4000000u | (uint32_t)fl_reject_kind;
            dec = MP3InitDecoder();
            track_fmt = FMT_MP3;
            rate_unsupported = 1u;
            return 0;
        }
        if (fe != FLAC_OK) {
            /* Hand the arena back to Helix rather than leaving the core with
             * no decoder -- otherwise one bad file breaks every load after
             * it, which is exactly what happened. */
            REG(R_STAT2) = 0xC0000000u | (uint32_t)fe;
            dec = MP3InitDecoder();
            track_fmt = FMT_MP3;
            return 0;
        }
        /* Refuse hi-res BEFORE committing the arena to it. Measured cutoff --
         * see ui_rate_unsupported(). Handing the arena back to Helix on the
         * way out matters: leaving the core with no decoder is what once made
         * a single bad file break every load after it. */
        if (fl.rate > FLAC_MAX_RATE) {
            REG(R_STAT2) = 0xC3000000u | fl.rate;
            fl_reject_kind = FLR_RATE;
            fl_reject_val  = fl.rate;
            dec = MP3InitDecoder();
            track_fmt = FMT_MP3;
            rate_unsupported = 1u;
            return 0;
        }
        fl_buf = (int32_t *)malloc((size_t)fl.max_blocksize * sizeof(int32_t));
        if (!fl_buf) { REG(R_STAT2) = 0xC1000000u; return 0; }
        fl.ch0     = fl_buf;
        fl.ch0_cap = fl.max_blocksize;
        fl_cap     = fl.max_blocksize;

        track_hz       = fl.rate;
        samprate       = fl.rate;
        track_kbps     = 0;      /* computed after the size probe, below */
        /* The format row's third field. On an MP3 it names the encoder; for a
         * lossless file the bit depth is the equivalent fact, and it is the
         * one thing about a FLAC that the rate does not already say. */
        {
            char *q = track_encoder;
            *q++ = 'F'; *q++ = 'L'; *q++ = 'A'; *q++ = 'C'; *q++ = ' ';
            q = ui_dec(q, fl.bps);
            *q++ = '-'; *q++ = 'b'; *q++ = 'i'; *q++ = 't';
            *q = 0;
        }
        /* Seek distance falls back to bytes_per_sec until the size probe lands
         * slot_size, and the 16000 default would make every FLAC seek about
         * six times too short. Estimate from the uncompressed rate instead:
         * FLAC lands around 60-77% of PCM on the test files (16/44.1 measured
         * 105 KB/s against 176 uncompressed, 24/44.1 204 against 265), so 70%
         * is within ~15% either way -- and it stops mattering entirely the
         * moment slot_size is known, when both helpers switch to the exact
         * size/duration figure. */
        bytes_per_sec  = ((uint32_t)fl.rate * (uint32_t)fl.channels
                          * (uint32_t)fl.bps / 8u) * 7u / 10u;
        samp_per_frame = fl.max_blocksize;
        track_secs     = fl.rate ? (uint32_t)(fl.total_samples / fl.rate) : 0;
        rate_set       = 1;
        pcm_rate_apply(fl.rate);
        flac_stall     = 0;
        flac_scan_metadata();      /* seek table + where audio starts */
        fl_rate_hz     = fl.rate;
    }
#if IO_BENCH
    /* Here specifically: the FIFO is flushed and the gap is already silent,
     * so a one-off burst costs gap length rather than audio. */
    io_bench(audio_start);
    tphase = cycles();                      /* keep ld_size honest */
#endif
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
        /* ARMED, not run. It used to block here, deliberately, on the argument
         * that a load is silent anyway so the reads cost gap length rather
         * than audio. True, but it cost 480 ms of every FLAC load, and worse,
         * it produced the WRONG answer for a file opened by name -- the slot
         * has only just been opened and a random read far into it still fails,
         * so a 30 MB track measured 5 MB and seeking could not work at all.
         * Running it a second later, spread across the main loop, is both
         * faster to load and correct. */
        size_probe_arm();
    /* FLAC's bitrate is only knowable once the file SIZE is -- the stream
     * carries a duration but never a rate, and it is variable anyway, so this
     * is the average over the whole file. It has to be here rather than in the
     * format branch above, which runs before the probe. */
    if (track_fmt == FMT_FLAC && track_secs && slot_size > fl_first_frame) {
        uint64_t bits = (uint64_t)(slot_size - fl_first_frame) * 8u;
        track_kbps = (uint32_t)(bits / (uint64_t)track_secs / 1000u);
    }
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
        /* Same file. Free, and the common case on a restart. */
        has_art = art_have;
    } else {
        /* Different file, but very often the same PICTURE: every track of an
         * album embeds one cover. Measured on this hardware, that decode is
         * 2801 ms of a 3731 ms load, so asking first is worth a few reads.
         *
         * Must happen BEFORE ui_art_mount(), which fills the stash with the
         * panel colour -- checking afterwards would compare against an image
         * it had already destroyed. */
        uint32_t sig = art_sig_of(audio_start);
        art_bad = 0;
        if (art_have && sig && sig == art_sig) {
            has_art = 1;                 /* the stash already holds this cover */
        } else if (sig && sig == art_bad_sig) {
            /* Known bad. The frame and its reason are still drawn -- that is a
             * standing fact about the file, not an event -- but nothing is
             * decoded again and nothing is announced again. Repeating either
             * on all thirteen tracks of an album helps nobody. */
            ui_art_mount();
            ui_art_reason(art_bad_code == PJPG_UNSUPPORTED_MODE);
            ui_art_round();
            has_art = 0;
            art_bad = 1;
        } else {
            ui_art_mount();
            has_art = art_decode(audio_start);
            if (!has_art) {
                if (art_fail_code) {     /* present, and unreadable */
                    art_bad      = 1;
                    art_bad_code = art_fail_code;
                    art_bad_sig  = sig;
                    ui_art_reason(art_bad_code == PJPG_UNSUPPORTED_MODE);
                } else {
                    ui_art_placeholder();   /* simply no artwork */
                }
            }
            ui_art_round();
            art_sig = has_art ? sig : 0u;

            /* A cover that IS there and cannot be read is worth a word. An
             * empty frame with no reason for it reads as the core being
             * broken, and the reason was already in hand -- picojpeg says
             * exactly why and it was being thrown away.
             *
             * Only when a picture was actually found: art_fail_code stays 0
             * when a file simply has no artwork, which is not a fault and
             * needs no announcement. */
            if (art_bad)
                ui_toast_msg(art_bad_code == PJPG_UNSUPPORTED_MODE
                             ? "COVER: PROGRESSIVE JPEG"
                             : "COVER: CANNOT BE READ");
        }
        art_file_id = cur_file_id;
    }
    art_ready = 1;
    ld_art = LD_MS(cycles() - tphase); tphase = cycles();

    /* Panel state follows the TRACK, not the session. art_x is set directly
     * rather than animated -- a track change should not look like a slide. */
    art_have  = (uint8_t)has_art;
    art_shown = (uint32_t)(ART_PANEL_WANTED && art_pref);
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

#if UI_SHOW_LOAD_TIMES
    /* The load-phase breakdown, as a toast, after every load.
     *
     * ROADMAP has had "track changes take too long" open since 2026-08-12 with
     * TWO guesses in this codebase pointing at different culprits: that entry
     * blames the artwork, the comment on the size probe blames the probe. The
     * timers have existed the whole time and settle it in one session.
     *
     * A toast rather than a key binding: the numbers are wanted for the load
     * that just happened, and needing to press something to see them means
     * pressing it during the gap you are trying to measure. Nothing to
     * remember, nothing to hold.
     *
     * H head read, S size probe, A art decode, P prefill, T total, all in ms.
     * OFF for any build a user sees. */
    {
        /* P is gone: measured at 6..26 ms against a 2200..3700 ms total, so it
         * only ever cost this row the width that clipped T -- "T3731" showed
         * as "T373", a total smaller than one of its own parts. */
        char b[40], *q = b;
        const char *lbl = "HSAT";
        const uint16_t v[4] = { ld_head, ld_size, ld_art, ld_total };
        for (uint32_t k = 0; k < 4u; k++) {
            *q++ = lbl[k];
            q = ui_dec(q, v[k]);
            if (k < 3u) *q++ = ' ';
        }
        *q = 0;
        ui_toast_set(b, 0xFFFFFFFFu, 0);
    }
#endif
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
                /* No toast here. The boot row went up when the pick was
                 * detected and holds by itself until ui_boot_cancel(); a toast
                 * refreshed alongside it put the SAME words on a second line,
                 * which is the duplicate that kept being reported. */
                uint32_t id = slot_file_id();
                if (id != 0u && id != stale_ref_file_id) ready = 1;
            }

            if (ready || (int32_t)(cycles() - reload_at) >= 0) {
                reload_armed = 0;
                uint32_t tk_ge = ready ? 1u : 2u;
                uint32_t tk_was = tk_prev_name;

                /* The indicator went up when the pick was detected, not here.
                 * was_idle only decides what to restore if the open fails. */
                int was_idle = idle;
                if (!was_idle)
                    ui_boot_note(resume_seek_req ? "RESUMING TRACK"
                                                 : "LOADING TRACK");
                int opened   = load_track();
                {   /* FNV over the filename APF reports for the slot. */
                    uint32_t h = 2166136261u;
                    for (uint32_t i = 0; i < sizeof(track_file)
                                      && track_file[i]; i++) {
                        h ^= (uint32_t)(uint8_t)track_file[i];
                        h *= 16777619u;
                    }
                    tk_prev_name = h;
                    tk_hist[0] = tk_hist[1];
                    tk_hist[1] = tk_hist[2];
                    tk_hist[2] = (uint16_t)((tk_ge << 8)
                               | ((opened ? 1u : 0u) << 4)
                               |  ((h != tk_was) ? 1u : 0u));
                }
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
                } else if (rate_unsupported) {
                    /* A file we CAN read and cannot play fast enough. Says so,
                     * rather than claiming the file could not be read. */
                    rate_unsupported = 0;
                    pl_sw_tk++;
                    if (!idle) ui_rate_unsupported();
                } else { pl_sw_tk++; if (!idle) ui_load_failed(); }
                continue;
            }
        }

#if DEBUG_DIAG
        /* Select+B: freeze on the raw 0190 struct. Press again to resume;
         * decoding continues throughout, only drawing is suspended. */
        if (dt_dump_req) {
            dt_dump_req = 0;
            ui_dump_mode ^= 1u;
            if (ui_dump_mode) dt_dump_boot();
            else              ui_draw_chrome();
            continue;
        }
#endif

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
        /* Menu just closed. Ask slot 3 what it holds; if it is not what we
         * loaded, the notification for it never arrived, so raise the same
         * request it would have. Costs one 0190 at a moment when playback is
         * already interrupted -- a metadata query, not a slot READ, so it does
         * not drag the MP3 slot's fragment cache down with it the way a
         * periodic poll of slot 3 would. */
        /* PERIODIC identity check on slot 3, depending on NOTHING.
         *
         * The notification can be dropped and the menu-close edge can be
         * missed. This asks, every few seconds, whether the slot still holds
         * what we loaded -- so a pick lost anywhere upstream heals itself
         * within one interval instead of waiting for the user to retry.
         *
         * A 0190 getfile, not a slot READ. That is what makes it affordable:
         * the warning against polling slot 3 is about reads walking the
         * cluster chain, and a metadata query does not. If it costs anything
         * it will be audible as a tic every three seconds -- about as
         * diagnosable as a symptom gets -- and one constant backs it out.
         *
         * Held off while anything is mid-flight so it cannot race a load. */
        if (!idle && pl_count && !pl_check_req
            && !pl_reload_pending && !pl_reload_armed
            && !reload_pending    && !reload_armed
            && !rd_pending
            && (int32_t)(cycles() - pl_poll_at) >= 0) {
            pl_poll_at   = cycles() + CLK_HZ * 3u;
            pl_check_req = 1u;              /* same comparison path as below */
        }

        /* The same backstop for the TRACK slot. Staggered 2s against the
         * playlist's 3s so the two queries rarely land together, and held off
         * while anything is mid-flight so it cannot race a load. */
        if (!idle && cur_file_id && !pl_check_req
            && !pl_reload_pending && !pl_reload_armed
            && !reload_pending    && !reload_armed
            && !rd_pending
            && (int32_t)(cycles() - tk_poll_at) >= 0) {
            tk_poll_at = cycles() + CLK_HZ * 2u;
            if (slot_changed()) reload_pending = 1u;
        }

        if (pl_check_req) {
            pl_check_req = 0;
            pl_name_read();
            int differs = 0;
            for (uint32_t i = 0; i < sizeof(pl_cur_name); i++) {
                if (pl_name_raw[i] != pl_cur_name[i]) { differs = 1; break; }
                if (!pl_cur_name[i]) break;
            }
            /* Only when the slot names something. An empty answer means APF
             * would not say, which is not evidence of a change. */
            if (differs && pl_name_raw[0]) {
                /* pl_fb_at is deliberately NOT set here. It opens a window in
                 * which the notification handler discards arrivals as
                 * duplicates -- and setting it alongside the request meant the
                 * handler discarded THIS request, one iteration later, every
                 * single time. The fallback has never completed a load. It is
                 * set when the load finishes instead, which is the only point
                 * a later 008A is genuinely a duplicate. */
                pl_reload_pending = 1u;
                /* 0190 has ALREADY proved the slot switched, so the gate has
                 * nothing left to wait for -- without this it would sit out
                 * its full five seconds and expire. */
                pl_skip_gate = 1u;
            }
            continue;
        }

        if (pl_reload_pending) {
            pl_reload_pending = 0;
            REG(R_RELOAD) = RL_PL_RELOAD;            /* ack just this bit */
            /* A notification that lands just AFTER the menu-close fallback
             * already loaded this pick is the same event twice. Without this
             * it re-arms, finds the name unchanged, sits out the full five
             * seconds and then reloads the list it is already playing --
             * turning a dropped 008A into a worse fault than the one being
             * worked around. Three seconds only, so a deliberate re-pick of
             * the same list still reloads it. */
            if (pl_fb_at && (uint32_t)(cycles() - pl_fb_at) < CLK_HZ * 3u)
                continue;
            for (uint32_t i = 0; i < sizeof(pl_leaving); i++)
                pl_leaving[i] = pl_name_raw[i];
            pl_reload_armed = 1u;
            pl_retry        = 1u;        /* one automatic second attempt */
            pl_probe_at     = cycles();
            pl_reload_at    = cycles() + CLK_HZ * 5u;
            /* Cut the outgoing track NOW rather than at the moment the switch
             * lands -- see PAUSE_LOAD. */
            pcm_flush();
            refill_drain();
            ring_fill = 0; ring_rd = 0;
            paused |= PAUSE_LOAD;
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
                /* No toast: the boot row is already showing this. */

                int changed = 0;
                for (uint32_t i = 0; i < sizeof(pl_leaving); i++) {
                    if (pl_name_raw[i] != pl_leaving[i]) { changed = 1; break; }
                    if (!pl_leaving[i]) break;
                }

                if (changed || expired || pl_skip_gate) {
                    /* The fallback set pl_leaving from a name 0190 had
                     * ALREADY refreshed, so the "did it really change" retry
                     * below would compare the new name against itself, call
                     * it unchanged and load a second time. It has nothing to
                     * check here -- 0190 is the evidence. */
                    uint8_t from_fallback = pl_skip_gate;
                    if (pl_skip_gate) { pl_skip_gate = 0; pl_retry = 0; }
                    pl_reload_armed = 0;
                    pl_load_n++;
                    pl_sw_ge = changed ? 1u : 2u;
                    pl_sw_rt = 0u;
                    ui_boot_note("LOADING PLAYLIST");
                    /* Same cut as a skip: pl_load() blocks on slot-3 reads for
                     * longer than the FIFO holds, and picking a playlist means
                     * leaving the current track anyway. */
                    pcm_flush();
                    refill_drain();
                    ring_fill = 0; ring_rd = 0;
                    uint32_t sig_was = pl_sig;
                    pl_load();

                    /* Did the switch actually happen?
                     *
                     * If the slot still reports the name we were leaving, APF
                     * had not swapped it when the gate fired -- the probe saw
                     * a stale 0190, or the five second cap expired first --
                     * and we have just reloaded the OLD list. That is the
                     * intermittent "pick it twice" fault, and it is DETECTABLE
                     * here even though the race causing it is not reliably
                     * reproducible.
                     *
                     * So make the second attempt ourselves. Once only: picking
                     * the SAME playlist again is a legitimate case where the
                     * name does not change, and that must cost one wasted
                     * retry rather than a loop. */
                    if (pl_retry) {
                        int same = 1;
                        for (uint32_t i = 0; i < sizeof(pl_leaving); i++) {
                            if (pl_name_raw[i] != pl_leaving[i]) { same = 0; break; }
                            if (!pl_leaving[i]) break;
                        }

                        /* The name check alone was NOT enough, and the report
                         * that it still happens is what shows why.
                         *
                         * There are two stale things here, not one. 0190 can
                         * still name the old file -- that is `same`, and it is
                         * caught. But 0190 can ALSO report the new name while
                         * the reads keep coming out of APF's fragment cache,
                         * so the bytes are the old list under the new name.
                         * The name check passes, no retry fires, and the old
                         * playlist plays: exactly the surviving symptom.
                         *
                         * The text answers it directly. Two different lists
                         * hashing the same means they hold identical bytes, in
                         * which case reloading costs one wasted attempt and
                         * changes nothing the user hears. */
                        int stale = (!same && pl_sig && pl_sig == sig_was);

                        if (same || stale) {
                            pl_sw_rt        = same ? 1u : 2u;
                            pl_retry        = 0;
                            /* 0190 already names the right file, so there is
                             * nothing to reopen -- only APF's cached fragments
                             * for slot 3, which still describe the old one.
                             * Touching a different slot is the documented (and
                             * here already proven) way to drop them. Waiting
                             * would not: a cache has no timeout.
                             *
                             * Safe at this point specifically: the stream is
                             * already flushed for the reload, so the re-walk
                             * this costs lands in silence rather than starving
                             * a running decode. */
                            if (stale) target_flush_slot_cache();
                            pl_reload_armed = 1u;
                            /* A stale read has already changed name, so its
                             * probe fires at once -- this delay IS the settle.
                             * The `same` case is still waiting on the name, so
                             * it keeps the fast poll. */
                            pl_probe_at     = cycles() +
                                              (stale ? CLK_HZ / 4u : CLK_HZ / 10u);
                            pl_reload_at    = cycles() + CLK_HZ * 5u;
                            continue;        /* indicator stays up across it */
                        }
                    }
                    pl_retry = 0;

                    /* Release the row WITHOUT wiping: it is the transport row
                     * in the player, and ui_mode_dirty below repaints it. */
                    ui_boot_cancel();
                    ui_mode_dirty = 1;
                    ui_last_pause = 0xFFFFFFFFu;
                    /* Released here as well as by load_track(), for the paths
                     * that never reach one -- an empty or unreadable list, or
                     * a pick that lost the race to something else. */
                    paused &= (uint32_t)~PAUSE_LOAD;
                    pl_report();
                    /* Only take playback if nothing else is claiming it. A
                     * Load MP3 pick can bring a playlist notification with it,
                     * and starting track 1 then discards the chosen file. */
                    pl_sw_ct = pl_count;
                    pl_sw_fl = (uint8_t)((reload_pending ? 1u : 0u)
                                       | (reload_armed  ? 2u : 0u));
                    pl_sw_pp = (pl_count && !reload_pending && !reload_armed)
                             ? 1u : 0u;
                    pl_sw_hist[0] = pl_sw_hist[1];
                    pl_sw_hist[1] = pl_sw_hist[2];
                    pl_sw_hist[2] = (uint16_t)(((uint32_t)pl_sw_ge << 12)
                                             | ((uint32_t)pl_sw_rt << 8)
                                             | ((uint32_t)pl_sw_pp << 4)
                                             |  (uint32_t)pl_sw_fl);
                    /* NOW the dedupe window opens: a notification arriving
                     * after this really is the same pick reported late. */
                    if (from_fallback) pl_fb_at = cycles();
                    if (pl_sw_pp) pl_play_at(0);
                    ui_mode_dirty = 1;
                    continue;
                }
            }
            /* Still waiting for APF to switch the slot. Nothing is playing
             * while we do -- see PAUSE_LOAD. */
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

        /* Drive the size search from HERE as well, one step a pass.
         *
         * It was driven only from refill_pump()'s "ring at least half full"
         * branch, which on a dense FLAC comes round about once a second -- so
         * a sixteen-step search took sixteen seconds, and often never finished
         * at all. Two things had already broken on that: resume gave up
         * waiting for slot_size, and the FLAC format row stayed blank because
         * track_kbps is derived from it. Both were treated as their own bugs;
         * they were one.
         *
         * A step is a single 512-byte read and there are about sixteen of
         * them, so from here the whole thing is over in a fraction of a
         * second. Nothing new is asked of the card -- the same reads, sooner.
         * The gate inside still holds it off until 256 KB has been read, which
         * is what stops it measuring a file the slot has not settled into. */
        if (!reload_armed && !reload_pending) size_probe_pump();


        if (art_toggle) {
            art_toggle = 0;
            art_pref  = (uint8_t)!art_pref;
            art_shown = (uint32_t)(art_pref && ART_PANEL_WANTED);
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
            /* Get the size the way it used to be got: BLOCKING, once.
             *
             * Stepping the incremental probe from here was tried and still
             * came back Y9. Rather than keep guessing at why -- three attempts
             * and three hardware trips -- this restores exactly what worked
             * before the probe was moved out of load_track(), scoped to the
             * one case that needs it. resume is opt-in, happens once per boot,
             * and the ~480 ms lands while the track is already playing from
             * the start, which is the thing being corrected anyway.
             *
             * seek_size_tried is shared with the seek backstop deliberately:
             * both mean "the blocking probe has already been spent on this
             * track", and neither wants it twice. */
            if (!slot_size && !seek_size_tried) {
                seek_size_tried = 1u;
                slot_size = probe_file_size();
            }

            /* Kept as well: on a file where the blocking probe cannot answer,
             * the incremental one still might.
             *
             * This is what Y9 was. The seek cannot run without slot_size, and
             * since the probe moved out of load_track() it only steps from
             * refill_pump()'s "ring at least half full" branch -- which on a
             * dense FLAC comes round about once a second. Twenty steps then
             * take twenty seconds, the resume gives up at twelve, and the
             * track plays from the start with a perfectly good point saved.
             *
             * One step per pass of the main loop instead, and only while a
             * resume is actually waiting: the whole search finishes in
             * milliseconds. Each step is a single 512-byte read, the same one
             * the idle path issues, so nothing new is being asked of the card
             * -- only sooner. */
            if (!slot_size && szp_phase && szp_phase < 4u) size_probe_step();

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
            if (track_fmt == FMT_FLAC) {
                /* Reopen: the decoder cannot resume from a rewound ring. */
                if (!flac_restart()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
                frames = 0; min_level = 0xFFFFFFFFu;
                ui_sec = 0; ui_sec_acc = 0;
                ui_last_sec = 0xFFFFFFFFu; ui_prog_sec = 0xFFFFFFFFu;
                ui_last_pause = 0xFFFFFFFFu;
                ui_draw_dynamic();
                continue;
            }
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
            /* A track opened from a PLAYLIST arrives with no size: pl_arm_load()
             * zeroes slot_size and sets force_size_probe, because the file was
             * opened by name rather than mounted as a sized slot. Playback
             * never notices -- it only reads forward, and refill_pump()
             * records the real end when a read finally comes back short.
             * Seeking notices immediately: it needs to know where the end IS
             * before it can aim at a point inside the file.
             *
             * This is why the same FLAC seeks correctly after Load MP3 and not
             * after being played from a .m3u -- reported against Crash Test
             * Dummies, and true of any file whose size was never mounted.
             * flac_seek_locate() refuses outright on an unknown size, and
             * ui_byte_rate() below has the same dependency, so the MP3 path
             * was degraded by it too.
             *
             * Re-measured, not merely filled in when absent. An earlier
             * version only ran when slot_size was ZERO and never fired,
             * because the size was not missing -- it was WRONG. Measured on
             * hardware, one 30 MB FLAC: 5307 KB via a playlist against 30856
             * KB via Load MP3, and 172 kbps displayed instead of 1063.
             *
             * load_track() probes at load, when a file opened by name with
             * 0192 has only just been opened and a random read far into it
             * still fails. That truncated answer then stands for the whole
             * track. It poisons more than seeking -- the bitrate readout and
             * the end-of-track check read the same number -- but seeking is
             * where it shows, because the bracket then claims the whole
             * duration fits in a fifth of the file and every target lands
             * far short.
             *
             * Taking the LARGER is safe in the one direction that matters: an
             * early probe can only stop short of the true end, never run past
             * it, since it stops where a read first fails.
             *
             * ONCE per track. The probe is ~20 blocking reads at 480 ms, and
             * repeating it per press would replace a seek that does not move
             * with one that stalls first. */
            if (!seek_size_tried) {
                seek_size_tried = 1u;
                /* Finish the incremental search rather than starting a second
                 * one: same reads, none of them repeated. It has usually
                 * completed during playback long before a seek, in which case
                 * this costs nothing at all -- which is the point, because
                 * paying ~480 ms on the first press of every track was the
                 * "not as smooth as MP3" complaint. */
                uint32_t guard = 0;
                while (szp_phase && szp_phase < 4u && ++guard < 64u)
                    size_probe_step();

                uint32_t z = slot_size ? slot_size : probe_file_size();
                if (z > slot_size) {
                    slot_size = z;
                    /* The bitrate was derived from the old figure and is on
                     * screen right now -- 172 kbps for a FLAC that is really
                     * 1063. Recompute it here rather than leaving the header
                     * disagreeing with the file until the next load. */
                    if (track_fmt == FMT_FLAC && track_secs &&
                        slot_size > fl_first_frame) {
                        uint64_t bits = (uint64_t)(slot_size - fl_first_frame) * 8u;
                        track_kbps = (uint32_t)(bits / (uint64_t)track_secs / 1000u);

                    }
                }
            }
#if UI_SHOW_SEEK_DIAG
            dg_size  = slot_size;
            dg_dur   = track_secs;
            dg_first = fl_first_frame;
            dg_pts   = fl_seek_pts;
            dg_len   = (fl.rate && fl.total_samples)
                     ? (uint32_t)(fl.total_samples / (uint64_t)fl.rate) : 0u;
            dg_intent = fl_seek_intent;
#endif

            uint32_t step = ui_byte_rate() * (seek_secs ? seek_secs : 5u);
            uint32_t want;

            /* FLAC with a seek table works in TIME, not bytes: the target
             * second is exact, and flac_seek_locate() MEASURES the offset
             * rather than interpolating one: it probes frame headers until the
             * byte it returns really is the second asked for. The byte path
             * below stays for MP3, where it has a great deal of history in
             * it. */
            if (track_fmt == FMT_FLAC && fl_first_frame && fl.rate) {
                uint32_t secs = seek_secs ? seek_secs : 5u;
                uint32_t tgt;
                /* Base a press on the last thing asked for while a run is in
                 * progress, otherwise on where playback actually is. Without
                 * this a short landing is inherited by the next press and the
                 * transport can wedge -- measured under tools/rv32sim.py as
                 * +22, +11, +5, +0, +0, +0. */
                uint32_t base = ui_sec;
                if (fl_seek_intent > ui_sec && fl_seek_intent - ui_sec < 30u)
                    base = fl_seek_intent;
                if (seek_req == 1u) {
                    tgt = base + secs;
                    /* Leave three seconds so the end is audible and the track
                     * finishes normally, the same rule the byte path uses. */
                    uint32_t last = (track_secs > 3u) ? track_secs - 3u : 0u;
                    if (tgt > last) tgt = last;
                } else {
                    tgt = (base > secs) ? base - secs : 0u;
                }
                fl_seek_intent = tgt;

                uint64_t landed = 0;
                uint32_t at = flac_seek_locate((uint64_t)tgt * (uint64_t)fl.rate,
                                               &landed);
                seek_req = 0;
#if UI_SHOW_SEEK_DIAG
                /* A REFUSED seek leaves the rows stale and reads as "nothing
                 * happened", which is the one outcome that must not be
                 * ambiguous. Record it. */
                if (!at) { dg_tgt = tgt; dg_at = 0; dg_pos = 0; dg_fe = 98u; }
                dg_intent = fl_seek_intent;
#endif
                if (at && at != file_pos) {
                    file_pos = at;
                    stopped  = 0;
                    /* The landing was MEASURED before the jump, not assumed,
                     * so the clock is simply set to it. */
                    ui_sec      = (uint32_t)(landed / (uint64_t)fl.rate);
                    ui_sec_acc  = 0;
                    ui_last_sec = 0xFFFFFFFFu;
                    ui_prog_sec = 0xFFFFFFFFu;
                    meas_pos0   = file_pos;
                    meas_sec0   = ui_sec;
#if UI_SHOW_SEEK_DIAG
                    dg_tgt  = tgt;   dg_at   = at;
                    dg_blk  = fl.max_blocksize;
                    dg_rate = fl.rate;
                    dg_ui   = ui_sec;
                    dg_pos  = fl.rate ? (uint32_t)(landed / (uint64_t)fl.rate)
                                      : 0u;
                    dg_n    = 0;     dg_fe   = 0xFFu;  dg_live = 1u;
                    dg_num[0] = dg_num[1] = dg_num[2] = 0;
#endif

                    pcm_flush();
                    refill_drain();
                    ring_fill = 0; ring_rd = 0;
                    flac_flush_input(&fl);
                    flac_stall = 0;
                    fl_meter_n = 0;
                    /* From the MEASURED landing, not from the request: the
                     * two are the same only because the offset was refined
                     * until they were, and if a pathological file leaves them
                     * a frame apart the audio is what counts. */
                    if (fl.max_blocksize)
                        frames = (uint32_t)(landed / (uint64_t)fl.max_blocksize);
                    /* Must follow the rebase, and must equal it: the clock
                     * accumulator fires on `frames != ui_last_frames`, so a
                     * stale value here spends a phantom frame -- and the
                     * sentinel 0xFFFFFFFF would guarantee one. */
                    ui_last_frames = frames;
                    if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
                    if (paused) { ui_draw_dynamic(); continue; }
                }
                goto seek_done;
            }

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

                if (track_fmt == FMT_FLAC) {
                    /* The ring just moved; the decoder must not carry the old
                     * position's buffered bytes and half-consumed bit
                     * reservoir across with it. Without this it decodes stale
                     * input as though it belonged at the new offset and never
                     * resyncs -- the hang. frame_header() then scans for the
                     * next sync from clean state, and rejects a false one via
                     * its blocksize check against ch0_cap. */
                    flac_flush_input(&fl);
                    flac_stall  = 0;
                    fl_meter_n  = 0;

                    /* Rebase the frame counter. The FLAC path derives ui_sec
                     * from `frames`, so leaving it alone would let the next
                     * decoded frame snap the clock straight back to where the
                     * seek started. */
                    if (fl.max_blocksize)
                        frames = (uint32_t)(((uint64_t)ui_sec * (uint64_t)fl.rate)
                                            / (uint64_t)fl.max_blocksize);
                }

                if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
                if (paused) { ui_draw_dynamic(); continue; }
            }
        seek_done: ;
        }

        /* ---- playlist overlay ------------------------------------------
         * Serviced here rather than in poll_input() because pl_play_at() lives
         * in playlist.inc, which is included AFTER poll_input -- the same
         * reason every other cross-file action in this loop is a request flag.
         *
         * Drawing is gated on pl_ui_dirty, so an open overlay costs nothing
         * per frame; it repaints when the selection moves and not otherwise.
         * That matters more than it looks: the decoder keeps running
         * underneath, and FLAC has less headroom than MP3. */
        /* load_track() paints the player card, so an auto-advance while the
         * overlay is up draws straight through it -- and the '>' marker has
         * moved anyway. Comparing against what was last drawn catches both,
         * and any other route that changes the position. */
        if (pl_ui_open && pl_ui_drawn_pos != pl_pos) pl_ui_dirty = 1u;
        if (pl_ui_open && pl_ui_dirty) {
            pl_ui_dirty = 0;
            pl_ui_draw();
        }

        /* Scroll the selected row when its name does not fit. Only that row is
         * repainted, so this costs one row of drawing a few times a second
         * rather than the whole list -- the decoder is still running.
         *
         * Whole characters, like the title marquee: the engine will place a
         * glyph at any x but cannot clip one partly off the left edge. */
        if (pl_ui_open && pl_count) {
            if (pl_ui_mq_sel != pl_ui_sel) {
                pl_ui_mq_sel  = pl_ui_sel;
                pl_ui_mq_off  = 0;
                pl_ui_mq_next = cycles() + CLK_HZ;      /* hold at the start */
            } else if ((int32_t)(cycles() - pl_ui_mq_next) >= 0) {
                char nm[64];
                pl_ui_label(pl_ui_sel, nm, sizeof(nm));
                if (fb_text_width(nm, TS_1X) > PL_UI_W - 40u) {
                    uint32_t len = 0;
                    while (nm[len]) len++;
                    pl_ui_mq_next = cycles() + CLK_HZ / 3u;
                    if (++pl_ui_mq_off >= len) {
                        pl_ui_mq_off  = 0;
                        pl_ui_mq_next = cycles() + CLK_HZ;  /* pause, then again */
                    }
                    if (pl_ui_sel >= pl_ui_top &&
                        pl_ui_sel <  pl_ui_top + PL_UI_ROWS)
                        pl_ui_row(pl_ui_sel - pl_ui_top);
                } else {
                    /* Fits: nothing to scroll, so back off rather than
                     * re-measuring it every few milliseconds. */
                    pl_ui_mq_next = cycles() + CLK_HZ;
                }
            }
        }
        if (pl_ui_restore) {
            pl_ui_restore = 0;
            /* ui_draw_chrome() paints the gradient itself -- calling it here
               too would push ~360 rects twice for one repaint. */
            ui_draw_chrome();
            if (art_ready && art_shown) ui_art_draw();
            /* The meters cache what they last drew; the overlay painted over
             * all of it, so every column has to be considered stale. */
            ui_wave_force = 1u;
            for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
            for (uint32_t z = 0; z < SPEC_BANDS; z++) spec_drawn[z] = 0xFFu;
            }
            ui_mode_dirty = 1u;
            ui_last_info  = 0xFFFFFFFFu;
            ui_last_sec   = 0xFFFFFFFFu;
            ui_prog_sec   = 0xFFFFFFFFu;
        }
        if (pl_ui_play_req) {
            pl_ui_play_req = 0;
            if (pl_count && pl_ui_sel < pl_count) {
                stop_req = 0;
                if (pl_play_at(pl_ui_sel)) { ui_mode_dirty = 1u; continue; }
                ui_toast_msg("TRACK WOULD NOT OPEN");
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
            for (uint32_t z = 0; z < SPEC_BANDS; z++) spec_drawn[z] = 0xFFu;
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
            for (uint32_t z = 0; z < SPEC_BANDS; z++) spec_drawn[z] = 0xFFu;
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

        if (track_fmt == FMT_FLAC) {
            /* One FLAC frame per pass. The decoder pulls from the ring and
             * pushes to the FIFO through the two adapters, so this loop keeps
             * its existing shape -- UI, input and end-of-track all still run
             * between frames. */
            /* The MP3 loop has this net a few lines further down; the FLAC
             * loop needs its own, and the count is what tells the diag row a
             * hiccup actually happened rather than was imagined. */
            if (!under_shadow && pcm_underrun()) {
                under_shadow = 1u;
                pcm_under_n++;
                fade_left    = FADE_SAMPLES;
            }
            flac_err fe = flac_decode_frame(&fl, flac_emit, 0);
            /* Meters are fed from flac_emit on a fixed 1152-pair interval,
             * not here: once per frame is 9.6 Hz and looks delayed. */
            if (fe == FLAC_END || fe == FLAC_ERR_SHORT) {
                if (pl_advance_auto()) { ui_mode_dirty = 1; continue; }
                if (pl_count && rep_mode == REP_OFF &&
                    pl_pos + 1u >= pl_count) {
                    paused |= 1u;
                    ui_toast_msg("END OF PLAYLIST");
                }
                soft_restart_req = 1;
                continue;
            }
            if (fe != FLAC_OK) { errs++; REG(R_STAT2) = 0xC2000000u | fe; }
            frames++;
#if UI_SHOW_SEEK_DIAG
            /* The three frames after a seek, exactly as the decoder saw them.
             * Recorded even when fe is an error, because "the first frame
             * failed" is itself a candidate explanation. */
            if (dg_live && dg_n < 3u) {
                if (dg_fe == 0xFFu) dg_fe = (uint8_t)fe;
                dg_num[dg_n++] = (uint32_t)fl.frame_number;
                dg_samp = fl.number_is_sample;
                if (dg_n == 1u && fl.rate) {
                    uint64_t smp = fl.number_is_sample
                                 ? fl.frame_number
                                 : fl.frame_number * (uint64_t)fl.max_blocksize;
                    dg_pos = (uint32_t)(smp / fl.rate);
                }
                if (dg_n >= 3u) dg_live = 0u;
            }
#endif
            /* The clock is NOT set here. ui_draw_dynamic() already advances
             * ui_sec by an accumulator whenever `frames` moves, and this line
             * recomputed it independently -- two writers, the same nominal
             * rate, different rounding, so they disagreed by a second
             * depending on which ran last. That was the +-1s twitch on the
             * progress row, and driving the UI more often made it worse by
             * running the accumulator far more often.
             *
             * The accumulator is also the cheaper of the two: samp_per_frame
             * is fl.max_blocksize and samprate is fl.rate, so it is adds and
             * compares in place of a 64-bit divide (__udivdi3, hundreds of
             * cycles on RV32) once per frame. */
            st0 |= (1u << 3);
            REG(R_STAT0) = st0;
            if (!ui_dump_mode) ui_draw_dynamic();
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
            pcm_under_n++;
            fade_left    = FADE_SAMPLES;
        }

        int n = fi.outputSamps;                  /* interleaved L,R */
        int stereo = (fi.nChans == 2);

        meters_feed(pcm, n, stereo);

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
            if (PCM_FULL(REG(R_PCM_ST))) {
                uint32_t t0 = cycles();
                do {
                    poll_input();
                    refill_pump();
                    if (reload_pending) { fl_idle_cyc += cycles() - t0;
                                          goto next_outer; }
                } while (PCM_FULL(REG(R_PCM_ST)));
                fl_idle_cyc += cycles() - t0;
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
