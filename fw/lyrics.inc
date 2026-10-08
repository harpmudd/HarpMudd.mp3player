/* Lyrics: sidecar first, the file's own ID3v2 USLT frame second, then
 * parse and hand lines to the meter.
 *
 * SIDECAR FIRST. `<track>.mp3` -> `<track>.lrc` in the same folder. It
 * needs no tag parsing on either format, which matters because the two in-file
 * routes both have obstacles: ID3 `USLT` is not a plain text frame (encoding
 * byte, language, then a NUL-terminated descriptor before the text), and
 * `vorbis_comments()` keeps only the first 80 bytes of a value, which a lyric
 * sheet blows through immediately.
 *
 * THE DIRECTORY COMES FOR FREE. `pl_open_try()` builds its 0192 parameter
 * struct from a 0190 template and keeps the template slot's directory, swapping
 * only the last path component. Templating off the MP3 slot therefore looks for
 * the .lrc wherever the track itself lives -- album subfolders included -- with
 * no path construction here at all.
 *
 * READ AT LOAD, NEVER LAZILY. Touching another slot drops the fragment cache
 * for the streaming slot, and doing that mid-playback is exactly the pattern
 * behind the v1.4.0 stutter: no underrun is recorded, the decoder just resyncs
 * on wrong bytes. So this runs inside load_track() alongside the artwork,
 * whether or not the lyrics meter happens to be showing.
 */

/* The buffers and the size constants live in player.c, above
 * ui_draw_dynamic() -- that function sits higher in the file than any include,
 * so anything the meter touches has to be declared before it. Same reason the
 * FLAC and playlist declarations are hoisted. Only the logic is here, and it
 * lands after playlist.inc because lrc_load() calls pl_open_try(). */

static void lrc_clear(void)
{
    lrc_n = 0; lrc_synced = 0; lrc_text[0] = 0;
    /* INVALIDATE THE METER'S REDRAW GATE, which is keyed on the line index.
     *
     * A new track starts at line 0, and a track with no sidecar also sits at
     * line 0 -- so going from one to the other left the gate reading
     * "unchanged" and the previous track's lyrics stayed on screen until
     * something forced a repaint. Stopping and starting appeared to fix it
     * only because that raises ui_wave_force.
     *
     * Same shape as the fl_first_frame leak: per-track state that nothing
     * reset on a track change. */
    lrc_drawn = 0xFFFFu;
}

/* Read the whole file. Same shrinking ladder as pl_read_raw(), and for the same
 * reason: a read past EOF FAILS rather than returning short, so the size has to
 * be discovered by trying. Cheap here -- a lyric sheet is one 4 KB read against
 * artwork's 43 to 207. */
static uint32_t lrc_read_raw(void)
{
    uint32_t got = 0, step = TAG_SIZE;

    while (got < LRC_TEXT_MAX - 1u) {
        uint32_t want = (LRC_TEXT_MAX - 1u) - got;
        if (want > step) want = step;

        uint32_t n = 0;
        while (want >= 1u) {
            /* Zero first, then measure what arrived: a read may be CLAMPED as
             * well as failed, and .lrc is text, so a NUL marks the end either
             * way. */
            volatile uint8_t *z = (volatile uint8_t *)(uintptr_t)(UNCACHED + TAG_OFF);
            for (uint32_t k = 0; k < want; k++) z[k] = 0u;
            if (target_read_slot(LRC_SLOT_ID, got, TAG_OFF, want)) {
                while (n < want && tagbuf[n]) n++;
                if (n) break;
            }
            want >>= 1;
        }
        if (!n) break;

        step = want;
        for (uint32_t k = 0; k < n; k++) lrc_text[got + k] = (char)tagbuf[k];
        got += n;
        if (n < want) break;                        /* short read = tail */
    }
    lrc_text[got] = 0;
    return got;
}

/* Split into lines and pull the timestamps off the front.
 *
 * A line is `[mm:ss.xx]text`, and several stamps may share one line when the
 * same words recur -- only the first is kept, so a repeated chorus shows at its
 * first occurrence rather than not at all.
 *
 * Metadata lines (`[ar:...]`, `[ti:...]`) fall out for free: the character
 * after `[` is not a digit, so no time is taken, and they carry no text after
 * the bracket to store.
 *
 * Lines are NUL-terminated IN PLACE, which is why lrc_off holds offsets rather
 * than pointers -- the buffer is the storage and the index at once. */
static void lrc_parse(uint32_t len)
{
    lrc_n = 0; lrc_synced = 0;
    uint32_t i = 0;

    while (i < len && lrc_n < LRC_LINES) {
        while (i < len && (lrc_text[i] == '\n' || lrc_text[i] == '\r')) i++;
        if (i >= len) break;

        uint32_t a = i;
        while (i < len && lrc_text[i] != '\n' && lrc_text[i] != '\r') i++;
        uint32_t b = i;

        /* STEP PAST THE TERMINATOR BEFORE the NUL is written over it below.
         *
         * Terminating each line in place is what lets lrc_off hold offsets
         * instead of pointers -- the buffer is storage and index at once. But
         * the NUL lands exactly where the newline was, and the scan at the top
         * of this loop skips NEWLINES. Writing first left `i` parked on a NUL
         * it did not recognise, so every line after the first was parsed
         * starting at that NUL: empty text, no timestamp. Only line 0 carried a
         * time, lrc_index_for() could only return 0, and the meter sat on the
         * first lyric for the whole song. */
        if (i < len) i++;

        while (b > a && lrc_text[b - 1u] == ' ') b--;

        uint32_t t = LRC_NOTIME, p = a;
        while (p + 1u < b && lrc_text[p] == '[') {
            uint32_t q = p + 1u, mm = 0, ss = 0, cs = 0;
            int ok = 0;
            if (lrc_text[q] >= '0' && lrc_text[q] <= '9') {
                while (q < b && lrc_text[q] >= '0' && lrc_text[q] <= '9')
                    mm = mm * 10u + (uint32_t)(lrc_text[q++] - '0');
                if (q < b && lrc_text[q] == ':') {
                    q++;
                    while (q < b && lrc_text[q] >= '0' && lrc_text[q] <= '9')
                        ss = ss * 10u + (uint32_t)(lrc_text[q++] - '0');
                    ok = 1;
                    /* The fractional part, which used to be skipped along with
                     * everything else up to the ']'. Both `.` and `:` appear as
                     * the separator in the wild. One digit means tenths
                     * already; two means centiseconds. */
                    if (q < b && (lrc_text[q] == '.' || lrc_text[q] == ':')) {
                        q++;
                        uint32_t d = 0;
                        while (q < b && d < 2u &&
                               lrc_text[q] >= '0' && lrc_text[q] <= '9') {
                            cs = cs * 10u + (uint32_t)(lrc_text[q++] - '0');
                            d++;
                        }
                        if (d == 1u) cs *= 10u;
                    }
                }
            }
            while (q < b && lrc_text[q] != ']') q++;
            if (q >= b) break;
            q++;
            if (ok && t == LRC_NOTIME) {
                /* TENTHS, not seconds. uint16 holds 6553 s that way -- an hour
                 * and three quarters, past any track -- and a second of slop
                 * was audible on fast passages where lines land close. */
                uint32_t v = (mm * 60u + ss) * 10u + cs / 10u;
                t = (v < LRC_NOTIME) ? v : (LRC_NOTIME - 1u);
                lrc_synced = 1u;
            }
            p = q;
        }
        while (p < b && lrc_text[p] == ' ') p++;

        if (p < b) {
            lrc_off[lrc_n] = (uint16_t)p;
            lrc_sec[lrc_n] = (uint16_t)t;
            lrc_text[b]    = 0;
            lrc_n++;
        }
    }
}

/* Playback position in TENTHS. ui_sec alone is a second wide, which is enough
 * to put a line visibly out of step where they land close together; the clock's
 * own accumulator already carries the remainder, so the precision is free. */
static uint32_t lrc_now10(void)
{
    uint32_t t = ui_sec * 10u;
    if (samprate) t += (ui_sec_acc * 10u) / samprate;
    return t;
}

/* Which line belongs to `t10`: the last one whose stamp has passed. Linear over
 * at most 96 entries, which is nothing beside a single fb_rect. */
static uint32_t lrc_index_for(uint32_t t10)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < lrc_n; i++)
        if (lrc_sec[i] != LRC_NOTIME && (uint32_t)lrc_sec[i] <= t10) k = i;
    return k;
}


/* ---- Lyrics embedded in the file's own ID3v2 tag -------------------------
 *
 * A FALLBACK behind the sidecar, not a replacement. Measured over the
 * reference library of 7,169 MP3s: 4,618 carry a USLT frame and 4,563 of
 * those are 12-byte EMPTY placeholders -- an encoding byte, "XXX", a BOM and
 * nothing else, written wholesale by some tagger. Sixteen have real content
 * and fourteen of THOSE are a filesharing watermark. Two files in seven
 * thousand hold actual lyrics. So this earns its place only where a sidecar
 * is absent, and an empty frame must read as "no lyrics" rather than as an
 * empty panel.
 *
 * USLT is NOT a text frame, which is why id3_text_body() cannot be reused:
 * after the encoding byte come three bytes of language and then a
 * NUL-TERMINATED DESCRIPTOR in the frame's own encoding -- for UTF-16 that
 * is a 16-bit terminator, and the descriptor and the text each carry their
 * own BOM.
 *
 * Nothing stops anyone pasting LRC-formatted text into a lyrics tag, so the
 * decoded body goes through the SAME lrc_parse() the sidecar uses: a tag
 * with timestamps gives synced lyrics, one without scrolls unsynced. */

/* Defined further down player.c than this include sits, same reason the
 * FLAC and playlist declarations are hoisted. */
static uint32_t id3_len(const uint8_t *b);

/* Decode a USLT body into lrc_text as UTF-8, a tag-buffer window at a time.
 *
 * Windowed because the frame can exceed the 4 KB tag buffer -- the UTF-16
 * test file's is 6 KB, and UTF-16 is the encoding every tagged file in that
 * library uses. Wide windows are kept 2-byte aligned so a code unit is never
 * split across a read; the 8-bit encodings copy bytes verbatim, so a split
 * UTF-8 sequence simply continues in the next window. */
static uint32_t lrc_uslt_decode(uint32_t pos, uint32_t len)
{
    uint32_t want = (len < TAG_SIZE) ? len : TAG_SIZE;
    if (want < 6u) return 0;
    if (!target_read_slot(MP3_SLOT_ID, pos, TAG_OFF, want)) return 0;

    uint8_t  enc  = tagbuf[0];
    int      wide = (enc == 1u || enc == 2u);
    int      be   = (enc == 2u);
    uint32_t s    = 4u;                  /* encoding byte + 3-byte language */

    if (wide) {
        if (enc == 1u && s + 1u < want) {
            if (tagbuf[s] == 0xFFu && tagbuf[s + 1u] == 0xFEu)      { be = 0; s += 2u; }
            else if (tagbuf[s] == 0xFEu && tagbuf[s + 1u] == 0xFFu) { be = 1; s += 2u; }
        }
        while (s + 1u < want && (tagbuf[s] || tagbuf[s + 1u])) s += 2u;
        s += 2u;                          /* past the 16-bit terminator */
        if (enc == 1u && s + 1u < want) { /* the TEXT carries its own BOM */
            if (tagbuf[s] == 0xFFu && tagbuf[s + 1u] == 0xFEu)      { be = 0; s += 2u; }
            else if (tagbuf[s] == 0xFEu && tagbuf[s + 1u] == 0xFFu) { be = 1; s += 2u; }
        }
    } else {
        while (s < want && tagbuf[s]) s++;
        s += 1u;
    }
    if (s >= want) return 0;

    uint32_t out  = 0;                    /* bytes written to lrc_text */
    uint32_t off  = s, have = want;
    uint32_t fp   = pos + want;           /* next unread file offset */
    uint32_t end  = pos + len;

    while (out + 4u < LRC_TEXT_MAX) {
        if (off + (wide ? 1u : 0u) >= have) {
            if (fp >= end) break;
            uint32_t n = end - fp;
            if (n > TAG_SIZE) n = TAG_SIZE;
            if (wide) n &= ~1u;
            if (!n || !target_read_slot(MP3_SLOT_ID, fp, TAG_OFF, n)) break;
            off = 0; have = n; fp += n;
            continue;
        }
        uint32_t o2;
        if (wide) {
            uint32_t u = be ? (((uint32_t)tagbuf[off] << 8) | tagbuf[off + 1u])
                            : (((uint32_t)tagbuf[off + 1u] << 8) | tagbuf[off]);
            off += 2u;
            if (!u) break;
            if (u >= 0xD800u && u < 0xE000u) { if (u < 0xDC00u) off += 2u; u = '?'; }
            o2 = u8_put(lrc_text, out, LRC_TEXT_MAX, u);
        } else {
            uint8_t c = tagbuf[off++];
            if (!c) break;
            if (enc == 3u) {              /* already UTF-8: copy verbatim */
                if (out + 1u >= LRC_TEXT_MAX) break;
                lrc_text[out] = (char)c;
                o2 = out + 1u;
            } else {
                o2 = u8_put(lrc_text, out, LRC_TEXT_MAX, c);
            }
        }
        if (o2 == out) break;
        out = o2;
    }
    lrc_text[out] = 0;
    return out;
}

/* Walk the tag for a USLT frame. Same shape as id3_walk_collect(): a sliding
 * window over frame HEADERS, bodies skipped by arithmetic, so a megabyte of
 * cover art ahead of the lyrics costs nothing. */
static uint32_t lrc_tag_load(void)
{
    if (track_fmt != FMT_MP3) return 0;          /* ID3 is an MP3 tag */
    if (!target_read_slot(MP3_SLOT_ID, 0, TAG_OFF, 512u)) return 0;
    if (tagbuf[0] != 'I' || tagbuf[1] != 'D' || tagbuf[2] != '3') return 0;

    uint8_t  major   = tagbuf[3];
    uint32_t tag_len = id3_len(tagbuf);
    if (tag_len < 20u) return 0;

    uint32_t wo = 0, wl = 512u, p = 10u;
    while (p + 10u <= tag_len) {
        if (!(p >= wo && p + 10u <= wo + wl)) {
            if (!target_read_slot(MP3_SLOT_ID, p, TAG_OFF, 512u)) return 0;
            wo = p; wl = 512u;
        }
        const uint8_t *h = tagbuf + (p - wo);
        if (h[0] == 0) return 0;                 /* padding reached */

        uint32_t fsize = (major >= 4)
            ? (((uint32_t)(h[4] & 0x7Fu) << 21) | ((uint32_t)(h[5] & 0x7Fu) << 14) |
               ((uint32_t)(h[6] & 0x7Fu) << 7)  |  (uint32_t)(h[7] & 0x7Fu))
            : (((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) |
               ((uint32_t)h[6] << 8)  |  (uint32_t)h[7]);
        if (!fsize || p + 10u + fsize > tag_len) return 0;

        if (h[0] == 'U' && h[1] == 'S' && h[2] == 'L' && h[3] == 'T')
            return lrc_uslt_decode(p + 10u, fsize);

        p += 10u + fsize;
    }
    return 0;
}

/* Called from load_track(). Silent when there is no sidecar -- most tracks will
 * not have one, and that is not a failure worth reporting. */
static void lrc_load(void)
{
    lrc_clear();

    char path[96];
    slot_filename(path, sizeof(path));
    if (!path[0]) return;

    /* Last component only. pl_open_try() supplies the directory from the MP3
     * slot's own template, so passing a full path here would ask for the
     * directory twice. */
    uint32_t n = 0;
    while (path[n]) n++;
    uint32_t base = 0;
    for (uint32_t k = 0; k < n; k++)
        if (path[k] == '/' || path[k] == 0x5Cu) base = k + 1u;

    char nm[64];
    uint32_t m = 0;
    while (base + m < n && m < sizeof(nm) - 5u) { nm[m] = path[base + m]; m++; }
    while (m && nm[m - 1u] != '.') m--;
    if (!m) return;                              /* no extension, no sidecar */
    nm[m] = 'l'; nm[m + 1u] = 'r'; nm[m + 2u] = 'c'; nm[m + 3u] = 0;

    if (pl_open_try(LRC_SLOT_ID, nm, MP3_SLOT_ID)) {
        uint32_t len = lrc_read_raw();
        if (len) { lrc_parse(len); if (lrc_n) return; }
    }

    /* No sidecar, or one that parsed to nothing: try the file's own tag.
     * Sidecar FIRST and unconditionally -- it is the format anyone
     * actually produces synced lyrics in, and a file may carry both a
     * good .lrc and one of the empty USLT placeholders this library is
     * full of. Letting the tag win there would replace real lyrics with
     * nothing. */
    uint32_t tn = lrc_tag_load();
    if (tn) lrc_parse(tn);
}
