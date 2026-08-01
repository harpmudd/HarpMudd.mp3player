// =============================================================================
// Stage 1 + 2 bring-up firmware.
//
// Stage 1 (CONFIRMED WORKING on hardware 2026-07-30): VexRiscv boots from
// SD-loaded firmware and drives video + audio.
//
// Stage 2: prove APF target command 0180 -- read from an ARBITRARY OFFSET in a
// data slot. That is the capability real playback needs (pull the next few KB
// of an MP3 on demand); it has never been exercised by any core here.
//
// NOTE on 0192 (open file): it is NOT a file browser. It opens a file BY NAME
// and needs target_buffer_param_struct pointing at a BRAM region holding the
// filename/flags -- a pointer core_top.v declares but never drives. Getting a
// file into a slot is instead done declaratively: data.json slot 2 is
// required:true with no filename, so APF shows its own browser at core load.
// So this firmware only exercises 0180.
//
// Display (four rows of 32 blocks, MSB on the LEFT):
//   row0 green   flags: [0] heartbeat  [8] cmd done  [10] timeout
//                       [16] MP3 sync found  [17] read succeeded
//   row1 red     first 32 bits streamed back (expect 0xFFFB.. / 0xFFF3..)
//   row2 blue    PHASE as a left-aligned bar (3 blocks lit = phase 3),
//                plus the last error code in the low 4 bits (far right)
//   row3 yellow  free-running counter -- churns while alive
// =============================================================================

#include <stdint.h>

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

/* Streamed bytes arrive by DMA behind the CPU's back, so they MUST be read
 * through the uncached alias -- the cached window can hand back stale lines. */
#define UNCACHED    0xC0000000u
#define RX_OFF      0x00030000u
#define RX_LEN      512u

#define MP3_SLOT_ID 2u
#define CLK_HZ      50000000u
#define SAMPLE_HZ   48000u
#define CYC_PER_SMP (CLK_HZ / SAMPLE_HZ)

#define PH_TONE     1u
#define PH_READ     2u
#define PH_RETRY    3u
#define PH_OK       4u   /* read at offset 0 succeeded            */
#define PH_SEEK     5u   /* reading again at the post-ID3 offset  */
#define PH_SEEK_OK  6u   /* offset read succeeded -- STAGE 2 DONE */

static uint32_t st0, st3, cur_err;

static inline uint32_t cycles(void) { return REG(R_CYCLES); }

static void show_phase(uint32_t p)
{
    uint32_t bar = (p >= 32u) ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32u - p));
    REG(R_STAT2) = bar | (cur_err & 0xFu);
}

/* 440 Hz square wave, kept running everywhere so silence unambiguously means
 * the CPU stopped rather than "finished". */
static void tone_pump(uint32_t until_cycles)
{
    static uint32_t next_smp = 0, phase = 0, hb_div = 0;

    while ((int32_t)(cycles() - until_cycles) < 0) {
        if ((int32_t)(cycles() - next_smp) >= 0) {
            next_smp += CYC_PER_SMP;
            phase += 440u * 65536u / SAMPLE_HZ;
            int16_t s = (phase & 0x8000u) ? 6000 : -6000;
            REG(R_AUDIO) = ((uint32_t)(uint16_t)s) | ((uint32_t)(uint16_t)s << 16);

            if (++hb_div >= SAMPLE_HZ / 4u) {      /* ~2 Hz */
                hb_div = 0;
                st0 ^= 1u;
                REG(R_STAT0) = st0;
            }
        }
        REG(R_STAT3) = ++st3;
    }
}

/* Returns 1 on success. Timeout is generous: the first access to a slot makes
 * APF walk the FAT cluster chain, which the docs warn can be slow. */
static int target_read(uint32_t off, uint32_t bridge_addr, uint32_t len)
{
    REG(R_TGT_ID)  = MP3_SLOT_ID;
    REG(R_TGT_OFF) = off;
    REG(R_TGT_ADR) = bridge_addr;
    REG(R_TGT_LEN) = len;
    REG(R_TGT_GO)  = 0u;                        /* write starts command 0180 */

    uint32_t deadline = cycles() + CLK_HZ * 10u;
    for (;;) {
        uint32_t s = REG(R_TGT_GO);
        if (s & 0x2u) {                          /* done */
            cur_err = (s >> 2) & 7u;
            st0 |= (1u << 8);
            REG(R_STAT0) = st0;
            return (cur_err == 0u);
        }
        if ((int32_t)(cycles() - deadline) >= 0) {
            st0 |= (1u << 10);                   /* timed out */
            REG(R_STAT0) = st0;
            return 0;
        }
        tone_pump(cycles() + CYC_PER_SMP);
    }
}

static int scan_sync(void)
{
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)(UNCACHED + RX_OFF);
    for (uint32_t i = 0; i + 1u < RX_LEN; i++)
        if (b[i] == 0xFFu && (b[i + 1u] & 0xE0u) == 0xE0u) return 1;
    return 0;
}

/* Returns the total ID3v2 tag length, or 0 if the buffer does not start with
 * one. Size field is "syncsafe": 7 significant bits per byte. */
static uint32_t id3_len(void)
{
    volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)(UNCACHED + RX_OFF);
    if (b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    uint32_t sz = ((uint32_t)(b[6] & 0x7Fu) << 21) |
                  ((uint32_t)(b[7] & 0x7Fu) << 14) |
                  ((uint32_t)(b[8] & 0x7Fu) << 7)  |
                  ((uint32_t)(b[9] & 0x7Fu));
    return sz + 10u;
}

static void capture_rx(void)
{
    volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)(UNCACHED + RX_OFF);
    REG(R_STAT1) = w[0];
    if (scan_sync()) st0 |= (1u << 16);
    REG(R_STAT0) = st0;
}

int main(void)
{
    st0 = 0; st3 = 0; cur_err = 0;
    REG(R_STAT0) = 0; REG(R_STAT1) = 0; REG(R_STAT3) = 0;

    /* Phase 1 -- prove the basics before touching the untested path. */
    show_phase(PH_TONE);
    tone_pump(cycles() + CLK_HZ * 3u);

    /* Phase 2 -- the actual Stage 2 experiment. */
    show_phase(PH_READ);
    if (target_read(0, RX_OFF, RX_LEN)) {
        st0 |= (1u << 17);
        REG(R_STAT0) = st0;
        capture_rx();
        show_phase(PH_OK);

        /* Phase 5/6 -- the test that actually matters for streaming: read from
         * an ARBITRARY OFFSET, not just 0. Most MP3s open with a multi-KB ID3v2
         * tag, so offset 0 legitimately contains no frame sync. Skip the tag and
         * re-read there; finding sync at that offset proves random access works,
         * which is exactly what feeding the decoder will require. */
        uint32_t skip = id3_len();
        if (skip) {
            st0 |= (1u << 18);                  /* ID3 tag detected */
            REG(R_STAT0) = st0;
            REG(R_STAT1) = skip;                /* show the tag length */
            show_phase(PH_SEEK);
            if (target_read(skip, RX_OFF, RX_LEN)) {
                if (scan_sync()) st0 |= (1u << 19);   /* sync AFTER seek */
                REG(R_STAT0) = st0;
                show_phase(PH_SEEK_OK);
            }
        }
    } else {
        /* Phase 3 -- keep retrying once a second. If the slot gets a file later
         * (user loads one from the core menu) this will pick it up without a
         * reboot, and the error code stays visible in the meantime. */
        for (;;) {
            show_phase(PH_RETRY);
            tone_pump(cycles() + CLK_HZ);
            if (target_read(0, RX_OFF, RX_LEN)) {
                st0 |= (1u << 17);
                REG(R_STAT0) = st0;
                capture_rx();
                show_phase(PH_OK);
                break;
            }
        }
    }

    for (;;) tone_pump(cycles() + CLK_HZ);
    return 0;
}
