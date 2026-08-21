/* Freestanding console + file access for harnesses run under tools/rv32sim.py.
 *
 * No libc: the harness is built -nostdlib so that the only C being exercised
 * is the firmware's own. memset/memcpy are provided here because GCC emits
 * calls to them regardless of -ffreestanding. */
#ifndef HOSTIO_H
#define HOSTIO_H

#include <stdint.h>

#define MMIO_PUTC   (*(volatile uint32_t *)0xF0000000u)
#define MMIO_EXIT   (*(volatile uint32_t *)0xF0000004u)
#define MMIO_DST    (*(volatile uint32_t *)0xF0000010u)
#define MMIO_SRC    (*(volatile uint32_t *)0xF0000014u)
#define MMIO_LEN    (*(volatile uint32_t *)0xF0000018u)
#define MMIO_SIZE   (*(volatile uint32_t *)0xF000001Cu)

static void hputc(char c) { MMIO_PUTC = (uint32_t)(uint8_t)c; }

static void hputs(const char *s) { while (*s) hputc(*s++); }

static void hputu(uint32_t v)
{
    char b[12];
    int  n = 0;
    if (!v) { hputc('0'); return; }
    while (v) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) hputc(b[--n]);
}

static void hputu64(uint64_t v)
{
    char b[24];
    int  n = 0;
    if (!v) { hputc('0'); return; }
    while (v) { b[n++] = (char)('0' + (uint32_t)(v % 10u)); v /= 10u; }
    while (n) hputc(b[--n]);
}

static void hputx(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    hputs("0x");
    for (int i = 28; i >= 0; i -= 4) hputc(d[(v >> i) & 0xFu]);
}

static void hnl(void) { hputc('\n'); }

static void hexit(int code) { MMIO_EXIT = (uint32_t)code; for (;;) { } }

/* Copies from the data file the simulator was given. Returns bytes copied. */
static uint32_t hread(uint32_t off, void *dst, uint32_t len)
{
    MMIO_DST = (uint32_t)dst;
    MMIO_SRC = off;
    MMIO_LEN = len;
    return MMIO_LEN;
}

static uint32_t hfilesize(void) { return MMIO_SIZE; }

void *memset(void *d, int c, unsigned n)
{
    uint8_t *p = (uint8_t *)d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    uint8_t *p = (uint8_t *)d;
    const uint8_t *q = (const uint8_t *)s;
    while (n--) *p++ = *q++;
    return d;
}

#endif
