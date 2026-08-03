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
 * Three facts then line up on 0184:
 *
 *   - All four files ended at the SAME size despite being different lengths,
 *     which is what happens when a write targets one fixed offset in each: the
 *     resulting size is offset + length, independent of what was there before.
 *   - That common size minus SET_BYTES (32) is a whole number, 20,382,433. The
 *     settings blob is 32 bytes. The arithmetic is ours.
 *   - The settings magic ("SPM3", 0x53504D33) is present INSIDE one of the .mp3
 *     files, at offset 5,505,024 -- a 128 KB cluster boundary immediately past
 *     that file's real audio, in the region the extension added.
 *
 * The gap between each file's real end and its new end is filled with unrelated
 * data, because exFAT does not zero newly allocated clusters. That is why one
 * file's audio also stopped decoding: the damage is not confined to the tail.
 *
 * So the write is landing in the wrong file, at an offset that is not ours to
 * choose, and extending it. Whether the fault is our slot handling or APF's
 * resolution of a slot opened by 0192 is not yet established -- but it is our
 * 32 bytes doing it, and that is enough to stop.
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

/* Writes the whole record every time. It is 32 bytes -- one command -- so
 * tracking which field changed would cost more than it saves. */
static int settings_store(void)
{
#if !SETTINGS_WRITE
    return 1;                       /* report success; simply never write */
#else
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

    uint32_t deadline = cycles() + CLK_HZ * 2u;
    for (;;) {
        uint32_t st = REG(R_TGT_GO);
        if (((st >> 8) & 0xFFu) != seq0) return (((st >> 2) & 7u) == 0u);
        if ((int32_t)(cycles() - deadline) >= 0) return 0;
    }
#endif
}

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
