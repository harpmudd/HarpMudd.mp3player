/* Validates tools/rv32sim.py before any conclusion is drawn from it.
 *
 * An emulator written to debug a subtle bug is itself a place for subtle bugs,
 * and a wrong one would produce exactly what has already gone wrong twice: a
 * confident, false answer. So this exercises the operations fw/flac.c actually
 * leans on -- 64-bit shifts and masks for the bit reservoir, unsigned compares,
 * multiply and divide including the signed edge cases GCC lowers to library
 * sequences -- and prints results the caller checks against Python.
 *
 * It deliberately includes the arithmetic the post-seek clock uses:
 * frame_number * blocksize / rate in 64-bit.
 */
#include "hostio.h"

volatile uint32_t sink32;
volatile uint64_t sink64;

int main(void)
{
    /* 64-bit reservoir behaviour: shift in, read a window, shift out. */
    uint64_t acc = 0;
    uint32_t cnt = 0;
    const uint8_t bytes[8] = { 0xFF, 0xF8, 0x69, 0x18, 0x00, 0xA3, 0x5C, 0x11 };
    for (int i = 0; i < 8; i++) { acc = (acc << 8) | bytes[i]; cnt += 8; }
    hputs("acc_hi "); hputu((uint32_t)(acc >> 32)); hnl();
    hputs("acc_lo "); hputu((uint32_t)(acc & 0xFFFFFFFFu)); hnl();

    /* Reading a 14-bit window the way the sync scan does. */
    hputs("win14 "); hputu((uint32_t)((acc >> (cnt - 14u)) & 0x3FFFu)); hnl();

    /* Leading-zero count via the same trick the unary decoder uses. */
    uint32_t w = 0x00018000u, lead = 0;
    while (lead < 32u && !(w & 0x80000000u)) { w <<= 1; lead++; }
    hputs("clz "); hputu(lead); hnl();

    /* Signed division edge cases, lowered to __divsi3 style sequences. */
    int32_t a = -2147483647 - 1, b = -1;
    volatile int32_t vb = 3;
    hputs("div1 "); hputu((uint32_t)(a / vb)); hnl();
    hputs("rem1 "); hputu((uint32_t)(a % vb)); hnl();
    vb = b;
    hputs("div2 "); hputu((uint32_t)(a / vb)); hnl();

    /* Unsigned 64-bit multiply and divide -- the post-seek clock arithmetic. */
    uint64_t fn = 2500u, blk = 4608u, rate = 44100u;
    uint64_t smp = fn * blk;
    hputs("smp "); hputu64(smp); hnl();
    hputs("pos "); hputu((uint32_t)(smp / rate)); hnl();

    uint64_t big = 36864000ull;
    hputs("pos96 "); hputu((uint32_t)(big / 96000u)); hnl();

    /* Signed right shift of a negative, which the LPC path relies on. */
    int32_t neg = -12345;
    volatile int sh = 3;
    hputs("sar "); hputu((uint32_t)(neg >> sh)); hnl();

    /* Unsigned compare that a signed compare would get wrong. */
    uint32_t u1 = 0x80000000u, u2 = 0x7FFFFFFFu;
    hputs("ucmp "); hputu(u1 > u2 ? 1u : 0u); hnl();

    /* Byte and halfword loads, signed and unsigned. */
    const int8_t  sb8 = -5;
    const int16_t s16 = -300;
    volatile const int8_t  *p8  = &sb8;
    volatile const int16_t *p16 = &s16;
    hputs("sb "); hputu((uint32_t)(int32_t)*p8); hnl();
    hputs("sh "); hputu((uint32_t)(int32_t)*p16); hnl();

    hputs("DONE"); hnl();
    return 0;
}
