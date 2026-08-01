#include <stdint.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"
#include "../common/test_mp3.h"

/* Stage 0 benchmark -- see fw_helix/main.c for the rationale behind reporting
 * instructions-retired alongside cycles. Same bounded-frame structure so the
 * two codecs are measured identically on the same test vector.
 *
 * Prints every iteration (not just samples>0) so a frame that decodes zero
 * samples due to the bit reservoir is still visible rather than silently
 * looking like a hang.
 */

#define MAX_FRAMES 3

void print_str(const char *p);
void print_dec(unsigned int val);

static short pcm_out[MINIMP3_MAX_SAMPLES_PER_FRAME];

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
	print_str("MINIMP3 BENCH START\n");

	mp3dec_t dec;
	mp3dec_init(&dec);

	const uint8_t *buf = test_mp3;
	int bytesLeft = TEST_MP3_LEN;
	int iter = 0;

	while (bytesLeft > 0 && iter < MAX_FRAMES) {
		mp3dec_frame_info_t info;

		uint32_t c0 = rdcycle();
		uint32_t i0 = rdinstret();
		int samples = mp3dec_decode_frame(&dec, buf, bytesLeft, pcm_out, &info);
		uint32_t c1 = rdcycle();
		uint32_t i1 = rdinstret();

		iter++;

		print_str("frame ");
		print_dec(iter);
		print_str(" samples ");
		print_dec((unsigned int)samples);
		print_str(" bytes ");
		print_dec((unsigned int)info.frame_bytes);
		print_str(" cycles ");
		print_dec(c1 - c0);
		print_str(" instret ");
		print_dec(i1 - i0);
		print_str("\n");

		if (info.frame_bytes == 0) break;
		buf += info.frame_bytes;
		bytesLeft -= info.frame_bytes;
	}

	print_str("BENCH DONE\n");
	return 0;
}
