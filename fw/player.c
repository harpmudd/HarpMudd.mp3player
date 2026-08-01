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

/* Target command selector, written to R_TGT_GO bits [1:0]. */
#define TGT_READ     0u   /* 0180 */
#define TGT_OPENFILE 1u   /* 0192 */
#define TGT_GETFILE  2u   /* 0190 */

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
#define EXPECT_VERSION 0x4D503310u   /* rev 16: param struct at word 64   */

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

/* Proportional advance. The engine paints the full 16-px cell, and glyphs are
 * left-aligned within it, so stepping by the ink width overwrites only the
 * previous glyph's blank padding -- proportional spacing without needing a
 * transparent blit. */
static uint32_t fb_adv(char ch, uint32_t sx)
{
    unsigned char c = (unsigned char)ch;
    if (c < FONT_FIRST || c > FONT_LAST) c = ' ';
    return ((uint32_t)font_adv[c - FONT_FIRST] * ts_half[sx]) / 2u;
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
static void fb_copy(uint32_t sx_, uint32_t sy_, uint32_t dx, uint32_t dy,
                    uint32_t w, uint32_t h)
{
    if (!w || !h) return;
    uint32_t src = sy_ * FB_STRIDE + sx_;
    fb_wait();
    REG(R_FB_ADDR)  = dy * FB_STRIDE + dx;
    REG(R_FB_SIZE)  = (h << 9) | w;
    REG(R_FB_COLOR) = ((src >> 16) & 0x7u) | ((src & 0xFFFFu) << 16);
    fb_color_shadow = 0xFFFFFFFFu;      /* colour regs clobbered -- invalidate */
    REG(R_FB_GO)    = FB_OP_COPY;
}

static void fb_char(uint32_t x, uint32_t y, char ch, uint32_t sx, uint32_t sy)
{
    fb_wait();
    REG(R_FB_ADDR) = y * FB_STRIDE + x;
    REG(R_FB_GO)   = FB_OP_CHAR
                   | (((uint32_t)(unsigned char)ch & 0x7Fu) << 3)
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
/* Up here with the other UI-visible state: the progress bar needs both, and C
 * needs them declared before ui_draw_dynamic(). */
static uint32_t audio_start;             /* first byte after any ID3 tag */
static uint32_t bytes_per_sec = 16000u;  /* refined once decoding */
static uint32_t track_kbps, track_hz;
/* Total frames declared by a Xing/Info/VBRI header, 0 when the file has none.
 * Duration from size/bitrate is only right for CBR -- on a VBR file the first
 * frame's bitrate is not the file's average, which is why both the total time
 * and the progress bar were off. */
static uint32_t track_frames, track_secs;
/* Average byte rate measured from real playback. Converges on the truth for
 * VBR files that carry no Xing header, which the first-frame bitrate cannot. */
static uint32_t meas_rate;

/* Ring cursors live up here with the UI state: the measured-rate calculation
 * needs them, and C wants them declared before ui_draw_dynamic(). */
static uint32_t file_pos;       /* next byte offset to fetch from the file   */
static uint32_t ring_fill;      /* valid bytes currently in the ring         */
static uint32_t ring_rd;        /* read cursor within the ring               */ /* stream format, known only once decoding starts */
static uint32_t ui_info_y;            /* where chrome left room for the format line */
static uint32_t ui_last_info, ui_last_prog, ui_pause_next, ui_breath, ui_icon_next;
static uint32_t ui_arr_t, ui_wave_force, ui_accent_changed;
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

/* Transport state. Declared up here with the other UI-visible globals rather
 * than down with poll_input(): ui_draw_dynamic() shows the play/pause state,
 * and C needs the declaration before that use. */
/* Volume as a straight percentage, 0..100, where 100 is unity gain -- the
 * loudest the stream goes without clipping. Applied as a Q8 fixed-point gain
 * recomputed once per change, because a divide per sample would be 2304
 * software divides every frame inside the decode budget. */
#define VOL_MAX   100u
#define VOL_STEP  5u
static uint32_t paused, volume = VOL_MAX;
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
#define PL_MAX       128u        /* tracks; 128 * 6 bytes of index is cheap */
#define PL_TEXT_MAX  8192u       /* the .m3u itself, names only after parsing */

static char     pl_text[PL_TEXT_MAX];
static uint16_t pl_off[PL_MAX];          /* byte offset of each name in pl_text */
static uint16_t pl_order[PL_MAX];        /* play order -> file index            */
static uint16_t pl_count;                /* 0 = no playlist loaded              */
static uint16_t pl_pos;                  /* index INTO pl_order                 */

enum { REP_OFF = 0, REP_ALL, REP_ONE };
static uint8_t  rep_mode;                /* cycles off -> all -> one -> off */
static uint8_t  shuffle_on;
static uint32_t pl_rng = 1u;             /* shuffle RNG, seeded from cycles() */
static uint32_t ui_mode_dirty = 1u;      /* repaint the mode icons / N-of-M   */

#define PL_HOLD_MS 400u                  /* Left/Right held this long = skip  */

/* Defined in playlist.inc, called from the input handler above it. */
static void pl_reorder(void);
static void pl_resync(uint16_t file_idx);

static uint32_t seek_req;                /* +1 forward, -1 back (as unsigned) */
static uint32_t restart_req;       /* B button: full reload, re-reads the tag */
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
static uint32_t cur_file_id;           /* 0190 identity of the loaded file   */
static uint32_t stale_ref_file_id;     /* ...and of the one being left       */
static uint32_t reload_retries;
static uint32_t tag_corrections;   /* periodic probe found a wrong tag */
static int      tag_fix_budget;    /* probe-triggered restarts left; see tag_probe_apply */

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
#define UI_TRACK   0x18E3u   /* unfilled part of the meter -- a visible track
                              * rather than bare background, so the meter reads
                              * as one object at any level */
#define UI_RED     0xF800u

#define UI_FAINT   0x6B4Du   /* filename line -- present but recessive */
#define UI_CARD_H  120u
#define UI_SHOW_DIAG 0        /* 1 = show A/S/T/F reload diagnostics */

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
/* One marquee per scrollable line. Title and artist can both overflow, and
 * they scroll independently -- a shared position would drag the shorter one
 * around for no reason. */
typedef struct {
    char     text[64];
    uint32_t y, scale, on, pos, next;
} ui_marquee_t;
static ui_marquee_t ui_mq_title, ui_mq_artist;
static unsigned char wave[UI_WAVE_N], wave_drawn[UI_WAVE_N];
static unsigned char wave_pk[UI_WAVE_N], wave_pk_drawn[UI_WAVE_N];

/* Art panel: art_x is where it currently sits, animated toward its target.
 * FB_W means fully off the right edge. */
/* Shown by default; SELECT hides it. art_x is where the panel currently sits,
 * and FB_W means fully off the right edge. */
static uint32_t art_x = ART_X, art_shown = 1, art_ready, ui_text_w;
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
        uint16_t top = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              (y + i) * UI_BANDS / FB_H, UI_BANDS);
        uint16_t bot = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              (y + h - 1u - i) * UI_BANDS / FB_H, UI_BANDS);
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
    uint32_t band = (FB_H + UI_BANDS - 1u) / UI_BANDS;
    for (uint32_t i = 0; i < UI_BANDS; i++) {
        uint32_t y = i * band;
        if (y + band <= ART_Y || y >= ART_Y + ART_H) continue;
        uint32_t y0 = y > ART_Y ? y : ART_Y;
        uint32_t y1 = (y + band < ART_Y + ART_H) ? (y + band) : (ART_Y + ART_H);
        fb_rect(x, y0, w, y1 - y0,
                ui_mix(UI_GRAD_TOP, UI_GRAD_BOT, i, UI_BANDS));
    }
}

/* Blit the stash to the current position, clipped at the right edge. The panel
 * enters from the right, so only its leftmost columns are on screen at first --
 * and an unclipped copy would run past column 400 into the memory the NEXT
 * scanline displays, i.e. corruption elsewhere rather than a clean cut. */
static void ui_art_draw(void)
{
    if (!art_ready || art_x >= FB_W) return;
    uint32_t w = FB_W - art_x;
    if (w > ART_W) w = ART_W;
    fb_copy(0, ART_STASH_Y, art_x, ART_Y, w, ART_H);
}

static void ui_gradient(void)
{
    uint32_t band = (FB_H + UI_BANDS - 1u) / UI_BANDS;
    for (uint32_t i = 0; i < UI_BANDS; i++) {
        uint32_t y = i * band;
        uint32_t h = (y + band > FB_H) ? (FB_H - y) : band;
        if (!h) break;
        fb_rect(0, y, FB_W, h, ui_mix(UI_GRAD_TOP, UI_GRAD_BOT, i, UI_BANDS));
    }
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
    if (meas_rate) return meas_rate;
    return bytes_per_sec;
}

static uint32_t ui_total_secs(void)
{
    if (track_secs) return track_secs;          /* Xing/VBRI: exact */
    uint32_t rate = ui_byte_rate();
    if (slot_size > audio_start && rate)
        return (slot_size - audio_start) / rate;
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
        uint16_t top = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              (ART_Y + i) * UI_BANDS / FB_H, UI_BANDS);
        uint16_t bot = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              (ART_Y + ART_H - 1u - i) * UI_BANDS / FB_H, UI_BANDS);
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
    fb_rect(ART_PAD - 1u, ART_STASH_Y + ART_PAD - 1u,
            ART_IMG + 2u, ART_IMG + 2u, UI_FAINT);
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
    uint32_t band = (FB_H + UI_BANDS - 1u) / UI_BANDS;
    for (uint32_t i = 0; i < UI_BANDS; i++) {
        uint32_t y = i * band;
        if (y + band <= UI_WAVE_Y || y >= UI_WAVE_Y + UI_WAVE_H) continue;
        uint32_t y0 = y > UI_WAVE_Y ? y : UI_WAVE_Y;
        uint32_t y1 = (y + band < UI_WAVE_Y + UI_WAVE_H)
                    ? (y + band) : (UI_WAVE_Y + UI_WAVE_H);
        fb_rect(UI_MARGIN, y0, UI_INNER_W, y1 - y0,
                ui_mix(UI_GRAD_TOP, UI_GRAD_BOT, i, UI_BANDS));
    }
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
    uint32_t ts = fb_text_fit(title, ui_text_w, TS_2X);
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
    ui_last_prog  = 0xFFFFFFFFu;
    ui_sec = 0; ui_sec_acc = 0; ui_last_frames = 0xFFFFFFFFu;
    ui_prog_sec   = 0xFFFFFFFFu;
    track_kbps    = 0;
    ui_underrun_shown = 0;
}

/* A load failure used to spin in `for(;;){}`, which is the worst possible
 * outcome: an I/O problem became a frozen screen with no information, and the
 * user could not even pick another file. Say what happened, keep the status
 * bytes on screen, and stay responsive so the Core menu can load a new track. */
static void poll_input(void);

static void ui_load_failed(void)
{
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
        for (uint32_t i = 0; i < UI_WAVE_N; i++) {
            wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
        }
        ui_wave_force = 1;              /* recolour even while paused      */
        ui_last_prog  = 0xFFFFFFFFu;    /* progress fill                   */
        ui_last_pause = 0xFFFFFFFFu;    /* PLAYING label                   */
        ui_icon_next  = cycles();       /* arrows, on the next tick        */
        ui_mode_dirty = 1u;             /* repeat/shuffle icons, N-of-M    */
    }

    /* Frozen, not decaying, while paused: shifting the history along with a
     * zero sample scrolled the whole waveform off the screen, which reads as
     * "lost the audio" rather than "stopped". */
    if ((!paused || ui_wave_force) && ++ui_last_vu >= 2u) {
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
        uint16_t bed = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              UI_WAVE_Y * UI_BANDS / FB_H, UI_BANDS);

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
        ui_sec_acc += 1152u;
        while (ui_sec_acc >= samprate) { ui_sec_acc -= samprate; ui_sec++; }
    }
    uint32_t sec = ui_sec;

    /* Measured average: bytes actually consumed divided by elapsed time. Only
     * after a few seconds, so a partly-filled ring cannot skew it. */
    if (sec >= 4u && !track_secs) {
        uint32_t buffered = ring_fill - ring_rd;
        uint32_t consumed = (file_pos > audio_start + buffered)
                          ? file_pos - buffered - audio_start : 0u;
        if (consumed) meas_rate = consumed / sec;
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
        uint16_t cbg = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              UI_TIME_Y * UI_BANDS / FB_H, UI_BANDS);
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

    /* Format line, drawn once the decoder has told us what the stream is. */
    uint32_t info = track_kbps * 1000u + track_hz / 100u;
    if (track_kbps && info != ui_last_info) {
        ui_last_info = info;
        char b[32], *q = b;
        q = ui_dec(q, track_kbps);
        *q++ = ' '; *q++ = 'k'; *q++ = 'b'; *q++ = 'p'; *q++ = 's';
        *q++ = ' '; *q++ = '-'; *q++ = ' ';
        q = ui_dec(q, track_hz / 1000u);
        *q++ = '.';
        q = ui_dec(q, (track_hz % 1000u) / 100u);
        *q++ = ' '; *q++ = 'k'; *q++ = 'H'; *q++ = 'z';
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
                ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                       UI_PROG_Y * UI_BANDS / FB_H, UI_BANDS));
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
        uint16_t tbg2 = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                               UI_TOAST_Y * UI_BANDS / FB_H, UI_BANDS);
        uint32_t age  = cycles() - ui_toast_t0;
        uint32_t step = (age <= UI_TOAST_HOLD) ? 0u
                      : ((age - UI_TOAST_HOLD) * UI_TOAST_STEPS) / UI_TOAST_FADE;
        if (step > UI_TOAST_STEPS) step = UI_TOAST_STEPS;

        if (step != ui_toast_step) {
            ui_toast_step = step;
            if (step >= UI_TOAST_STEPS) {
                fb_rect(UI_MARGIN, UI_TOAST_Y, UI_INNER_W, UI_TOAST_H, tbg2);
                ui_toast_t0 = 0;
            } else {
                fb_set_color(ui_mix(UI_WHITE, tbg2, step, UI_TOAST_STEPS), tbg2);
                fb_text_clipped(UI_MARGIN, UI_TOAST_Y, ui_toast,
                                TS_1X, TS_1X, UI_INNER_W);
            }
        }
    }

    /* Transport state, in the gap to the right of the clock. Both strings are
     * the same length so one overwrites the other cleanly. */
    {
        uint16_t tbg = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              (UI_TIME_Y + 10u) * UI_BANDS / FB_H, UI_BANDS);
        /* Left end of its own row. The arrows sit at a FIXED x derived from
         * the WIDER of the two words, so switching PLAYING <-> PAUSED cannot
         * shuffle them sideways. */
        const uint32_t ly = UI_TRANSPORT_Y;
        const uint32_t lx = UI_MARGIN;
        const uint32_t lbl_w = fb_text_width("PLAYING", TS_1X);
        const uint32_t ix = lx + lbl_w + 12u;
        const uint32_t iy = ly;

        /* Breathe: a triangle over 64 draws. Paused redraws at ~30 Hz, playing
         * is throttled below, so both land near a 2 s cycle. */
        uint32_t ph  = (++ui_breath) & 63u;
        uint32_t lvl = (ph < 32u) ? ph : (63u - ph);

        if (paused) {
            uint16_t c = ui_mix(UI_PANEL, UI_WHITE, lvl, 31u);
            fb_rect(lx, ly, lbl_w + 8u, FB_CELL(TS_1X), tbg);
            fb_set_color(c, tbg);
            fb_text_clipped(lx, ly, "PAUSED", TS_1X, TS_1X, lbl_w + 8u);
            fb_rect(ix, iy, UI_ARR_SPAN, UI_ICONBOX_H, tbg);
            ui_icon_pause(ix, iy, c);
            ui_last_pause = 0xFFFFFFFFu;      /* force a repaint on resume */
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

            fb_rect(mx, iy, (UI_MODE_W + 10u) * 2u, UI_ICONBOX_H, tbg);
            /* Inactive modes stay visible but recede, so the controls advertise
             * themselves instead of only appearing once found. */
            ui_icon_repeat(mx, my,
                           rep_mode == REP_OFF ? UI_FAINT : ui_accent,
                           rep_mode == REP_ONE);
            ui_icon_shuffle(mx + UI_MODE_W + 10u, my,
                            shuffle_on ? ui_accent : UI_FAINT);

            /* "4 / 12", right-aligned so the numbers do not shuffle sideways as
             * the track index gains a digit. */
            if (pl_count) {
                char pos[16]; char *q = pos;
                q = ui_dec(q, (uint32_t)(pl_pos + 1u));
                *q++ = ' '; *q++ = '/'; *q++ = ' ';
                q = ui_dec(q, (uint32_t)pl_count);
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
        uint16_t dbg = ui_mix(UI_GRAD_TOP, UI_GRAD_BOT,
                              (FB_H - 24u) * UI_BANDS / FB_H, UI_BANDS);
        fb_rect(UI_MARGIN, FB_H - 24u, UI_INNER_W, FB_CELL(TS_1X), dbg);
        fb_set_color(UI_RED, dbg);
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
 * B         : full reload (re-reads the tag)
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

    if (edge & (KEY_A | KEY_START)) paused ^= 1u;
    if (edge & KEY_B)               restart_req = 1u;

    /* ---- Left/Right: tap seeks, hold skips track ---- */
    {
        const uint32_t kmask[2] = { KEY_LEFT, KEY_RIGHT };
        const uint32_t hold_cy  = CLK_HZ / 1000u * PL_HOLD_MS;
        for (int i = 0; i < 2; i++) {
            if (edge & kmask[i]) { lr_t0[i] = cycles(); lr_fired[i] = 0; }

            /* Fire the skip the moment the threshold passes rather than on
             * release: holding a button and having nothing happen until you
             * let go feels broken, and repeat-skip needs the same shape. */
            if ((keys & kmask[i]) && !lr_fired[i] &&
                (int32_t)(cycles() - lr_t0[i]) >= (int32_t)hold_cy) {
                lr_fired[i] = 1;
                if (pl_count) {
                    skip_req = i ? 1u : (uint32_t)-1;
                } else {
                    ui_toast_msg("NO PLAYLIST");
                }
            }

            if (fall & kmask[i]) {
                if (!lr_fired[i]) {          /* a tap: the original seek */
                    seek_req = i ? 1u : (uint32_t)-1;
                    ui_toast_msg(i ? "SEEK +5s" : "SEEK -5s");
                }
                lr_fired[i] = 0;
            }
        }
    }

    if (edge & KEY_UP)   { volume = (volume + VOL_STEP > VOL_MAX)
                                  ? VOL_MAX : volume + VOL_STEP;
                           vol_apply(); ui_toast_set("VOLUME", volume, "%"); }
    if (edge & KEY_DOWN) { volume = (volume < VOL_STEP) ? 0u : volume - VOL_STEP;
                           vol_apply(); ui_toast_set("VOLUME", volume, "%"); }

    /* ---- Select as a modifier for L/R ---- */
    if (edge & KEY_SELECT) sel_used = 0;

    if (keys & KEY_SELECT) {
        if (edge & KEY_L1) {
            sel_used = 1;
            rep_mode = (uint8_t)((rep_mode + 1u) % 3u);
            ui_toast_msg(rep_mode == REP_OFF ? "REPEAT OFF"
                       : rep_mode == REP_ALL ? "REPEAT ALL" : "REPEAT ONE");
            ui_mode_dirty = 1u;
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
        }
    } else {
        if (edge & KEY_R1) { ui_pal_idx = (ui_pal_idx + 1u) % UI_PALETTE_N;
                             ui_accent_changed = 1u; }
        if (edge & KEY_L1) { ui_pal_idx = (ui_pal_idx + UI_PALETTE_N - 1u) % UI_PALETTE_N;
                             ui_accent_changed = 1u; }
    }

    /* Only a Select that was NOT used as a modifier toggles the art panel. */
    if ((fall & KEY_SELECT) && !sel_used) art_toggle = 1u;

    /* Pause while the OS menu ("Load MP3" etc) is open, without clobbering the
     * user's own A/Start pause -- bit 1 is the menu's, bit 0 is theirs. */
    if (in & IN_MENU) paused |= 2u; else paused &= ~2u;

    /* Only SET the flag here. This runs from inside the sample-push wait,
     * which is where the CPU spends most of its time when keeping up, so a
     * reload is noticed immediately instead of after the current frame. */
    if (REG(R_RELOAD) & RL_PENDING) reload_pending = 1u;
    if (REG(R_RELOAD) & RL_PL_RELOAD) pl_reload_pending = 1u;
}

/* Drops every sample queued in the hardware FIFO. Required on any
 * discontinuity (track change, seek, restart) -- without it the OLD position's
 * queued ~43 ms keeps draining while the new position spins up. */
static inline void pcm_flush(void) { REG(R_PCM_ST) = 1u; }

static uint32_t rd_seq0, rd_deadline, rd_len;
static int      rd_pending, rd_ok;

/* What the in-flight read is for. Only one APF command can be outstanding, so
 * both consumers share the slot and completion dispatches on this. */
enum { RD_RING, RD_TAG };
static int      rd_kind;

/* Periodic ID3 re-probe: a light backstop behind the 0190 identity gate. */
static uint32_t tag_next;
static uint32_t tag_probes_left;

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
    rd_kind = RD_RING;                  /* retires here, not in the pump */
    target_read_start_slot(slot, off, dst_off, len);
    while (!target_read_poll()) { }
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

static void tag_probe_apply(void);

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
        if (!rd_ok) { st0 |= (1u << 4); REG(R_STAT0) = st0; return; }
        if (rd_kind == RD_TAG) tag_probe_apply();
        else { file_pos += rd_len; ring_fill += rd_len; }
        return;
    }

    /* Ring first, always -- audio starvation beats a late tag update. */
    if (ring_fill - ring_rd >= RING_SIZE / 2u) {
        if (tag_probes_left && (int32_t)(cycles() - tag_next) >= 0) {
            tag_probes_left--;
            tag_next = cycles() + CLK_HZ * 2u;
            /* NO slot-cache flush here: flushing makes APF forget the cluster
             * chain, so every following refill has to re-walk it. That is a
             * load-time tool; on the streaming path it starves the decoder. */
            rd_kind  = RD_TAG;
            target_read_start(0, TAG_OFF, TAG_SIZE);
        }
        return;
    }
    rd_kind = RD_RING;

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
            fsize > 1) {
            uint8_t enc = ring[p + 10];
            if (enc == 1u || enc == 2u) return ID3_UNSUPPORTED_ENCODING;
            uint32_t n = fsize - 1u;
            if (n > out_size - 1u) n = out_size - 1u;
            uint32_t i;
            for (i = 0; i < n; i++) {
                uint8_t c = ring[p + 11 + i];
                if (c == 0) break;
                out[i] = (char)c;
            }
            out[i] = 0;
            return (i > 0) ? ID3_OK : ID3_NO_FRAME;
        }
        p += 10u + fsize;
    }
    return ID3_NO_FRAME;
}

/* "This is still the file we were told we are leaving."
 *
 * Compares the parsed TITLE, not the raw head bytes: the first four bytes of
 * any ID3v2.3 tag are "ID3" plus a version, identical across essentially every
 * tagged MP3, so that comparison was comparing a constant and answered "stale"
 * for every read including correct ones. */
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

/* Applies a completed periodic head re-read. A stale post-reload read gets
 * BOTH the tag and the start offset wrong together, so this does not merely
 * refresh the caption: if the probed tag disagrees, the offset being played
 * from was wrong too. */
static void tag_probe_apply(void)
{
    uint32_t skip = id3_len(tagbuf);
    char t[48] = { 0 }, a[48] = { 0 };
    int  st = ID3_NO_TAG;
    if (skip) {
        st = id3_find_text(tagbuf, TAG_SIZE, skip, "TIT2", t, sizeof(t));
        id3_find_text(tagbuf, TAG_SIZE, skip, "TPE2", a, sizeof(a));
    }

    if (title_is_stale(t)) return;      /* still the previous file */

    int same_title = 1;
    for (uint32_t i = 0; i < sizeof(t); i++)
        if (t[i] != track_title[i]) { same_title = 0; break; }

    /* Consistent -> nothing to change. Deliberately does NOT stop probing: a
     * stale probe agrees with the equally-stale caption, so stopping here once
     * switched the mechanism off in exactly the case it exists for. */
    if (same_title && skip == audio_start) return;

    for (uint32_t i = 0; i < sizeof(t); i++) track_title[i]  = t[i];
    for (uint32_t i = 0; i < sizeof(t); i++) last_title[i]   = t[i];
    for (uint32_t i = 0; i < sizeof(a); i++) track_artist[i] = a[i];
    title_status = st;
    for (int i = 0; i < 4; i++) head_bytes[i] = tagbuf[i];
    tag_corrections++;

    ui_draw_chrome();

    if (skip != audio_start && tag_fix_budget > 0) {
        tag_fix_budget--;
        audio_start = skip;
        /* SOFT restart -- reposition only, do NOT re-read the head. Running a
         * full load here re-reads offset 0, and a second stale read overwrote
         * the very title this probe had just recovered. */
        soft_restart_req = 1;
    }
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
            return ((uint32_t)ring[i+8]  << 24) | ((uint32_t)ring[i+9]  << 16) |
                   ((uint32_t)ring[i+10] << 8)  |  (uint32_t)ring[i+11];
        }
        if (a == 'V' && b == 'B' && c == 'R' && d == 'I')
            return ((uint32_t)ring[i+14] << 24) | ((uint32_t)ring[i+15] << 16) |
                   ((uint32_t)ring[i+16] << 8)  |  (uint32_t)ring[i+17];
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
    for (;;) {

    /* Drop APF's stale fragment cache before reading -- inside the loop, so a
     * retry is a fresh attempt rather than another read of the same chain. */
    target_flush_slot_cache();

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
    track_secs   = 0;
    meas_rate    = 0;

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
    track_frames = vbr_frame_count();
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
    bytes_per_sec = 16000u;
    paused = 0;
    seek_req = 0; restart_req = 0; soft_restart_req = 0;
    st0 = 0; REG(R_STAT0) = 0; REG(R_STAT1) = 0; REG(R_STAT2) = 0; REG(R_STAT3) = 0;
    st0 |= (1u << 0); REG(R_STAT0) = st0;            /* decoder up */

    /* The RTL only latches R_SLOT_SZ on a reload edge, so at boot there is
     * genuinely nothing to read. Take APF's number when there is one. */
    slot_size = REG(R_SLOT_SZ);

    if (!read_track_head()) { REG(R_STAT2) = 0xE0000000u; return 0; }
    if (audio_start) { st0 |= (1u << 1); REG(R_STAT0) = st0; }

    if (slot_size <= audio_start) slot_size = probe_file_size();

    /* Cover art BEFORE the chrome, because whether it exists decides the
     * layout: no art means no panel and a full-width waveform. Also before
     * prefill, so its blocking reads cannot starve playback. */
    art_ready = 0;
    ui_art_mount();
    int has_art = art_decode(audio_start);
    if (!has_art) ui_art_placeholder();
    ui_art_round();
    art_ready = 1;

    /* Panel state follows the TRACK, not the session. art_x is set directly
     * rather than animated -- a track change should not look like a slide. */
    art_shown = (uint32_t)has_art;
    art_x     = has_art ? ART_X : FB_W;

    ui_draw_chrome();   /* title/artist are populated now -- draw the UI */

    /* Light backstop only; the reload waits on 0190's authoritative identity. */
    tag_probes_left = 4;
    tag_next        = cycles() + CLK_HZ;

    if (!prefill()) { REG(R_STAT2) = 0xD0000000u; return 0; }
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
    /* Before the track, deliberately: reading the playlist slot makes APF drop
     * its fragment cache for the MP3 slot, so doing it once here costs nothing
     * while doing it mid-stream would make every refill re-walk the cluster
     * chain. No playlist on the card simply leaves pl_count at 0. */
    pl_load();
    tag_fix_budget = 2;
    if (!load_track()) ui_load_failed();
    pl_report();

    for (;;) {
        poll_input();

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

            tag_fix_budget = 2;
            REG(R_RELOAD)  = 1;             /* ack */
            reload_pending = 0;
            reload_armed    = 1;
            reload_probe_at = cycles();
            reload_settle   = cycles() + CLK_HZ * 3u / 2u;   /* blind fallback */
            reload_at       = cycles() + CLK_HZ * 5u;        /* hard cap       */
        }
        if (reload_armed) {
            /* ASK APF, do not infer: slot_file_id() hashes the 0190 response,
             * so "has the slot changed?" is answered by the file's identity
             * rather than by its contents. */
            int ready = 0;
            if (stale_ref_file_id == 0u) {
                /* No identity to compare against. Loading immediately here
                 * skips the gate entirely and reads the slot before it has
                 * switched, which puts the old file's tag length and frame
                 * count on the new track. Settle instead of not waiting. */
                if ((int32_t)(cycles() - reload_settle) >= 0) ready = 1;
            } else if ((int32_t)(cycles() - reload_probe_at) >= 0) {
                reload_probe_at = cycles() + CLK_HZ / 10u;
                uint32_t id = slot_file_id();
                if (id != 0u && id != stale_ref_file_id) ready = 1;
            }

            if (ready || (int32_t)(cycles() - reload_at) >= 0) {
                reload_armed = 0;
                if (!load_track()) ui_load_failed();
                continue;
            }
        }

        /* The user picked a different playlist. Re-reading slot 3 flushes the
         * MP3 slot's fragment cache, so this pauses briefly rather than doing
         * it underneath a running stream. */
        if (pl_reload_pending) {
            pl_reload_pending = 0;
            REG(R_RELOAD) = RL_PL_RELOAD;            /* ack just this bit */
            pl_load();
            pl_report();
            if (pl_count) pl_play_at(0);
            ui_mode_dirty = 1;
            continue;
        }

        /* Track skip (Left/Right held). pl_play_at() issues 0192; APF then
         * raises 008A and the ordinary reload path does the actual loading. */
        if (skip_req) {
            uint32_t d = skip_req; skip_req = 0;
            if (pl_skip(d == 1u ? 1 : -1)) {
                ui_toast_set("TRACK", (uint32_t)(pl_pos + 1u), 0);
                ui_mode_dirty = 1;
            }
            continue;
        }

        if (art_toggle) {
            art_toggle = 0;
            art_shown ^= 1u;
            art_next   = cycles();          /* start moving immediately */
            /* Only the waveform changes shape; repainting the whole chrome
             * here would flash the text for no reason. */
            ui_wave_clear();
        }

        /* B = restart. A FULL reload rather than a rewind, so it re-resolves
         * both the tag and audio_start -- the manual escape hatch for a stale
         * post-reload read. */
        if (restart_req) {
            restart_req = 0;
            if (!load_track()) ui_load_failed();
            continue;
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
            file_pos  = audio_start;
            ring_fill = 0; ring_rd = 0;
            if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
            continue;
        }

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
                ui_draw_dynamic();
            }
            continue;
        }
        st0 &= ~(1u << 7); REG(R_STAT0) = st0;
        if (ui_was_paused) {
            ui_wave_force = 1;              /* recolour back to full on resume */
            for (uint32_t i = 0; i < UI_WAVE_N; i++) {
                wave_drawn[i] = 0xFFu; wave_pk_drawn[i] = 0xFFu;
            }
        }
        ui_was_paused = 0;

        if (seek_req) {
            /* ~5 s at the AUTHORITATIVE byte rate. Using the first frame's
             * bitrate made a "5 second" seek move by some other amount on any
             * VBR file -- the same error that made the total length wrong. */
            uint32_t step = ui_byte_rate() * 5u;
            if (seek_req == 1u) file_pos += step;
            else file_pos = (file_pos > audio_start + step)
                          ? file_pos - step : audio_start;
            seek_req = 0;

            /* Re-anchor the clock to the new POSITION. Elapsed time is
             * accumulated from the frame counter, which a seek does not touch,
             * so the readout and the bar otherwise ignored the jump. */
            uint32_t rate = ui_byte_rate();
            ui_sec      = rate ? (file_pos - audio_start) / rate : 0u;
            ui_sec_acc  = 0;
            ui_last_sec = 0xFFFFFFFFu;
            ui_prog_sec = 0xFFFFFFFFu;

            /* Same bleed-through problem as a track change. */
            pcm_flush();
            refill_drain();
            ring_fill = 0; ring_rd = 0;
            if (!prefill()) { st0 |= (1u << 4); REG(R_STAT0) = st0; }
        }

        /* Advance the asynchronous refill: start one if the ring has dropped
         * to half, or collect one that has landed. Never blocks. */
        refill_pump();

        int bytesLeft = (int)(ring_fill - ring_rd);
        if (bytesLeft < 512) {
            /* End of file: everything APF says the file holds has been read
             * and the ring is drained. Repeat -- with a playlist this is where
             * it advances instead, which is why it is a soft restart. */
            if (slot_size && file_pos >= slot_size && !rd_pending) {
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

        if (!rate_set && fi.samprate) {
            /* Drain at the file's true rate so pitch is right; sound_i2s
             * zero-order-holds up to its fixed 48 kHz. */
            uint64_t inc = ((uint64_t)fi.samprate << 32) / CLK_HZ;
            REG(R_PCM_RATE) = (uint32_t)inc;
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

        int n = fi.outputSamps;                  /* interleaved L,R */
        int stereo = (fi.nChans == 2);

        /* Real amplitude, not a proxy: max |sample| over the frame just
         * decoded, so the meter reflects what is actually playing. */
        {
            int32_t pk = 0;
            for (int i = 0; i < n; i++) {
                int32_t v = pcm[i];
                if (v < 0) v = -v;
                if (v > pk) pk = v;
            }
            peak_amp = (uint32_t)pk;
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

        ui_draw_dynamic();

next_outer: ;
    }

    return 0;
}
