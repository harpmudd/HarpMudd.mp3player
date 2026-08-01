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

#define SET_SLOT_ID  4u
#define SET_MAGIC    0x334D5053u        /* "SPM3" */
#define SET_VERSION  1u
#define SET_BYTES    32u                /* fixed: the file must not change size */
#define SET_QUIET    (CLK_HZ * 2u)      /* settle before writing */

static uint32_t set_dirty_at;           /* 0 = nothing pending */
static uint8_t  set_present;            /* a settings file was found at boot */

/* Field order is part of the file format. Append only, and bump SET_VERSION --
 * an older file is ignored rather than misread. */
enum {
    SET_W_MAGIC = 0, SET_W_VERSION, SET_W_VOLUME, SET_W_PALETTE,
    SET_W_REPEAT, SET_W_SHUFFLE, SET_W_ART, SET_W_COUNT
};

static void settings_mark_dirty(void) { set_dirty_at = cycles() | 1u; }

static int settings_load(void)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + TAG_OFF);
    for (uint32_t i = 0; i < SET_BYTES / 4u; i++) w[i] = 0u;

    if (!target_read_slot(SET_SLOT_ID, 0, TAG_OFF, SET_BYTES)) return 0;
    if (w[SET_W_MAGIC] != SET_MAGIC) return 0;      /* not ours, or never written */
    if (w[SET_W_VERSION] != SET_VERSION) return 0;  /* older layout: use defaults */

    /* Range-check every field. A corrupt or half-written file must not be able
     * to leave the player silent, or indexing off the end of the palette. */
    uint32_t v = w[SET_W_VOLUME];
    if (v <= VOL_MAX) { volume = v; vol_apply(); }

    v = w[SET_W_PALETTE];
    if (v < UI_PALETTE_N) { ui_pal_idx = v; ui_accent = ui_palette[v]; }

    v = w[SET_W_REPEAT];
    if (v <= REP_ONE) rep_mode = (uint8_t)v;

    shuffle_on = w[SET_W_SHUFFLE] ? 1u : 0u;
    set_present = 1;
    return 1;
}

/* Writes the whole record every time. It is 32 bytes -- one command -- so
 * tracking which field changed would cost more than it saves. */
static int settings_store(void)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + TAG_OFF);
    for (uint32_t i = 0; i < SET_BYTES / 4u; i++) w[i] = 0u;

    w[SET_W_MAGIC]   = SET_MAGIC;
    w[SET_W_VERSION] = SET_VERSION;
    w[SET_W_VOLUME]  = volume;
    w[SET_W_PALETTE] = ui_pal_idx;
    w[SET_W_REPEAT]  = rep_mode;
    w[SET_W_SHUFFLE] = shuffle_on;
    w[SET_W_ART]     = art_shown ? 1u : 0u;

    refill_drain();                     /* one command in flight, ever */
    uint32_t seq0 = (REG(R_TGT_GO) >> 8) & 0xFFu;
    REG(R_TGT_ID)  = SET_SLOT_ID;
    REG(R_TGT_OFF) = 0;
    REG(R_TGT_ADR) = TAG_OFF;
    REG(R_TGT_LEN) = SET_BYTES;
    REG(R_TGT_GO)  = TGT_WRITE;

    uint32_t deadline = cycles() + CLK_HZ * 2u;
    for (;;) {
        uint32_t st = REG(R_TGT_GO);
        if (((st >> 8) & 0xFFu) != seq0) return (((st >> 2) & 7u) == 0u);
        if ((int32_t)(cycles() - deadline) >= 0) return 0;
    }
}

/* Called from the main loop. Does nothing until the settings have been stable
 * for SET_QUIET, so a held volume button costs one write rather than a dozen.
 * Deliberately not called while a track is loading -- see the call site. */
static void settings_pump(void)
{
    if (!set_dirty_at) return;
    if ((int32_t)(cycles() - (set_dirty_at + SET_QUIET)) < 0) return;
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
