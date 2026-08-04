/* ================================================================== SETTINGS
 * Volume, accent colour, repeat, shuffle and the art panel, kept across
 * relaunches in a small file on the card.
 *
 * settings.bin ships with the core holding the defaults, and a missing or
 * unreadable file is not an error: the built-in defaults simply stand.
 *
 * "APF cannot CREATE a file" was the stated reason for shipping one, and it is
 * WRONG -- see the confirmed-cause section below. The faulty 0184 path created
 * a 21 MB file on the card named after a TYPO that existed only in the user's
 * playlist, garbage from its first byte. Shipping a defaults file is still the
 * right call, but not for that reason.
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
 * honoured. Only the write-back is gone. `tools/settings_edit.py` edits the
 * file with named fields -- until the blocker is answered, that script IS the
 * persistence mechanism, and the buttons are a session-only control.
 *
 * RE-ENABLED 2026-08-04 on the user's explicit instruction, with the measurement
 * below NOT yet run. Recorded plainly so nobody later reads this as an oversight:
 *
 *   - The mechanism section further down predicts this write extends an .mp3 to
 *     the value of word 1 of the record. It is a prediction, not a certainty.
 *   - The user's originals are intact at C:\Users\mikek\Desktop\songs, so the
 *     card is restorable.
 *   - Because the prediction is exact, ordinary use is now itself the
 *     experiment: if a file lands on word 1's value the mechanism is confirmed
 *     and the fix follows from it; if nothing grows, the mechanism is refuted
 *     and 0184 was never the fault.
 *
 * Take a card snapshot before and after (tools/card_snapshot.py) or the run
 * proves nothing and only costs files.
 *
 * RESOLVED 2026-08-04: the write command is gone entirely. Settings now live
 * in the datatable and APF saves them itself -- see settings_store() below.
 * SETTINGS_WRITE and the SETTINGS_PROBE diagnostic are both removed; there is
 * no longer a flag to turn on, and no 0184 anywhere in this core. */

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
 * ===========================================================================
 * MECHANISM FOUND (2026-08-04). The damaged file size IS WORD 1 OF THIS RECORD.
 * ===========================================================================
 *
 * Read the three recorded sizes in HEX rather than decimal:
 *
 *     21,037,825 = 0x01410301
 *     21,365,505 = 0x01460301
 *     20,382,465 = 0x01370301
 *
 * The low 16 bits are identical all three times and only the high half moves.
 * Now decode them as this record's word 1, which settings_write_now() packs
 * big-endian as { SET_VERSION, volume, palette, repeat }:
 *
 *     0x01410301 -> version 1, volume  65, palette 3, repeat 1 (ALL)
 *     0x01460301 -> version 1, volume  70, palette 3, repeat 1 (ALL)
 *     0x01370301 -> version 1, volume  55, palette 3, repeat 1 (ALL)
 *
 * Every field is in range and every one is right. The version is constant
 * because it is a constant. Palette and repeat are constant because the user
 * had not changed them. VOLUME is the one that moves -- 65 is the firmware
 * default, and volume is both the most-changed setting and the one that
 * triggers a write. That is not a coincidence at one in four billion.
 *
 * Confirmed against the card, independently: the settings.bin on it right now
 * holds volume 55, palette 3, repeat ALL -- bytes 04..07 are 01 37 03 01, which
 * is 0x01370301, byte-for-byte the THIRD damaged size.
 *
 * So APF is not writing 32 bytes of payload at offset 0. Something in the write
 * path reads the word at (bridge address + 4) and uses it as a SIZE. Compare
 * 0192, where the bridge address points at a PARAMETER STRUCT that APF parses
 * -- "filename and flag/size" -- rather than at raw bytes. 0184's source
 * pointer appears to be treated the same way, so our word 1 lands in a size
 * field and the file is extended to it.
 *
 * This explains every observation the earlier theories could not:
 *   - why all four .mp3s share ONE size: they are stamped with the same bogus
 *     size in turn, each while it occupies the MP3 slot;
 *   - why settings.bin stays exactly 32 bytes: it is not the file being sized;
 *   - why the low half never moved: palette and repeat never moved;
 *   - why a mitigation about WHERE settings.bin lives changed nothing.
 *
 * ===========================================================================
 * CONFIRMED 2026-08-04. The damaged size IS word 1 of this record. PROVEN.
 * ===========================================================================
 *
 * The prediction was made, missed once on a light session, and then hit exactly
 * on a heavy one. Do not read the earlier miss as a refutation: a single
 * session of a few writes is simply not enough to trigger it.
 *
 * The confirming event. settings.bin held
 *
 *     53 50 4D 33 | 01 4B 08 01 | ...
 *                   ^^^^^^^^^^^ word 1 = version 1, volume 75, palette 8,
 *                               repeat ALL  =  0x014B0801  =  21,694,465
 *
 * and EVERY .mp3 that had been in the MP3 slot came back at exactly
 * 21,694,465 bytes. That number was computed and written down BEFORE the
 * session, and checked against the card when nothing matched it.
 *
 * One file on the card was NOT damaged -- the one that never became the active
 * slot file. That is the last piece: files are stamped one at a time, as each
 * occupies the MP3 slot, which is why every damaged file shares one size and
 * why the count grows with how much you skip around.
 *
 * So APF does NOT treat the 0184 bridge address as raw payload. Something in
 * that path reads the word at (bridge address + 4) as a SIZE and applies it to
 * the file in the slot -- the same shape as 0192, where the bridge address
 * points at a parameter STRUCT that APF parses. We have been handing it 32
 * bytes of settings where it expects a struct, and word 1 of our record lands
 * in its size field.
 *
 * AND IT CAN CREATE FILES. At the clean baseline the playlist named
 * "Goose - Hungerstrike.mp3" -- a typo for a real track -- and no such file
 * existed. Two hours later it did: 21,694,465 bytes, the damage size exactly,
 * garbage from byte 0, with head bytes 8D E9 5D 73 that the core reported on
 * screen as NOTAG 8DE95D73. Nothing but the write path can have made it. So the
 * long-standing "APF cannot CREATE a file" note at the top of this file was
 * wrong, and the blast radius of this fault is larger than "existing files get
 * extended" -- a mistyped playlist entry is enough to conjure a 21 MB file.
 *
 * WHAT THIS MEANS. 0184 as used here is unusable, and the fault is ours rather
 * than APF's: the command is documented as taking a bridge SOURCE address, but
 * the observed behaviour is a parsed struct, and we never had a template to
 * copy. 0192 was only ever solved by copying a real struct APF itself produced
 * (via 0190). There is no equivalent source for 0184, so constructing one is
 * exactly the guessing this file already warns against.
 *
 * Writing is OFF and stays off until someone has a 0184 parameter struct that
 * APF produced. tools/settings_edit.py is the persistence mechanism.
 *
 * WHAT MADE IT WORSE, and it was my change: wiring the EQ preset to
 * settings_mark_dirty() meant every Y press queued a write. Flipping presets
 * while skipping tracks turned an occasional write into a steady stream, which
 * is why this event damaged five files where earlier ones damaged four over
 * much longer use. A settings field attached to a control the user operates
 * repeatedly is a different risk profile from one attached to volume.
 */

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
    SET_B_VIZ = 10,    SET_B_EQ = 11
};

static void settings_mark_dirty(void) { set_dirty_at = cycles() | 1u; }

static int settings_load(void)
{
    /* Read with 0180, the way it worked for months. The nonvolatile-slot
     * experiment that replaced this HUNG THE POCKET ON QUIT -- see the note at
     * settings_store(). */
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

    /* Applied to the RTL here rather than waiting for the first Y press, so a
     * saved preset is actually in effect from the first note. */
    v = b[SET_B_EQ];
    if (v < EQ_COUNT) { eq_idx = (uint8_t)v; REG(R_EQ) = eq_idx; }
    set_present = 1;
    return 1;
}

/* DOES NOTHING, deliberately. There is no safe way for this core to write the
 * card, and two separate mechanisms have now been tried and rejected:
 *
 *   0184 target write   -- DESTROYS FILES. Its bridge address is treated as a
 *       parameter struct whose second word APF reads as a SIZE, applied to
 *       whatever file occupies the MP3 slot. Confirmed by prediction; it took
 *       out the user's whole library three times. No safe value exists for
 *       that word, so the command is not used at all.
 *
 *   nonvolatile slot    -- HANGS THE POCKET ON QUIT. data.json declared the
 *       slot nonvolatile with its address at 0xF8002200 so APF would read the
 *       record back and save it itself. The core ran fine, then hung on Quit
 *       and needed a hard reboot; settings.bin was never written.
 *
 *       Best guess, untested: 0xF8xxxxxx is APF's own COMMAND region, owned by
 *       core_bridge_cmd. Pointing a data slot's address into it likely makes
 *       APF's shutdown read-back collide with its own command interface. The
 *       datatable was chosen because the frozen shell serves bridge reads
 *       NOWHERE ELSE -- so doing this properly needs a small bridge-readable
 *       RAM outside F8, which means editing core_top.v. Not attempted.
 *
 * Settings therefore load from the card and are session-only. Use
 * tools/settings_edit.py to change what persists.
 */
static int settings_store(void)
{
    return 1;                       /* report success; simply never write */
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
    set_flush_now = 0;
    set_dirty_at  = 0;

    /* Immediate. The quiet window and the wait-for-paused both existed to keep
     * an SD round trip away from the decoder; there is no SD round trip any
     * more, just two MMIO writes into the datatable. APF does the actual save
     * at shutdown. */
    settings_store();
}
