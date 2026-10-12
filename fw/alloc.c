/* A tight arena allocator, replacing newlib's malloc for the decoder.
 *
 * WHY, measured rather than assumed: Helix's eight allocations sum to 23816
 * bytes, but newlib's malloc consumed the ENTIRE 33936-byte heap to serve them
 * -- H33936/33936 on hardware -- and refused to work at all at 25232, where
 * MP3InitDecoder() returned 0 and every track showed LOAD FAILED. So newlib's
 * overhead on this workload is somewhere between 1.4 KB and 10 KB, it was
 * never possible to size a static buffer against it, and two attempts to do so
 * were wrong in opposite directions.
 *
 * This makes the cost exact. A bump pointer over a fixed arena: no headers, no
 * free lists, no padding beyond 8-byte alignment.
 *
 * SAFE FOR THIS WORKLOAD SPECIFICALLY, and only this one. Helix allocates all
 * eight blocks together and frees all eight together (MP3FreeDecoder), and
 * nothing else in the firmware allocates at all -- checked. So a reference
 * count that resets the arena when it reaches zero reclaims everything on each
 * decoder teardown, which is the only lifetime that exists here. If anything
 * else ever calls malloc and holds it, the count never reaches zero and this
 * degrades to a pure bump allocator: it stops reclaiming, but it never hands
 * out memory that is still in use.
 *
 * Helix itself is untouched. buffers.c invites you to replace its allocator by
 * editing it -- but it is vendored unmodified under RPSL and the README says
 * so, which is a licensing claim, not a preference. Overriding malloc/free at
 * link time achieves the same thing and keeps that true.
 */

#include <stddef.h>
#include <stdint.h>

/* 23816 for the eight blocks, plus 8-byte alignment slack on each, plus room
 * for a future decoder that is larger. FLAC's block buffer is ~16 KB against
 * Helix's 8708-byte SubbandInfo, so a swap needs headroom here, not a second
 * arena -- that is the whole point of sizing it once. */
/* 24576, sized from the MEASURED peak rather than a guess: A23824/26624 on
 * hardware, so Helix's real high-water mark is 23824 and this leaves 752.
 * The FLAC decoder needs less -- one blocksize of int32 (18432 for the
 * 4608-sample blocks on the test card) plus its state -- so the two swap
 * inside this without either being the binding case.
 *
 * It was 26624; the 2048 came back because the FLAC decoder's CODE has to be
 * resident alongside Helix's, and total RAM is image + heap, not just data. */
/* Sized by FLAC, not by Helix, since 2026-10-12.
 *
 * Helix needs 23,816 bytes and for a long time that set this number, with
 * 24,576 being the next round figure above it. But the FLAC channel-0 buffer
 * is this same arena -- the two decoders never run together -- so
 * ARENA_BYTES/4 silently became "the largest FLAC block the core accepts",
 * and at 24,576 that was 6144. Nothing about 6144 had anything to do with
 * FLAC; it was the MP3 decoder's working set, divided by four.
 *
 * 32,768 makes the cap 8192, which is the block size hi-res encoders
 * actually use. Measured on hardware before changing it: a real 8192-block
 * file ran at L0/E330/X1ms/T63ms against two known-good 4608-block files at
 * L0/E211/X2ms/T80ms and L0/E577/X2ms/T39ms -- inside the baseline band on
 * every field, with the SHORTEST worst-case gap of the three. The big block
 * costs nothing.
 *
 * Still overridable, e.g. -DARENA_BYTES=65536u for a 16384 cap, which does
 * not currently fit in the headroom. */
#ifndef ARENA_BYTES
#define ARENA_BYTES 32768u
#endif

static uint8_t  arena[ARENA_BYTES] __attribute__((aligned(8)));
static uint32_t arena_next;
static uint32_t arena_live;      /* outstanding allocations */

void *malloc(size_t n)
{
    uint32_t sz = ((uint32_t)n + 7u) & ~7u;          /* 8-byte aligned */
    if (!sz) sz = 8u;
    if (arena_next + sz > ARENA_BYTES) return (void *)0;

    void *p = &arena[arena_next];
    arena_next += sz;
    arena_live++;
    return p;
}

void free(void *p)
{
    if (!p) return;
    if (arena_live) arena_live--;
    /* Everything went at once, so everything comes back at once. */
    if (!arena_live) arena_next = 0;
}

void *calloc(size_t a, size_t b)
{
    uint32_t n = (uint32_t)a * (uint32_t)b;
    uint8_t *p = (uint8_t *)malloc(n);
    if (p) for (uint32_t i = 0; i < n; i++) p[i] = 0u;
    return p;
}

/* realloc exists only so a stray reference cannot pull newlib's allocator back
 * in beside this one. Nothing here calls it. */
void *realloc(void *p, size_t n) { (void)p; (void)n; return (void *)0; }

unsigned int arena_limit(void) { return ARENA_BYTES; }
