#include <stdint.h>
#include "mp3dec.h"
#ifdef USE_VECTOR_128
#include "../common/test_mp3_128.h"   /* 128 kbps / 44.1 kHz -- typical */
#else
#include "../common/test_mp3.h"       /* 320 kbps / 48 kHz  -- worst case */
#endif

/* Stage 0 benchmark: decode a bounded number of frames and report, per frame,
 * the Helix error code, elapsed cycles, and (crucially) instructions retired.
 *
 * instret is the decision-grade metric: cycles are specific to picorv32's ~4 CPI
 * and this testbench's memory model, but instructions-retired is a property of
 * the codec itself. Required clock for ANY candidate core is then:
 *     clock = instret_per_frame * CPI_of_core / frame_period
 * which lets us evaluate a pipelined core without re-running the whole harness.
 *
 * NOTE: frame 1 of an MP3 normally returns ERR_MP3_INDATA_UNDERFLOW (-9) because
 * main_data_begin references bit-reservoir bytes that precede the file. That frame
 * is cheap and must NOT be counted as representative of steady-state decode cost.
 */

#ifndef MAX_FRAMES
#define MAX_FRAMES 3
#endif

void print_str(const char *p);
void print_dec(unsigned int val);
void print_hex(unsigned int val, int digits);
unsigned int heap_used(void);
void sim_finish(void);

static void print_int(int v) {
	if (v < 0) { print_str("-"); print_dec((unsigned int)(-v)); }
	else print_dec((unsigned int)v);
}

static short pcm_out[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

static inline uint32_t rdcycle(void) {
	uint32_t c;
	__asm__ volatile ("rdcycle %0" : "=r"(c));
	return c;
}

static inline uint32_t rdinstret(void) {
	uint32_t c;
	__asm__ volatile ("rdinstret %0" : "=r"(c));
	return c;
}

int main(void) {
	print_str("HELIX BENCH START\n");

	HMP3Decoder dec = MP3InitDecoder();
	if (!dec) {
		print_str("INIT FAILED\n");
		return -2;
	}
	print_str("HEAP_AFTER_INIT ");
	print_dec(heap_used());
	print_str("\n");

	const unsigned char *buf = test_mp3;
	int bytesLeft = TEST_MP3_LEN;
	int decoded = 0;

	while (bytesLeft > 32 && decoded < MAX_FRAMES) {
		int offset = MP3FindSyncWord((unsigned char *)buf, bytesLeft);
		if (offset < 0) { print_str("NO SYNC\n"); break; }
		buf += offset;
		bytesLeft -= offset;

		const unsigned char *inbuf = buf;
		int startLeft = bytesLeft;

		uint32_t c0 = rdcycle();
		uint32_t i0 = rdinstret();
		int err = MP3Decode(dec, (unsigned char **)&inbuf, &bytesLeft, pcm_out, 0);
		uint32_t c1 = rdcycle();
		uint32_t i1 = rdinstret();

		int consumed = startLeft - bytesLeft;
		decoded++;

		print_str("frame ");
		print_dec(decoded);
		print_str(" err ");
		print_int(err);
		print_str(" consumed ");
		print_dec(consumed);
		print_str(" cycles ");
		print_dec(c1 - c0);
		print_str(" instret ");
		print_dec(i1 - i0);
		print_str("\n");

		if (err != 0 && consumed <= 0) { buf += 1; bytesLeft -= 1; continue; }
		buf = inbuf;
	}

	MP3FreeDecoder(dec);
	print_str("BENCH DONE\n");
	sim_finish();
	return 0;
}
