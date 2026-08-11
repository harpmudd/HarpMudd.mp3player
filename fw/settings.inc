/* ================================================================== SETTINGS
 * Volume, accent colour, repeat, shuffle, album art, meter and EQ preset,
 * remembered between sessions.
 *
 * The Pocket stores them, not this core. interact.json declares one `persist`
 * variable per setting at a word of a register file at 0x20000000; APF reads
 * those words back from the core every frame, lets the user adjust them in
 * Core Settings, writes them back, and saves them itself. Nothing here touches
 * the SD card.
 *
 * ---------------------------------------------------------------------------
 * DO NOT REPLACE THIS WITH A FILE WRITE. Two earlier mechanisms did, and both
 * damaged the user's music:
 *
 *   0184 target write   -- destroyed the library three times. Its bridge
 *       address is parsed as a struct whose second word APF reads as a SIZE,
 *       applied to whatever file occupies the MP3 slot.
 *   nonvolatile slot    -- hung the Pocket, at two different addresses.
 *       Isolated to the flag itself: the identical slot without it loaded fine.
 *
 * The full forensics, including the root cause -- this core was writing its
 * 0190 response struct over APF's dataslot ID/size table at word 0 -- are in
 * ROADMAP.md under Fixed. Read that before touching persistence again.
 * ---------------------------------------------------------------------------
 */

/* Set when something changes, cleared when it has been published. `set_poll_at`
 * throttles the check for changes made in the Core Settings menu -- APF
 * rewrites these words every frame and there is no reason to compare all seven
 * at the main loop's rate. */
static uint32_t set_dirty_at;
static uint32_t set_poll_at;

static void settings_mark_dirty(void) { set_dirty_at = cycles() | 1u; }


/* ---------------------------------------------------------------------------
 * PERSISTENCE, third mechanism and the first that cannot touch a data slot.
 *
 * interact.json declares one `persist` variable per setting, each at a word of
 * the register file at 0x20000000. Per Analogue: "Each frame, all of the UI
 * elements are read back from the core at a specified 32-bit BRIDGE address,
 * updated with new user-adjusted values, and finally written back", and
 * `persist` "retains the value set by the user after the core is shut down",
 * saved to /Settings/<core>/Interact/interact_persist.json.
 *
 * So APF does the storing, into its OWN file. Nothing here writes the card, and
 * no data slot is involved at any point. That is the entire reason for this
 * route: 0184 and the nonvolatile slot both reached the user's .mp3 files, three
 * libraries between them. This mechanism has no path to them.
 *
 * Because APF reads back from the core rather than only writing to it, a change
 * made with the BUTTONS is picked up and persisted too -- it is not merely a
 * menu of defaults.
 *
 * Direction is resolved by comparing against what we last published: if the
 * word APF hands back differs from that, the user moved it in the Core Settings
 * menu and we adopt it. Otherwise ours stands.
 *
 * ---------------------------------------------------------------------------
 * RULES FOR CHANGING interact.json IN A RELEASE. Read these before editing it.
 *
 * The persist file keys on `id`. Nothing else -- not the name, not the order,
 * not the address. A released core's saved settings survive an update only for
 * as long as those numbers hold still, and there is no versioning or migration
 * in APF to fall back on. Stable ids ARE the mechanism.
 *
 *   1. NEVER renumber an existing variable. Every updating user's stored value
 *      would land on whichever setting inherited its id -- their volume
 *      arriving as an accent colour, silently, on first launch.
 *
 *   2. RETIRING a setting: delete it and leave the id dead. The orphaned entry
 *      in the user's file is ignored. Do not close the gap.
 *
 *   3. ADDING one: take the next unused id, even when reusing a retired
 *      variable's ADDRESS. That is why resume took ids 18 and 19 while sitting
 *      at the addresses album art and screen blank had vacated -- reusing 14
 *      and 17 would have applied a stored "album art = 1" to Resume point, and
 *      every updating user would have resumed at a garbage position exactly
 *      once, on the launch right after updating.
 *
 *   4. Changing a `type` CANNOT be made safe. The list experiment wrote its
 *      type into the persist file and reverting interact.json was not enough
 *      to recover -- the file had to be deleted. Treat any type change as a
 *      settings reset and say so in the release notes.
 *
 *   5. Keep the ADDRESS stable too, separately from the id: it selects the
 *      register-file slot via bridge_addr[4:2], so moving it changes which
 *      firmware variable the word feeds.
 *
 * The persist file is plain JSON on the card at
 * /Settings/<core>/Interact/_core/interact_persist.json. It is readable, it is
 * authoritative, and it answers persistence questions with no hardware round
 * trip -- reading it is what found the signed-int bug that made resume store
 * -1. Check it FIRST.
 * ---------------------------------------------------------------------------
 */
/* Slot 4 was the album-art panel and slot 7 the screen-blank timeout. Both are
 * now button-only and unpersisted, freeing the two slots resume needs -- the
 * index is 3 bits wide (`set_idx <= dDAT_MOSI[2:0]`), so eight is all there is
 * and a ninth would silently alias onto Volume.
 *
 * The ADDRESSES of the six survivors are unchanged, deliberately: renumbering
 * would have scrambled every already-saved setting on every card. */
enum { SW_VOL = 0, SW_PAL, SW_REP, SW_SHUF, SW_RESUME, SW_VIZ, SW_EQ,
       SW_RESUMEON, SW_N };

/* What we last published. Initialised to a value APF can never write for
 * these ranges, so the FIRST poll always counts as a change and adopts
 * whatever APF has -- we must not publish before adopting, or our defaults
 * would overwrite the persisted values on the way past. */
static uint32_t set_pub[SW_N] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                  0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                  0xFFFFFFFFu, 0xFFFFFFFFu };

static uint32_t set_rd(uint32_t w) { REG(R_SET_IDX) = w; return REG(R_SET_DAT); }
static void     set_wr(uint32_t w, uint32_t v)
{
    REG(R_SET_IDX) = w;
    REG(R_SET_DAT) = v;
    set_pub[w] = v;
}

/* Publish every current value. Called at boot after adopting, and whenever a
 * setting changes. Two MMIO writes per word -- nothing to debounce. */
static int settings_store(void)
{
    set_wr(SW_VOL,  volume);
    set_wr(SW_PAL,  ui_pal_idx);
    set_wr(SW_REP,  rep_mode);
    set_wr(SW_SHUF, shuffle_on ? 1u : 0u);
    set_wr(SW_RESUME, resume_word);
    set_wr(SW_VIZ,  viz_mode);
    set_wr(SW_EQ,   eq_idx);
    set_wr(SW_RESUMEON, resume_on ? 1u : 0u);
    return 1;
}

/* Take whatever APF has for us -- the persisted values at boot, or a menu
 * change at runtime. Every field is range-checked: a value from a file on the
 * card must not be able to silence the player or index off a palette. */
static int settings_load(void)
{
    uint32_t v;

    /* ALL ZERO means APF has not written yet -- the words power up at zero and
     * the core can reach this before APF's first pass. Adopting that would set
     * the volume to 0 and hand the user a silent player that looks broken.
     * An all-default state is indistinguishable from an unwritten one, so the
     * firmware defaults stand and the next poll picks up the real values. */
    uint32_t any = 0;
    for (uint32_t w = 0; w < SW_N; w++) any |= set_rd(w);
    if (!any) return 0;

    v = set_rd(SW_VOL);   if (v <= VOL_MAX)      { volume = v; vol_apply(); }
    v = set_rd(SW_PAL);   if (v < UI_PALETTE_N)  { ui_pal_idx = v;
                                                   ui_accent = ui_palette[v]; }
    v = set_rd(SW_REP);   if (v <= REP_ONE)        rep_mode = (uint8_t)v;
    v = set_rd(SW_SHUF);  shuffle_on = v ? 1u : 0u;
    /* Not range-checked as a whole -- it is three packed fields and each is
     * validated where it is used, against the playlist length and the file
     * actually open. A garbage word can only fail to resume. */
    resume_word = set_rd(SW_RESUME);
    v = set_rd(SW_VIZ);   if (v < VIZ_COUNT)       viz_mode = (uint8_t)v;
    v = set_rd(SW_EQ);    if (v < EQ_COUNT)      { eq_idx = (uint8_t)v;
                                                   REG(R_EQ) = eq_idx; }
    v = set_rd(SW_RESUMEON); resume_on = v ? 1u : 0u;
    settings_adopted = 1u;
    return 1;
}

/* Watch for changes made in the Core Settings menu while running. APF rewrites
 * every word each frame, so a word differing from what we published is the
 * user having moved it. */
static int settings_poll_menu(void)
{
    int changed = 0;
    for (uint32_t w = 0; w < SW_N; w++)
        if (set_rd(w) != set_pub[w]) { changed = 1; break; }
    if (!changed) return 0;
    settings_load();
    settings_store();          /* re-publish so pub[] tracks again */
    return 1;
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
    /* A change of ours: publish it. Immediate -- the quiet window and the
     * wait-for-paused both existed to keep an SD round trip away from the
     * decoder, and there is no SD access here at all now, just MMIO writes. */
    if (set_dirty_at) {
        set_flush_now = 0;
        set_dirty_at  = 0;
        settings_store();
        return;
    }

    /* Otherwise watch for a change made in the Core Settings menu. Throttled:
     * APF rewrites these words every frame and there is no reason to compare
     * seven of them at the main loop's rate. */
    if ((int32_t)(cycles() - set_poll_at) < 0) return;
    set_poll_at = cycles() + CLK_HZ / 10u;
    if (settings_poll_menu()) {
        ui_accent_changed = 1u;
        ui_mode_dirty     = 1u;
        ui_wave_force     = 1u;
    }
}
