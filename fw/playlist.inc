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
#define DT_RESP_W   0u      /* response struct  (APF writes) */
#define DT_PARAM_W  64u     /* parameter struct (APF reads)  */
#define DT_WORDS    64u     /* 256 bytes, matching slot_filename()'s window */

static inline void dt_write(uint32_t word, uint32_t val)
{
    REG(R_DT_ADDR) = word;
    REG(R_DT_DATA) = val;
}

/* Run one target command and wait for the sequence counter to move. Same
 * discipline as target_read_poll(): never trust the `done` LEVEL, which stays
 * asserted from the previous command until the next one is picked up. */
static int pl_cmd(uint32_t sel, uint32_t slot, uint32_t timeout_cycles)
{
    refill_drain();                       /* one command in flight, ever */
    uint32_t seq0 = (REG(R_TGT_GO) >> 8) & 0xFFu;
    REG(R_TGT_ID) = slot;
    REG(R_TGT_GO) = sel;

    uint32_t deadline = cycles() + timeout_cycles;
    for (;;) {
        uint32_t st = REG(R_TGT_GO);
        if (((st >> 8) & 0xFFu) != seq0) return (((st >> 2) & 7u) == 0u);
        if ((int32_t)(cycles() - deadline) >= 0) return 0;
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

static int pl_open_name(const char *name)
{
    pl_open_err = PLO_OK; pl_open_errcode = 0;

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
    if (!pl_cmd(TGT_OPENFILE, MP3_SLOT_ID, CLK_HZ * 3u)) {
        pl_open_err     = PLO_OPEN;
        pl_open_errcode = (uint8_t)((REG(R_TGT_GO) >> 2) & 7u);
        return 0;
    }
    return 1;
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

    /* Carry the last size that worked into the next chunk instead of restarting
     * the ladder at 4 KB every time. Restarting meant each successive chunk
     * re-failed every larger size before getting to a workable one: 62 APF
     * commands to read an 846-byte playlist, against 19 this way. Each command
     * is a cluster-chain walk, which is why the load was visibly slow. */
    uint32_t step = TAG_SIZE;

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

/* Start the track at `pos`.
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
static int pl_play_at(uint16_t pos)
{
    if (!pl_count) return 0;
    uint16_t prev_pos = pl_pos;
    pl_pos = (uint16_t)(pos % pl_count);
    const char *name = &pl_text[pl_off[pl_order[pl_pos]]];

    /* Captured BEFORE the open: after it, 0190 answers with the new file. */
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

    if (!pl_open_name(name)) {
        pl_pos = prev_pos;                  /* stay where we were */
        switch (pl_open_err) {
        case PLO_GETFILE: ui_toast_msg("OPEN FAIL: NO 0190");   break;
        case PLO_NONAME:  ui_toast_msg("OPEN FAIL: NO NAME");   break;
        case PLO_TOOLONG: ui_toast_msg("OPEN FAIL: NAME LONG"); break;
        default:          ui_toast_set("OPEN FAIL: 0192 ERR",
                                       pl_open_errcode, 0);     break;
        }
        return 0;
    }

    /* The slot has switched, but nothing else knows: file_pos, the decoder and
     * the ring all still describe the previous track, which is why a skip used
     * to start the new song at the old one's elapsed position. Arm the same
     * gate a menu reload uses, with the OUTGOING identity as the reference so
     * 0190 can confirm the switch before the track is loaded. */
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
    return 1;
}

/* delta = +1 next, -1 previous. Wraps in both directions regardless of repeat
 * mode: a MANUAL skip past the end should go to track 1, not stop dead.
 * Repeat only governs what happens when a track ends on its own. */
static int pl_skip(int delta)
{
    if (pl_count < 1u) return 0;
    uint16_t next = (uint16_t)((pl_pos + pl_count + (delta > 0 ? 1u : (uint16_t)(pl_count - 1u)))
                               % pl_count);
    return pl_play_at(next);
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
    return pl_play_at((uint16_t)(pl_pos + 1u));
}

/* Load (or reload) the playlist slot. Safe to call when no playlist file is
 * present -- pl_count stays 0 and every playlist control becomes a no-op. */
static void pl_load(void)
{
    pl_count = 0; pl_pos = 0; pl_status = PL_OK;
    uint32_t len = pl_read_raw();
    if (!len) { pl_status = PL_ERR_NOFILE; return; }
    pl_parse(len);
    if (!pl_count) { pl_status = PL_ERR_EMPTY; return; }
    pl_reorder();
}

/* One line saying what happened, so a playlist that did not load says why
 * rather than just not working. */
static void pl_report(void)
{
    if (pl_count)                         ui_toast_set("PLAYLIST", pl_count, " TRACKS");
    else if (pl_status == PL_ERR_NOFILE)  ui_toast_msg("NO PLAYLIST FILE");
    else                                  ui_toast_msg("PLAYLIST HAS NO TRACKS");
}
