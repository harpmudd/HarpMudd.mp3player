/* ================================================================== SETTINGS
 * Volume, accent colour, repeat, shuffle and the art panel, kept across
 * relaunches in a small file on the card.
 *
 * APF cannot CREATE a file -- 0184 writes into a slot that already has one --
 * so settings.bin ships with the core holding the defaults. A missing or
 * unreadable file is not an error: the built-in defaults simply stand.
 *
 * Saving is DEBOUNCED. Every write is an SD round trip, and holding Up on the
 * volume would otherwise issue one per repeat, in the same budget that keeps
 * the decoder fed. Changes mark the settings dirty; the write happens once
 * things have been quiet for a moment.
 */

/* WRITING IS DISABLED. Do not re-enable it without a mechanism, not a theory.
 *
 * THREE times now, every .mp3 in /Assets/mp3player/common/ has come back with
 * the SAME wrong size -- 21,037,825, then 21,365,505, then 20,382,465 -- while
 * settings.bin itself stayed exactly 32 bytes. Files being individually
 * overwritten does not look like that; a directory sector being rewritten with
 * bad size fields does, and every file in that folder shares one.
 *
 * The third event is the one that produced a mechanism, because the filesystem
 * turned out to be FINE. chkdsk reports no problems: the directory entries and
 * the cluster chains agree with each other. The files really ARE 20,382,465
 * bytes. Nothing is corrupt in the FAT sense -- they were EXTENDED, and the
 * extension was recorded correctly.
 *
 * What is established:
 *
 *   - The user's ORIGINAL files are intact and correct (5,462,016 / 5,480,448 /
 *     11,767,853 / 1,801,236). The copies on the card were grown from those, so
 *     this is damage, not a bad copy.
 *   - The card is never physically ejected -- it is mounted over Analogue's USB
 *     mode -- so an interrupted host write is not the explanation.
 *   - The settings magic ("SPM3", 0x53504D33) appears INSIDE one .mp3, at offset
 *     5,505,024: a 128 KB cluster boundary immediately past that file's real
 *     audio. Our bytes are in a file that is not ours.
 *   - The added region carries unrelated data, because exFAT does not zero newly
 *     allocated clusters. One file's audio no longer starts with a frame header,
 *     so "sizes wrong but audio intact" is retired.
 *
 * What is NOT established -- and a theory recorded here was WRONG, so read this
 * before building on it. All four files landing on one size looks like a write to
 * a fixed offset, where the result is offset + length regardless of the original.
 * But settings_store() passes OFFSET 0 and LENGTH 32. A write at offset 0 cannot
 * extend a file to 20 MB. So the parameters we set do not explain the size, and
 * the fault is either in what the registers actually carry by the time APF sees
 * them, or in APF's own handling. Do not assume it is the offset.
 *
 * The subdirectory mitigation stays in place -- settings.bin lives alone in
 * .../common/settings/ -- because it costs nothing and the layout is already
 * shipped. It never had a chance: the write is not landing where it is aimed, so
 * where the target sits in the directory tree is irrelevant.
 *
 * Reading stays enabled, so a settings.bin placed on the card by hand is still
 * honoured. Only the write-back is gone.
 *
 * TO RE-ENABLE: record every .mp3's size, perform ONE write with nothing else
 * running, and re-record. If any .mp3 grows, 0184 cannot be used by this core at
 * all and settings need an approach that never writes the card. Only if the
 * sizes hold is it worth asking which offset we are actually passing. Until that
 * test has been run and passed, this stays 0. */
#define SETTINGS_WRITE 0

/* That test, built. SETTINGS_PROBE=1 makes Select+Start perform exactly ONE
 * 0184 and then latch, so a whole session contains a single write with a known
 * before and after -- which normal operation cannot give you, because the
 * debounced pump fires on any settings change and there is no way to know
 * afterwards how many writes a session actually made.
 *
 * READ tools/settings_probe.md BEFORE RUNNING IT. The test is destructive by
 * design: if 0184 is what damages files, this damages them again. Put ONLY
 * expendable copies on the card.
 *
 * Set by `bash fw/build.sh probe`, never edited here: the checked-in source
 * must always build a core with no 0184 in it, and a diagnostic that is enabled
 * by editing a header is one that eventually ships enabled. */
#ifndef SETTINGS_PROBE
#define SETTINGS_PROBE 0
#endif

/* What has been RULED OUT by inspection, so the measurement does not get spent
 * re-checking it (2026-08-04):
 *
 *   - The slot ID is right. data.json declares Settings as id 4 and
 *     SET_SLOT_ID is 4, so the write is not aimed at another slot by a
 *     numbering mistake.
 *   - The slot is writeable. Analogue's data.json reference gives the
 *     `parameters` bitmap as bit 0 user-reloadable, 1 core-specific,
 *     2 nonvolatile filename, 3 READ-ONLY, 4 instance json, 5 init
 *     nonvolatile, 6 reset-on-load, 7 restart, 8 full reload, 9 persist
 *     filename. Ours is 0x1: bit 3 clear, and "must not be read-only" is the
 *     only slot requirement 0184 documents.
 *   - The parameters are the documented ones. 0184 takes slot id [15:0],
 *     slot offset, BRIDGE source address and length -- the same four words as
 *     0180, which works. There is no 48-bit offset packed into spare bits
 *     here; that is 0185, a separate command we do not issue.
 *   - The RTL is stock. core_bridge_cmd's 0184 arm is byte-identical in
 *     structure to its 0180 arm, latching target_20/24/28/2C at dequeue, and
 *     target_dataslot_id is 16 bits zero-extended into target_20.
 *
 * And one NEW measurement off the recorded sizes, which the earlier reasoning
 * missed by reading them as decimal:
 *
 *     21,037,825 = 0x01410301
 *     21,365,505 = 0x01460301
 *     20,382,465 = 0x01370301
 *
 * The LOW 16 BITS ARE IDENTICAL across three separate events, months of use
 * apart, and only the high half moves. A file length that ends in a constant
 * 0x0301 is not a length anything computed from our 32-byte record -- it is a
 * 32-bit word assembled from two 16-bit halves, one fixed and one varying.
 * Whatever is being written into the size field, it is a PACKED PAIR, and that
 * is a much narrower thing to look for than "the offset is wrong". Worth
 * recording the exact new sizes in hex when the probe runs: if the low half is
 * 0x0301 a fourth time, that constant is the thread to pull. */

#define SET_SLOT_ID  4u
#define SET_VERSION  1u
#define SET_BYTES    32u                /* fixed: the file must not change size */
#define SET_QUIET    (CLK_HZ)           /* settle before writing */

/* 0184 sources its bytes from a BRIDGE address, and the shell's bridge read mux
 * (core_top.v) serves 0xF8xxxxxx only -- the datatable. Everything else reads
 * back as zero, so pointing a write at SoC RAM wrote 32 zero bytes to the file
 * and the load then quietly fell back to defaults.
 *
 * Reads are unaffected: APF writing INTO RAM is the path data_loader already
 * provides, so settings_load() still lands in the tag buffer.
 *
 * Words 0..63 are the 0190 response, 64..127 the 0192 parameters; 128 is clear
 * of both. Byte offset 128*4 = 0x200, hence 0xF8002200. */
#define SET_DT_W     128u
#define SET_DT_ADDR  0xF8002200u

/* The record is handled as BYTES, packed into datatable words big-endian --
 * the convention the bridge's byte swap already imposes, and the one
 * slot_filename() proved. Reading the file back gives those same bytes, so the
 * two directions agree without either depending on word order. */
static const uint8_t set_magic[4] = { 'S', 'P', 'M', '3' };

static uint32_t set_dirty_at;           /* 0 = nothing pending */
static uint8_t  set_present;            /* a settings file was found at boot */

/* Byte offsets. Append only, and bump SET_VERSION -- an older file is ignored
 * rather than misread. */
enum {
    SET_B_MAGIC = 0,       /* 4 bytes */
    SET_B_VERSION = 4, SET_B_VOLUME = 5, SET_B_PALETTE = 6,
    SET_B_REPEAT = 7,  SET_B_SHUFFLE = 8, SET_B_ART = 9,
    SET_B_VIZ = 10
};

static void settings_mark_dirty(void) { set_dirty_at = cycles() | 1u; }

static int settings_load(void)
{
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)(UNCACHED + TAG_OFF);
    for (uint32_t i = 0; i < SET_BYTES; i++) b[i] = 0u;

    if (!target_read_slot(SET_SLOT_ID, 0, TAG_OFF, SET_BYTES)) return 0;
    for (uint32_t i = 0; i < 4u; i++)
        if (b[SET_B_MAGIC + i] != set_magic[i]) return 0;   /* not ours */
    if (b[SET_B_VERSION] != SET_VERSION) return 0;          /* older layout */

    /* Range-check every field. A corrupt or half-written file must not be able
     * to leave the player silent, or index off the end of the palette. */
    uint32_t v = b[SET_B_VOLUME];
    if (v <= VOL_MAX) { volume = v; vol_apply(); }

    v = b[SET_B_PALETTE];
    if (v < UI_PALETTE_N) { ui_pal_idx = v; ui_accent = ui_palette[v]; }

    v = b[SET_B_REPEAT];
    if (v <= REP_ONE) rep_mode = (uint8_t)v;

    shuffle_on = b[SET_B_SHUFFLE] ? 1u : 0u;
    art_pref   = b[SET_B_ART] ? 1u : 0u;

    v = b[SET_B_VIZ];
    if (v < VIZ_COUNT) viz_mode = (uint8_t)v;
    set_present = 1;
    return 1;
}

/* The write itself, split out from settings_store() so the probe can issue one
 * without SETTINGS_WRITE turning the debounced pump back on. Compiled only when
 * something actually calls it -- a released core contains no 0184 at all. */
#if SETTINGS_WRITE || SETTINGS_PROBE
static int settings_write_now(void)
{
    uint8_t rec[SET_BYTES];
    for (uint32_t i = 0; i < SET_BYTES; i++) rec[i] = 0u;

    for (uint32_t i = 0; i < 4u; i++) rec[SET_B_MAGIC + i] = set_magic[i];
    rec[SET_B_VERSION] = SET_VERSION;
    rec[SET_B_VOLUME]  = (uint8_t)volume;
    rec[SET_B_PALETTE] = (uint8_t)ui_pal_idx;
    rec[SET_B_REPEAT]  = rep_mode;
    rec[SET_B_SHUFFLE] = shuffle_on;
    rec[SET_B_ART]     = art_pref ? 1u : 0u;
    rec[SET_B_VIZ]     = viz_mode;

    for (uint32_t w = 0; w < SET_BYTES / 4u; w++)
        dt_write(SET_DT_W + w, ((uint32_t)rec[w*4+0] << 24) |
                               ((uint32_t)rec[w*4+1] << 16) |
                               ((uint32_t)rec[w*4+2] << 8)  |
                                (uint32_t)rec[w*4+3]);

    refill_drain();                     /* one command in flight, ever */
    uint32_t seq0 = (REG(R_TGT_GO) >> 8) & 0xFFu;
    REG(R_TGT_ID)  = SET_SLOT_ID;
    REG(R_TGT_OFF) = 0;
    REG(R_TGT_ADR) = SET_DT_ADDR;       /* the datatable, not RAM */
    REG(R_TGT_LEN) = SET_BYTES;
    REG(R_TGT_GO)  = TGT_WRITE;

    /* Returns the 0184 RESULT CODE, or -1 if it never answered. The probe wants
     * to tell "APF refused" from "APF said it worked" apart -- if files grow
     * after a write APF reported as successful, the fault is squarely inside
     * APF and no parameter we pass can fix it. */
    uint32_t deadline = cycles() + CLK_HZ * 2u;
    for (;;) {
        uint32_t st = REG(R_TGT_GO);
        if (((st >> 8) & 0xFFu) != seq0) return (int)((st >> 2) & 7u);
        if ((int32_t)(cycles() - deadline) >= 0) return -1;
    }
}
#endif

/* Writes the whole record every time. It is 32 bytes -- one command -- so
 * tracking which field changed would cost more than it saves. */
static int settings_store(void)
{
#if SETTINGS_WRITE
    return settings_write_now() == 0;
#else
    return 1;                       /* report success; simply never write */
#endif
}

#if !SETTINGS_PROBE
static void settings_probe_pump(void) { set_probe_req = 0; }
#else
/* ONE write per boot, on an explicit press, and then never again.
 *
 * The point is that the session contains exactly one 0184, so a before/after
 * comparison of the card is unambiguous. Normal operation cannot give that:
 * settings_pump() fires on any change, and after the fact there is no way to
 * know how many writes a session made -- which is why three events produced
 * three sizes and no mechanism.
 *
 * Latched rather than debounced. A second press must NOT write again, however
 * hard the button is leaned on, or the measurement is spoiled with nothing on
 * screen to say it happened -- so the screen reports the attempt NUMBER too,
 * and any number above 1 invalidates the run.
 *
 * The result is painted STATICALLY, not toasted. The probe is meant to be run
 * on an idle core with nothing loaded, and the main loop does
 * `if (idle) continue;` before it reaches ui_draw_dynamic(), so a toast would
 * never appear -- the same trap that made a failed playlist look like a hang. */
static uint8_t  set_probe_done;
static uint16_t set_probe_presses;
static int      set_probe_result = -2;  /* -2 not run, -1 no answer, else code */

static void settings_probe_pump(void)
{
    if (!set_probe_req) return;
    set_probe_req = 0;
    set_probe_presses++;

    if (!set_probe_done) {
        set_probe_done = 1;
        /* Nothing else may touch the card around it: drain any refill in
         * flight, and do not let the debounced pump slip a second write in
         * behind this one. */
        refill_drain();
        set_dirty_at = 0;
        set_flush_now = 0;
        set_probe_result = settings_write_now();
    }

    fb_rect(0, 0, FB_W, FB_H, UI_BG);
    fb_set_color(ui_accent, UI_BG);
    fb_text_clipped(UI_MARGIN, UI_TITLE_Y, "0184 PROBE", TS_2X, TS_2X, UI_INNER_W);

    uint32_t y = UI_TITLE_Y + FB_CELL(TS_2X) + 14u;
    fb_set_color(UI_WHITE, UI_BG);
    fb_text_clipped(UI_MARGIN, y,
                    set_probe_result == -1 ? "no answer from APF"
                  : set_probe_result ==  0 ? "APF reported SUCCESS"
                                           : "APF reported an ERROR",
                    TS_1X, TS_1X, UI_INNER_W);
    y += FB_CELL(TS_1X) + 6u;

    char b[24], *p = b;
    const char *t = "code ";
    while (*t) *p++ = *t++;
    p = ui_dec(p, (uint32_t)(set_probe_result < 0 ? 99 : set_probe_result));
    t = "   writes ";
    while (*t) *p++ = *t++;
    p = ui_dec(p, set_probe_presses);
    *p = 0;
    fb_set_color(UI_DIM, UI_BG);
    fb_text_clipped(UI_MARGIN, y, b, TS_1X, TS_1X, UI_INNER_W);
    y += FB_CELL(TS_1X) + 6u;

    fb_set_color(set_probe_presses > 1u ? UI_RED : UI_DIM, UI_BG);
    fb_text_clipped(UI_MARGIN, y,
                    set_probe_presses > 1u ? "PRESSED TWICE - RUN VOID"
                                           : "now re-read the card",
                    TS_1X, TS_1X, UI_INNER_W);
}
#endif

/* Called from the main loop. Does nothing until the settings have been stable
 * for SET_QUIET, so a held volume button costs one write rather than a dozen.
 * Deliberately not called while a track is loading -- see the call site. */
/* Writes are DEFERRED until playback is not running.
 *
 * The tic on every settings change is not the length of the write. Touching a
 * different data slot makes APF drop its fragment cache for the MP3 slot, so
 * the NEXT refill has to re-walk the FAT cluster chain -- the cost lands after
 * the write has finished, which is why banking audio in the FIFO first did not
 * help. It is the same behaviour target_flush_slot_cache() exists to exploit,
 * and the reason that function is marked load-time-only.
 *
 * So the write waits for a moment when there is no stream to disturb: paused,
 * stopped, or the OS menu opening -- which is also the moment before the core
 * is left, so a change made while playing is still saved on the way out.
 */

static void settings_pump(void)
{
    if (!set_dirty_at) return;
    if (!set_flush_now &&
        (int32_t)(cycles() - (set_dirty_at + SET_QUIET)) < 0) return;

    /* Nothing to disturb only when nothing is playing. Staying dirty is safe:
     * the flag persists until it is written, so the change is not lost -- it
     * just lands at the next pause, stop, or trip to the menu. */
    if (!set_flush_now && !paused) return;

    set_flush_now = 0;
    set_dirty_at = 0;

    /* A failure is reported ONCE. APF refusing to write is a real possibility
     * (the slot has to exist and be writable), and silently dropping settings
     * every couple of seconds would be worse than saying so. */
    static uint8_t complained;
    if (!settings_store() && !complained) {
        complained = 1;
        ui_toast_msg("SETTINGS NOT SAVED");
    }
}
