/* Run the VENDORED qrcodegen under tools/rv32sim.py and print the matrix.
 *
 * The point is to prove the encoder on the TARGET toolchain -- same compiler,
 * same -Os, same 32-bit RISC-V -- rather than on the host, and to render the
 * result at the size the Pocket would actually draw so a phone can be pointed
 * at it before a line of UI exists.
 *
 * Payload comes in over the simulator's blob DMA, so the scrobble list can be
 * edited in Python without touching this file.
 */
#include "hostio.h"
#include "qrcodegen.h"

#define INSTR (*(volatile uint32_t *)0xF0000020u)

/* Freestanding: the encoder wants a few libc functions. The firmware already
 * links memcpy/memmove/memset; these are the rest. strchr and strlen serve
 * only the alphanumeric path, which encodeBinary never reaches -- they are
 * here so the HARNESS links, and --gc-sections drops them in the real build. */
int abs(int v)            { return v < 0 ? -v : v; }
long labs(long v)         { return v < 0 ? -v : v; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d; const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

/* Version 23 is the biggest that fits 360 px at 3 px a module: 109 modules,
 * module on the Pocket's panel. The buffers are the real firmware cost. */
#define QR_MAXVER 19

static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAXVER)];
static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAXVER)];
static uint8_t payload[2048];

int main(void)
{
    MMIO_DST = (uint32_t)(uintptr_t)payload;
    MMIO_SRC = 0;
    MMIO_LEN = MMIO_SIZE;
    uint32_t n = MMIO_LEN;
    if (n > sizeof(payload)) n = sizeof(payload);

    for (uint32_t i = 0; i < n; i++) tmp[i] = payload[i];

    uint32_t t0 = INSTR;
    bool ok = qrcodegen_encodeBinary(tmp, (size_t)n, qr,
                                     qrcodegen_Ecc_LOW,
                                     /* PINNED at both ends, as player.c
                                      * pins it: letting the version float
                                      * down for a short session would make
                                      * the sample a different size from the
                                      * thing the Pocket actually draws. */
                                     QR_MAXVER, QR_MAXVER,
                                     /* The FIXED mask and no ECC boost, which
                                      * is what player.c does -- AUTO costs
                                      * 1282 ms on the target against 145, and
                                      * a sample rendered through a different
                                      * mask would not be the thing the Pocket
                                      * actually draws. */
                                     qrcodegen_Mask_0, false);
    uint32_t t1 = INSTR;

    if (!ok) { hputs("FAIL"); hnl(); return 0; }

    int size = qrcodegen_getSize(qr);
    hputs("SIZE "); hputu((unsigned)size);
    hputs(" BYTES "); hputu(n);
    hputs(" INSTR "); hputu(t1 - t0);
    hnl();
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++)
            hputc(qrcodegen_getModule(qr, x, y) ? '#' : '.');
        hnl();
    }
    return 0;
}
