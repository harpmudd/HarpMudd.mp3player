/* ================================================================== PLAYLIST
 * Reads an .m3u from its own data slot, then switches the MP3 slot between the
 * files it names using APF's 0192 "open file into data slot" command.
 *
 * The whole feature is FIRMWARE-ONLY. 0192 was already a valid selector in
 * tgt_cmd.v and the datatable already had a CPU write port -- both were built
 * for the reload work and left unused, so nothing in the RTL changes and the
 * bitstream is untouched.
 *
 * There is no directory-listing command in APF. A core fundamentally cannot
 * enumerate a folder, which is why the track names have to come from a file the
 * user writes rather than from scanning the card.
 */

#define PL_SLOT_ID   3u

/* ---------------------------------------------------------------------------
 * The 0192 parameter struct, WITHOUT guessing its layout.
 *
 * Analogue documents 0192 as taking "filename and flag/size" in the parameter
 * struct, but not the field order, widths or padding -- and guessing a struct
 * layout is exactly the class of mistake that cost several hardware rounds on
 * the reload bug. So this never constructs one.
 *
 * Instead: 0190 (getfile) asks APF to write the CURRENT file's descriptor into
 * the response struct. That is a real, correctly-formed struct in whatever
 * layout this firmware version uses. Copy it verbatim into the parameter
 * struct, overwrite just the filename, and hand it back. Every field that is
 * not the name keeps the value APF itself produced.
 *
 * The name is located the same way slot_filename() locates it -- the longest
 * run of printable ASCII -- so even the name's OFFSET is observed rather than
 * assumed.
 */
/* Word addresses inside mf_datatable, which is 256 WORDS deep. These MUST
 * match target_buffer_resp_struct / target_buffer_param_struct in
 * core_game.vh, and must not overlap: the parameter struct is built from the
 * response struct, so an overlap is a read-after-write on itself. The param
 * struct was originally at word 256 -- past the end of a 256-word RAM, so the
 * address wrapped and landed straight back on the response struct. */
/* MUST stay above APF's dataslot ID/size table, which occupies the START of
 * this BRAM as {slot_id, size} pairs -- measured, see core_game.vh. The
 * response struct used to sit at word 0 and APF overwrote the table with it on
 * every 0190. */
#define DT_RESP_W   64u     /* response struct  (APF writes) */
#define DT_PARAM_W  128u    /* parameter struct (APF reads)  */
#define DT_WORDS    64u     /* 256 bytes, matching slot_filename()'s window */

static inline void dt_write(uint32_t word, uint32_t val)
{
    REG(R_DT_ADDR) = word;
    REG(R_DT_DATA) = val;
}

/* Set when the last pl_cmd() gave up on the deadline rather than being answered.
 *
 * Both outcomes return 0, but they cost wildly different amounts of time and
 * mean different things. An answered command with a nonzero result is instant
 * and describes THIS file; a timeout burns the whole timeout and says the
 * command path itself is not responding, which the next file will not fix. The
 * skip loop in pl_play_dir() needs to tell them apart -- retrying 128 entries
 * that each take 3 s to not answer is six minutes of frozen core. */
static uint8_t pl_cmd_timedout;

/* Run one target command and wait for the sequence counter to move. Same
 * discipline as target_read_poll(): never trust the `done` LEVEL, which stays
 * asserted from the previous command until the next one is picked up. */
static int pl_cmd(uint32_t sel, uint32_t slot, uint32_t timeout_cycles)
{
    refill_drain();                       /* one command in flight, ever */
    pl_cmd_timedout = 0;
    uint32_t seq0 = (REG(R_TGT_GO) >> 8) & 0xFFu;
    REG(R_TGT_ID) = slot;
    REG(R_TGT_GO) = sel;

    uint32_t deadline = cycles() + timeout_cycles;
    for (;;) {
        uint32_t st = REG(R_TGT_GO);
        if (((st >> 8) & 0xFFu) != seq0) return (((st >> 2) & 7u) == 0u);
        if ((int32_t)(cycles() - deadline) >= 0) { pl_cmd_timedout = 1; return 0; }
    }
}

/* Ask APF to put `name` in the MP3 slot. Returns 0 if the command failed --
 * the caller stays on the current track rather than leaving the player in a
 * state where nothing is loaded. */
/* Why the last open failed, surfaced in the toast. Guessing between these is
 * exactly what cost several rounds on the reload bug -- each has a different
 * cause and a different fix, and they are indistinguishable from outside. */
enum { PLO_OK = 0, PLO_GETFILE, PLO_NONAME, PLO_TOOLONG, PLO_OPEN };
static uint8_t pl_open_err;
static uint8_t pl_open_errcode;      /* target_dataslot_err from 0192 */
static uint8_t pl_open_timeout;      /* 0192 never answered; errcode is stale  */

/* The result code 0192 returns for a file that is not on the card. Established
 * by observation, not from the docs: pointing the open at a bare filename that
 * did not exist at the card root is what produced it, repeatably, during the
 * playlist bring-up (see the path-handling note in pl_open_name). It is the one
 * code that justifies giving up on an entry for good. */
#define PL_ERRCODE_NOFILE 4u

static int pl_open_name(const char *name);   /* -> pl_open_into(MP3 slot) */

/* Fetch a template descriptor into `buf`. Returns the length of the printable
 * path it contains, or 0.
 *
 * Either slot will do. The MP3 slot is preferred because its path is the folder
 * the current track came from, but at launch it may hold nothing at all -- the
 * core no longer forces a file to be picked before it starts. The playlist slot
 * is filled at boot from playlist.m3u and sits in the same folder as the tracks
 * it names, so it is a perfectly good stand-in. */
static uint32_t pl_template(uint32_t slot, uint8_t *buf, uint32_t *name_at)
{
    if (!pl_cmd(TGT_GETFILE, slot, CLK_HZ)) return 0;

    for (uint32_t w = 0; w < DT_WORDS; w++) {
        uint32_t v = dt_read(DT_RESP_W + w);
        /* BIG-endian: the bridge byte-swaps. */
        buf[w*4 + 0] = (uint8_t)(v >> 24);
        buf[w*4 + 1] = (uint8_t)(v >> 16);
        buf[w*4 + 2] = (uint8_t)(v >> 8);
        buf[w*4 + 3] = (uint8_t)v;
    }

    uint32_t best = 0, best_len = 0, i = 0;
    while (i < DT_WORDS * 4u) {
        uint32_t start = i;
        while (i < DT_WORDS * 4u && buf[i] >= 0x20u && buf[i] < 0x7Fu) i++;
        if (i - start > best_len) { best_len = i - start; best = start; }
        i++;
    }
    *name_at = best;
    return best_len;
}

/* Open `name` into ANY slot. The target used to be hardwired to the MP3 slot;
 * slot 3 needs it too now that the playlist slot no longer declares a default
 * filename and has to be filled by the core on a fresh card. */
static int pl_open_into(uint32_t slot, const char *name)
{
    pl_open_err = PLO_OK; pl_open_errcode = 0; pl_open_timeout = 0;

    /* 1. Template: a struct APF built itself, for a file it already has. */
    uint8_t buf[DT_WORDS * 4u];
    uint32_t best = 0, best_len = pl_template(MP3_SLOT_ID, buf, &best);
    if (!best_len) best_len = pl_template(PL_SLOT_ID, buf, &best);
    if (!best_len) { pl_open_err = PLO_NONAME; return 0; }


    /* 3. If the word immediately before the name holds the OLD name's length,
     *    it is a length field and has to move with the name. Checked rather
     *    than assumed, and only accepted in that one adjacent position -- a
     *    scan of the whole struct for "any word equal to the length" would
     *    eventually match something unrelated and corrupt it. */
    /* The slot's current entry is a full PATH ("/Assets/mp3player/common/x.mp3"
     * -- the 0190 work saw exactly that), while a playlist line is normally a
     * bare filename. Replacing the whole path with a bare name asks APF to open
     * a file that does not exist at the root, which is what result code 4 was
     * reporting. So KEEP the directory the slot is already using and swap only
     * the last component. An entry that starts with '/' is taken as absolute
     * and replaces the path outright.
     *
     * The directory is read off the observed response rather than hardcoded --
     * a playlist can live anywhere, and this way it follows the slot. */
    uint32_t dir_len = 0;
    if (name[0] != '/') {
        for (uint32_t k = 0; k < best_len; k++)
            if (buf[best + k] == '/') dir_len = k + 1u;
    }

    uint32_t nm_len = 0;
    while (name[nm_len]) nm_len++;
    uint32_t new_len = dir_len + nm_len;
    if (new_len >= sizeof(buf) - best) { pl_open_err = PLO_TOOLONG; return 0; }

    uint32_t len_w = (best >= 4u) ? ((best - 4u) / 4u) : 0xFFFFFFFFu;
    int patch_len = 0;
    if (len_w != 0xFFFFFFFFu) {
        uint32_t v = ((uint32_t)buf[len_w*4+0] << 24) | ((uint32_t)buf[len_w*4+1] << 16) |
                     ((uint32_t)buf[len_w*4+2] << 8)  |  (uint32_t)buf[len_w*4+3];
        if (v == best_len) patch_len = 1;
    }

    /* 4. Overwrite the name, and clear the tail of the old one so a shorter
     *    name cannot leave the previous file's characters trailing behind it. */
    for (uint32_t k = dir_len; k < best_len || k < new_len; k++) {
        if (best + k >= sizeof(buf)) break;
        buf[best + k] = (k < new_len) ? (uint8_t)name[k - dir_len] : 0u;
    }
    if (best + new_len < sizeof(buf)) buf[best + new_len] = 0u;

    if (patch_len) {
        buf[len_w*4 + 0] = (uint8_t)(new_len >> 24);
        buf[len_w*4 + 1] = (uint8_t)(new_len >> 16);
        buf[len_w*4 + 2] = (uint8_t)(new_len >> 8);
        buf[len_w*4 + 3] = (uint8_t)new_len;
    }

    /* 5. Hand it back and fire 0192. */
    for (uint32_t w = 0; w < DT_WORDS; w++) {
        uint32_t v = ((uint32_t)buf[w*4+0] << 24) | ((uint32_t)buf[w*4+1] << 16) |
                     ((uint32_t)buf[w*4+2] << 8)  |  (uint32_t)buf[w*4+3];
        dt_write(DT_PARAM_W + w, v);
    }

    /* This switches the slot but raises NO 008A -- that notification is for a
     * file the USER picked from the menu, not one the core opened. The caller
     * therefore has to drive the load itself; see pl_play_at(). */
    if (!pl_cmd(TGT_OPENFILE, slot, CLK_HZ * 3u)) {
        pl_open_err     = PLO_OPEN;
        pl_open_timeout = pl_cmd_timedout;
        pl_open_errcode = (uint8_t)((REG(R_TGT_GO) >> 2) & 7u);
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------- bad entries
 *
 * What a failed open means for the REST of the list. Getting this wrong in
 * either direction loses music: treat a transient failure as permanent and a
 * track that would play is dropped for the session; treat a permanent one as
 * transient and every pass re-attempts a file that is not there.
 */
enum {
    PLF_DEAD = 0,       /* this entry is not on the card: never try it again  */
    PLF_SKIP,           /* this entry failed, but might not next time         */
    PLF_FATAL           /* not about this entry at all: stop trying entries   */
};

static uint8_t pl_classify_failure(void)
{
    switch (pl_open_err) {
    /* No template from EITHER slot. That is the 0190 path failing, which has
     * nothing to do with which file was asked for -- the next entry produces
     * exactly the same result, so walking the list is pure waste. */
    case PLO_GETFILE:
    case PLO_NONAME:
        return PLF_FATAL;

    /* The name cannot be expressed in the parameter struct at all. Its length
     * will not change, so this is as permanent as a missing file. */
    case PLO_TOOLONG:
        return PLF_DEAD;

    case PLO_OPEN:
        /* Answered, and answered "no such file". The only permanent verdict
         * APF actually gives us. */
        if (!pl_open_timeout && pl_open_errcode == PL_ERRCODE_NOFILE)
            return PLF_DEAD;
        /* Never answered. Cheap to misjudge one way, ruinous the other: each
         * timeout costs the full 3 s, so a list of 128 would freeze the core
         * for six minutes before reaching the idle screen. Stop. */
        if (pl_open_timeout)
            return PLF_FATAL;
        /* Answered with something else -- busy, or a state we have not seen.
         * Move past it for now, but do not condemn it. */
        return PLF_SKIP;
    }
    return PLF_SKIP;
}

/* Entries known to be missing, indexed by FILE index rather than play position:
 * shuffle permutes the order, and knowledge of what is on the card must survive
 * that. Cleared by pl_load(), so fixing the card and re-picking the playlist
 * gives every entry another chance. 128 bits total. */
static uint32_t pl_dead[(PL_MAX + 31u) / 32u];
static uint16_t pl_dead_n;

static int  pl_is_dead(uint16_t f) { return (pl_dead[f >> 5] >> (f & 31u)) & 1u; }
static void pl_mark_dead(uint16_t f)
{
    if (pl_is_dead(f)) return;
    pl_dead[f >> 5] |= 1u << (f & 31u);
    pl_dead_n++;
    /* The track count on screen is derived from this, so learning about an
     * entry changes what is displayed -- and that display is drawn only when
     * something is known to have changed. */
    ui_mode_dirty = 1u;
}

/* How many tracks the user actually HAS, as opposed to how many lines are in
 * the file. An entry that cannot be opened is not a song, so it must not be
 * counted as one -- a 5-line list with one typo is a 4-track playlist.
 *
 * This can only ever be right about entries something has TRIED. Nothing in APF
 * answers "does this file exist"; the only test is 0192, which opens it, and
 * that costs a directory walk per entry and switches the MP3 slot as a side
 * effect. So the count is exact for everything the walk has reached and
 * optimistic beyond it, correcting itself as entries are visited. */
static uint16_t pl_live_count(void)
{
    return (uint16_t)(pl_count - pl_dead_n);
}

/* Where `pos` sits among the LIVE entries, 1-based, so the position and the
 * total are counted the same way. Showing "4 / 4" while standing on the third
 * playable track would be worse than showing the raw numbers. */
/* Which entry of the PLAYLIST is playing -- "4" in "4 / 12".
 *
 * `pos` indexes pl_order[], the play sequence, and it is the FILE index behind
 * it that the number names. Counting along the sequence instead would just
 * count 1, 2, 3... under shuffle and identify nothing; the two only agree
 * because pl_order[i] == i when shuffle is off, which is why walking the wrong
 * one looked correct.
 *
 * Dead entries -- ones that failed to open -- are skipped so the total from
 * pl_live_count() and this ordinal are counting the same set. */
static uint16_t pl_live_ordinal(uint16_t pos)
{
    uint16_t f = (pos < pl_count) ? pl_order[pos] : 0u;
    uint16_t n = 0;
    for (uint16_t i = 0; i <= f && i < pl_count; i++)
        if (!pl_is_dead(i)) n++;
    return n ? n : 1u;
}
static void pl_dead_clear(void)
{
    for (uint32_t i = 0; i < sizeof(pl_dead) / sizeof(pl_dead[0]); i++) pl_dead[i] = 0;
    pl_dead_n = 0;
}

/* ---------------------------------------------------------------- diagnostic
 * Put the raw 0190 response struct on screen, 16 bytes per row, hex on the left
 * and printable ASCII on the right.
 *
 * Everything about the 0192 parameter struct here is inferred from this
 * response -- its layout, where the name sits, whether a length field precedes
 * it. If an open still fails, that inference is what is wrong, and no amount of
 * reasoning from outside will settle it. One photograph of this screen shows
 * the real layout. Bound to Select+B; harmless to leave in.
 */
static void pl_dump_struct(void)
{
    if (!pl_cmd(TGT_GETFILE, MP3_SLOT_ID, CLK_HZ)) { /* show what is there anyway */ }

    fb_rect(0, 0, FB_W, FB_H, UI_BG);
    fb_set_color(ui_accent, UI_BG);
    fb_text_clipped(6, 4, "0190 RESPONSE STRUCT", TS_1X, TS_1X, FB_W);

    for (uint32_t row = 0; row < 16u; row++) {
        char line[64]; char *q = line;
        uint8_t b[16];
        for (uint32_t k = 0; k < 16u; k += 4u) {
            uint32_t v = dt_read(DT_RESP_W + (row * 4u) + (k / 4u));
            b[k+0] = (uint8_t)(v >> 24); b[k+1] = (uint8_t)(v >> 16);
            b[k+2] = (uint8_t)(v >> 8);  b[k+3] = (uint8_t)v;
        }
        for (uint32_t k = 0; k < 16u; k++) {
            q = ui_hex2(q, b[k]);
            if ((k & 3u) == 3u) *q++ = ' ';
        }
        *q = 0;
        fb_set_color(UI_DIM, UI_BG);
        fb_text_clipped(6, 24u + row * 16u, line, TS_1X, TS_1X, FB_W - 90u);

        q = line;
        for (uint32_t k = 0; k < 16u; k++)
            *q++ = (b[k] >= 0x20u && b[k] < 0x7Fu) ? (char)b[k] : '.';
        *q = 0;
        fb_set_color(UI_WHITE, UI_BG);
        fb_text_clipped(FB_W - 86u, 24u + row * 16u, line, TS_1X, TS_1X, 84u);
    }

    fb_set_color(UI_FAINT, UI_BG);
    fb_text_clipped(6, 24u + 16u * 16u + 4u, "SELECT+B AGAIN TO RESUME",
                    TS_1X, TS_1X, FB_W);
}

/* ------------------------------------------------------------------ parsing */

/* Why a playlist load produced nothing -- so "no playlist" can say WHICH kind
 * of nothing, instead of leaving the user to guess between an empty slot, an
 * unreadable file and a file with no track lines in it. */
enum { PL_OK = 0, PL_ERR_NOFILE, PL_ERR_EMPTY };
static uint8_t pl_status;

/* Read the playlist slot into pl_text. The landing zone is the ID3 probe
 * buffer -- 4 KB, already reserved as a DMA target and not in use during a
 * playlist load. The ring cannot be used: it holds audio being decoded.
 *
 * The read size STEPS DOWN on failure. This host fails a read that extends past
 * the end of the file rather than returning a short one -- probe_file_size()
 * depends on exactly that behaviour to binary-search a file's length. So asking
 * for a flat 4 KB, as this first did, fails outright on any playlist smaller
 * than 4 KB, which is essentially all of them: a 20-track .m3u is under 500
 * bytes. Halving down finds the largest read the tail will actually serve --
 * all the way to a single byte, because stopping at 4 left the last 1-3 bytes
 * of the file unread and silently truncated the final track's name. */
static uint32_t pl_read_raw(void)
{
    uint32_t got = 0;
    int any = 0;
    pl_truncated = 0;

    /* Carry the last size that worked into the next chunk instead of restarting
     * the ladder at 4 KB every time. Restarting meant each successive chunk
     * re-failed every larger size before getting to a workable one: 62 APF
     * commands to read an 846-byte playlist, against 19 this way. Each command
     * is a cluster-chain walk, which is why the load was visibly slow. */
    uint32_t step = TAG_SIZE;

    /* One byte past the end, to tell "the file is exactly this long" from "the
     * file is longer and we stopped". Guessing from got == PL_TEXT_MAX would
     * cry truncation on a playlist that happens to end on the boundary. */
    while (got < PL_TEXT_MAX) {
        uint32_t want = PL_TEXT_MAX - got;
        if (want > step) want = step;

        uint32_t n = 0;
        while (want >= 1u) {
            /* Zero the landing zone first, then measure what actually arrived:
             * a read may also be CLAMPED rather than failed, and .m3u is text,
             * so a NUL reliably marks where the data stopped either way. */
            volatile uint8_t *z = (volatile uint8_t *)(uintptr_t)(UNCACHED + TAG_OFF);
            for (uint32_t k = 0; k < want; k++) z[k] = 0u;

            if (target_read_slot(PL_SLOT_ID, got, TAG_OFF, want)) {
                while (n < want && tagbuf[n]) n++;
                if (n) break;                  /* got real bytes at this size */
            }
            want >>= 1;                        /* too far past the end: shrink */
        }
        if (!n) break;

        step = want;                       /* start the next chunk here */
        any = 1;
        for (uint32_t k = 0; k < n; k++) pl_text[got + k] = (char)tagbuf[k];
        got += n;
        if (n < want) break;                            /* short read = tail */
    }

    /* Filled the buffer exactly? Then the file may or may not continue. One
     * byte past the end settles it, and only in that one case -- inferring it
     * from got == PL_TEXT_MAX alone would cry truncation at a playlist that
     * happens to end on the boundary. */
    if (any && got == PL_TEXT_MAX) {
        volatile uint8_t *z = (volatile uint8_t *)(uintptr_t)(UNCACHED + TAG_OFF);
        z[0] = 0u;
        if (target_read_slot(PL_SLOT_ID, got, TAG_OFF, 1u) && tagbuf[0])
            pl_truncated = 1u;
    }

    if (!any) pl_status = PL_ERR_NOFILE;
    return got;
}

/* Split the .m3u into names. Blank lines and #-comments are skipped, which
 * covers the extended format's #EXTM3U / #EXTINF lines, so both plain and
 * extended .m3u work with no separate parser. */
static void pl_parse(uint32_t len)
{
    pl_count = 0;
    uint32_t i = 0;
    while (i < len && pl_count < PL_MAX) {
        while (i < len && (pl_text[i] == '\n' || pl_text[i] == '\r')) i++;
        if (i >= len) break;

        uint32_t start = i;
        while (i < len && pl_text[i] != '\n' && pl_text[i] != '\r') i++;
        uint32_t end = i;

        /* Step PAST the line terminator BEFORE writing the NUL over it.
         *
         * Terminating first destroyed the very byte the next iteration's skip
         * loop looks for, so that loop stopped skipping and every following
         * line began at the NUL rather than at its first real character. A line
         * starting with NUL is not '#', so EVERY COMMENT LINE COUNTED AS A
         * TRACK -- a 23-line comment header parsed as 23 tracks. */
        if (i < len && pl_text[i] == '\r') i++;
        if (i < len && pl_text[i] == '\n') i++;

        /* Trailing whitespace, and the \r of a Windows line ending that a
         * hand-edited file will almost always have. */
        while (end > start && (pl_text[end-1] == ' ' || pl_text[end-1] == '\t'))
            end--;
        pl_text[end] = 0;

        if (end > start && pl_text[start] != '#')
            pl_off[pl_count++] = (uint16_t)start;
    }

    /* Ran out of index before running out of file. */
    while (i < len && (pl_text[i] == '\n' || pl_text[i] == '\r')) i++;
    if (i < len) pl_truncated = 1u;
}

/* ---------------------------------------------------------------- ordering */

static uint32_t pl_rand(void)
{
    /* xorshift32. The seed comes from the cycle counter at the moment the user
     * turned shuffle on, so the order differs between runs. */
    pl_rng ^= pl_rng << 13; pl_rng ^= pl_rng >> 17; pl_rng ^= pl_rng << 5;
    return pl_rng;
}

static void pl_reorder(void)
{
    for (uint16_t i = 0; i < pl_count; i++) pl_order[i] = i;
    if (!shuffle_on || pl_count < 2u) return;

    /* Fisher-Yates: every permutation equally likely, and no track repeats
     * until the list is exhausted -- unlike picking at random each time, which
     * can play the same track twice in a row. */
    for (uint16_t i = (uint16_t)(pl_count - 1u); i > 0; i--) {
        uint16_t j = (uint16_t)(pl_rand() % (uint32_t)(i + 1u));
        uint16_t t = pl_order[i]; pl_order[i] = pl_order[j]; pl_order[j] = t;
    }
}

/* Keep playing what is playing when the order changes: find the current track
 * in the new order rather than jumping to whatever landed at the same index. */
static void pl_resync(uint16_t file_idx)
{
    for (uint16_t i = 0; i < pl_count; i++)
        if (pl_order[i] == file_idx) { pl_pos = i; return; }
    pl_pos = 0;
}

/* --------------------------------------------------------------- interface */

static void ui_toast_msg(const char *msg);

/* Why the LAST attempt failed. Only reached when the walk below has given up
 * on the whole request -- a single bad entry it steps over says nothing. */
static void pl_toast_open_fail(void)
{
    switch (pl_open_err) {
    case PLO_GETFILE: ui_toast_msg("OPEN FAIL: NO 0190");   break;
    case PLO_NONAME:  ui_toast_msg("OPEN FAIL: NO NAME");   break;
    case PLO_TOOLONG: ui_toast_msg("OPEN FAIL: NAME LONG"); break;
    default:          ui_toast_set("OPEN FAIL: 0192 ERR",
                                   pl_open_errcode, 0);     break;
    }
}

/* Everything that has to follow a SUCCESSFUL open, and nothing that survives a
 * failed one -- which is what lets the walk below attempt an entry without
 * committing to it.
 *
 * A successful 0192 switches the slot but raises NO 008A -- that notification
 * is for a file the USER picked from the menu. So nothing here can wait for the
 * reload path to notice: without driving it, file_pos, the decoder and the ring
 * all carry over and the new track starts at the previous one's position. That
 * is the same fault the original "Load MP3 resumed mid-song" bug had, arriving
 * by a different route.
 *
 * Rather than re-implement loading, arm the very same gate the menu reload
 * uses, with the OUTGOING file's identity as the reference so 0190 can confirm
 * the switch actually happened before the track is loaded. */
static void pl_arm_load(uint32_t leaving_id)
{
    for (uint32_t i = 0; i < sizeof(stale_ref_title); i++)
        stale_ref_title[i] = last_title[i];
    stale_ref_size    = slot_size;
    stale_ref_file_id = leaving_id;

    /* R_SLOT_SZ is only latched by an 008A edge, so it still describes the
     * track being left. Measure the new one instead of inheriting that. */
    force_size_probe = 1;
    slot_size        = 0;

    reload_pending  = 0;
    reload_armed    = 1;
    reload_probe_at = cycles();
    /* Shorter settle than a menu pick: 0192 has already reported success, so
     * the slot is open rather than merely chosen. */
    reload_settle   = cycles() + CLK_HZ / 4u;
    reload_at       = cycles() + CLK_HZ * 3u;
}

/* Play the first entry at or after `pos` that will actually open, walking in
 * `dir` (+1 forward, -1 back) past the ones that will not.
 *
 * One mistyped line in an .m3u used to cost the whole playlist: a single
 * attempt was made, and no caller retried, so the failure propagated all the
 * way out to the boot path and the core never reached the player. One bad line
 * should cost one track.
 *
 * `span` is how many positions the walk may visit, and it is what stops the
 * skipping from changing the MEANING of a caller's request. At most pl_count --
 * every position visited at most once -- and that bound is load-bearing rather
 * than defensive: pl_skip() and the end-of-track auto-advance both arrive here,
 * so an unbounded retry would turn a list of missing files from a one-track
 * hiccup into an infinite loop. Auto-advance with repeat OFF passes a shorter
 * span so that walking past a missing last entry ends the playlist instead of
 * wrapping round and restarting it. */
static int pl_play_span(uint16_t pos, int dir, uint16_t span)
{
    if (!pl_count || !span) return 0;
    if (span > pl_count) span = pl_count;
    uint16_t prev_pos = pl_pos;

    /* Captured BEFORE any open: after a successful one, 0190 answers with the
     * new file. A FAILED open changes neither, so one capture serves the whole
     * walk -- and hoisting the blocking read out of the loop means a run of bad
     * entries does not pay for it once each. */
    uint32_t leaving_id = cur_file_id;

    /* Snapshot the head of the song we are LEAVING, so read_track_head can
     * wait for the slot to return something else. Audio is already cut at this
     * point, so a blocking 16-byte read costs nothing audible. */
    sw_have_prev = 0;
    if (target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, 16u)) {
        for (uint32_t i = 0; i < sizeof(sw_prev_head); i++)
            sw_prev_head[i] = tagbuf[i];
        sw_have_prev = 1;
    }

    uint16_t at = (uint16_t)(pos % pl_count);
    uint16_t nmiss = 0, nfail = 0;

    for (uint16_t n = 0; n < span; n++) {
        uint16_t file_idx = pl_order[at];

        /* Already known absent: skip without spending a command on it. Each
         * 0192 makes APF re-walk the cluster chain, so re-asking about a file
         * that was missing thirty seconds ago is the expensive way to get the
         * same answer. */
        if (pl_is_dead(file_idx)) {
            nmiss++;
        } else {
            pl_pos = at;
            track_from_pl = 1u;      /* the playlist started this track */
            if (pl_open_name(&pl_text[pl_off[file_idx]])) {
                pl_arm_load(leaving_id);
                /* ONE line about the whole walk, not one per bad entry: a list
                 * half mistyped would otherwise queue toasts faster than they
                 * can be read. (A "2 of 14" figure would need every entry
                 * probed at load time, and probing costs a cluster walk each --
                 * so this counts what this attempt actually walked past.) */
                if (nmiss)      ui_toast_set("SKIPPED", nmiss, " MISSING");
                else if (nfail) ui_toast_set("SKIPPED", nfail, " UNREADABLE");
                return 1;
            }

            switch (pl_classify_failure()) {
            case PLF_FATAL:
                /* Not about this entry -- the command path itself is not
                 * answering, and the next 127 entries would say so too. */
                pl_pos = prev_pos;
                pl_toast_open_fail();
                return 0;
            case PLF_DEAD:
                pl_mark_dead(file_idx);
                nmiss++;
                break;
            default:
                nfail++;
                break;
            }
        }

        at = (uint16_t)((at + pl_count + (dir > 0 ? 1u : (uint16_t)(pl_count - 1u)))
                        % pl_count);
    }

    /* Nothing in the list opened. Stay where we were and say so plainly --
     * the caller (the boot path especially) has to be able to tell this from
     * "no playlist at all". */
    pl_pos = prev_pos;
    if (nmiss) ui_toast_set("NO TRACKS:", nmiss, " MISSING");
    else       pl_toast_open_fail();
    return 0;
}

static int pl_play_dir(uint16_t pos, int dir) { return pl_play_span(pos, dir, pl_count); }
static int pl_play_at(uint16_t pos)           { return pl_play_dir(pos, +1); }

/* delta = +1 next, -1 previous. Wraps in both directions regardless of repeat
 * mode: a MANUAL skip past the end should go to track 1, not stop dead.
 * Repeat only governs what happens when a track ends on its own. */
static int pl_skip(int delta)
{
    if (pl_count < 1u) return 0;
    uint16_t next = (uint16_t)((pl_pos + pl_count + (delta > 0 ? 1u : (uint16_t)(pl_count - 1u)))
                               % pl_count);
    /* Skip past bad entries in the DIRECTION THE USER PRESSED. Walking forward
     * out of a back-press would hand them the track they were already on. */
    return pl_play_dir(next, delta);
}

/* A track finished by itself. Returns 1 if a new track was started, 0 if the
 * caller should fall back to its own end-of-track handling. */
static int pl_advance_auto(void)
{
    if (rep_mode == REP_ONE) return 0;          /* caller replays this track */
    if (!pl_count) return 0;

    if (pl_pos + 1u >= pl_count) {
        if (rep_mode != REP_ALL) return 0;      /* end of list: let it stop */
        /* A fresh shuffle each time round, otherwise "shuffle" plays the same
         * random order forever and stops feeling random after one pass. */
        if (shuffle_on) pl_reorder();
        return pl_play_at(0);
    }
    /* Repeat ALL may skip past the end and come round; repeat OFF must NOT.
     * Without the shorter span, a missing LAST entry would send the walk round
     * to track 1 and silently restart a playlist the user had set to stop --
     * skipping a bad file is allowed to cost a track, never to change the
     * repeat mode's behaviour. */
    if (rep_mode == REP_ALL) return pl_play_at((uint16_t)(pl_pos + 1u));
    return pl_play_span((uint16_t)(pl_pos + 1u), +1,
                        (uint16_t)(pl_count - (pl_pos + 1u)));
}

/* Load (or reload) the playlist slot. Safe to call when no playlist file is
 * present -- pl_count stays 0 and every playlist control becomes a no-op. */
/* The playlist slot's own filename, last component only, uppercased for the
 * splash. Empty if APF has nothing there or will not answer.
 *
 * Doubles as the diagnostic for the multi-playlist defect: our code never
 * names a playlist, it just reads whatever is in slot 3, so if resume behaves
 * differently for audiobook.m3u than for playlist.m3u the difference has to be
 * in what APF put there. Showing the name settles that in one boot. */
static void pl_name_read(void)
{
    pl_name[0] = 0; pl_name_raw[0] = 0;
    uint8_t buf[DT_WORDS * 4u];
    uint32_t at = 0, len = pl_template(PL_SLOT_ID, buf, &at);
    if (!len) return;

    /* Last path component only -- the slot holds a full path. */
    uint32_t start = at;
    for (uint32_t i = at; i < at + len; i++)
        /* 0x5C is a backslash; writing it as a char literal here kept
         * getting mangled by the tooling that generated this file. */
        if (buf[i] == '/' || buf[i] == 0x5Cu) start = i + 1u;

    uint32_t n = 0;
    for (uint32_t i = start; i < at + len && n < sizeof(pl_name) - 1u; i++) {
        char c = (char)buf[i];
        /* Raw case is kept as well as the uppercased display form. The stored
         * stem is reopened BY NAME later, and uppercasing it would ask APF for
         * SHENANIGANS.m3u when the card holds Shenanigans.m3u -- fine on FAT,
         * not something to rely on. */
        pl_name_raw[n] = c;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        pl_name[n++] = c;
    }
    pl_name[n] = 0;
    pl_name_raw[n] = 0;
}

/* The name the core falls back to. Only used when APF has nothing in the
 * playlist slot -- once the user picks a list, that pick is what persists. */
#define PL_DEFAULT_NAME "playlist.m3u"

static int pl_open_name(const char *name)
{
    return pl_open_into(MP3_SLOT_ID, name);
}

/* Reduce pl_name ("SHENANIGANS.M3U") to the stem we store ("SHENANIGANS").
 * Uppercase throughout: pl_name is already uppercased for the splash, and
 * comparing case-insensitively is what lets a stored stem match a slot whose
 * path APF reports in whatever case it likes. */
static void pl_stem_of(const char *name, char *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; name[i] && n < PL_STEM_MAX; i++) {
        if (name[i] == '.') break;                 /* stop at the extension */
        out[n++] = name[i];                        /* CASE PRESERVED */
    }
    out[n] = 0;
}

static char pl_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Case-insensitive: the two sides come from different places and only the
 * card's own spelling is authoritative. */
static int pl_stem_eq(const char *a, const char *b)
{
    for (uint32_t i = 0; i <= PL_STEM_MAX; i++) {
        if (pl_upper(a[i]) != pl_upper(b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

/* Remember whichever list is loaded, so the next boot can reopen it. Called
 * once a playlist has actually parsed -- an unreadable file must not overwrite
 * a good remembered name. */
static void pl_remember(void)
{
    char stem[PL_STEM_MAX + 1u];
    pl_stem_of(pl_name_raw, stem);
    if (!stem[0] || pl_stem_eq(stem, pl_saved_stem)) return;
    for (uint32_t i = 0; i <= PL_STEM_MAX; i++) pl_saved_stem[i] = stem[i];
    settings_mark_dirty();
}

static void pl_load(void)
{
    pl_count = 0; pl_pos = 0; pl_status = PL_OK;
    /* A new list means new entries: nothing carries over. This is also the only
     * way back for a user who fixed a filename on the card -- re-picking the
     * playlist gives every entry another chance. */
    pl_dead_clear();
    pl_name_read();

    /* Ask by READING, not by asking what is in the slot.
     *
     * The first version tested pl_name[0] for emptiness and skipped the
     * fallback whenever it looked non-empty -- but pl_name comes from the 0190
     * response struct, which holds RESIDUE from whatever getfile ran last. So
     * it read as full when the slot was empty, the fallback never fired, and
     * no playlist loaded at all. The only trustworthy test of "is there a
     * playlist here" is trying to read one.
     *
     * data.json no longer declares a filename for slot 3: declaring one made
     * APF reset the slot at every core load and throw away the user's chosen
     * list, which was the whole multi-playlist defect. Opening the documented
     * default here puts the first-run convenience back. */
    /* If a different list is remembered, reopen it BEFORE reading.
     *
     * The slot cannot remember anything itself -- with a filename declared in
     * data.json APF resets it to that file every core load, and without one it
     * comes up empty. Both measured. So the core reopens the remembered stem
     * by name, which is the only mechanism the platform offers.
     *
     * Skipped when the slot already holds the right list, so the ordinary
     * case costs nothing and picking a new list mid-session is untouched. */
    /* ONLY on the first load of the session.
     *
     * pl_load() runs at boot AND every time the user picks a list, and the
     * first version did not distinguish them -- so choosing Shenanigans.m3u
     * made this see a name differing from the remembered one and helpfully
     * "restore" the remembered one over the top. It fought the user's pick
     * every single time, which is exactly how it was reported. */
    if (pl_restore_pending && pl_saved_stem[0]) {
        char cur[PL_STEM_MAX + 1u];
        pl_stem_of(pl_name_raw, cur);
        if (!pl_stem_eq(cur, pl_saved_stem)) {
            char want[PL_STEM_MAX + 5u];
            uint32_t n = 0;
            while (pl_saved_stem[n] && n < PL_STEM_MAX) { want[n] = pl_saved_stem[n]; n++; }
            want[n++] = '.'; want[n++] = 'm'; want[n++] = '3'; want[n++] = 'u';
            want[n] = 0;
            if (pl_open_into(PL_SLOT_ID, want)) pl_name_read();
        }
    }

    pl_restore_pending = 0;

    uint32_t len = pl_read_raw();
    if (!len && pl_open_into(PL_SLOT_ID, PL_DEFAULT_NAME)) {
        pl_name_read();
        len = pl_read_raw();
    }
    if (!len) { pl_status = PL_ERR_NOFILE; return; }

    /* FNV-1a over the raw text, before parsing touches anything. */
    {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < len; i++) {
            h ^= (uint32_t)(uint8_t)pl_text[i];
            h *= 16777619u;
        }
        pl_sig = h ? h : 1u;               /* 0 is reserved for "none" */
    }

    pl_parse(len);
    if (!pl_count) { pl_status = PL_ERR_EMPTY; return; }

    /* Seed HERE, not only when shuffle is switched on.
     *
     * pl_rng is a static 1 and the toggle handler was the only thing that ever
     * seeded it from cycles(). Shuffle persists across sessions, so a user who
     * leaves it on and never presses Select+R got pl_reorder() running on seed
     * 1 at every boot -- a real Fisher-Yates shuffle producing the identical
     * permutation forever.
     *
     * This point has genuine entropy: the playlist reads above walk the card's
     * cluster chain, whose timing varies run to run, so the cycle counter here
     * is not the fixed value it would be earlier in a deterministic boot. */
    pl_remember();
    pl_rng = cycles() | 1u;
    pl_reorder();
}

/* One line saying what happened, so a playlist that did not load says why
 * rather than just not working. */
static void pl_report(void)
{
    if (pl_count && pl_truncated)         ui_toast_set("CLIPPED AT ", pl_live_count(),
                                                       " TRACKS");
    else if (pl_count)                    ui_toast_set("PLAYLIST", pl_live_count(),
                                                       " TRACKS");
    else if (pl_status == PL_ERR_NOFILE)  ui_toast_msg("NO PLAYLIST FILE");
    else                                  ui_toast_msg("PLAYLIST HAS NO TRACKS");
}
