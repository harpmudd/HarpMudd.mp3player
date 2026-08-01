/* Smoke test + CPU capability probe.
 *
 * Also probes whether the CPU implements the user-mode rdcycle/rdinstret CSRs
 * (0xC00/0xC02). The benchmark firmware depends on them; VexRiscv advertises
 * only a "light subset" of machine CSRs, so verify cheaply here rather than
 * discovering it after a multi-minute benchmark run. Two successive reads must
 * differ and be non-zero -- a stuck 0 means the CSR is not implemented.
 */

#include <stdint.h>

void print_str(const char *p);
void print_dec(unsigned int val);
void sim_finish(void);

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
	print_str("HELLO FROM SOFTCORE\n");
	print_dec(12345);
	print_str("\n");

	uint32_t c0 = rdcycle();
	uint32_t i0 = rdinstret();
	for (volatile int i = 0; i < 100; i++) { }
	uint32_t c1 = rdcycle();
	uint32_t i1 = rdinstret();

	print_str("CSR cycles ");
	print_dec(c1 - c0);
	print_str(" instret ");
	print_dec(i1 - i0);
	print_str("\n");

	if (c1 == c0 || i1 == i0)
		print_str("WARNING: counters not advancing (CSR unsupported?)\n");
	else
		print_str("CSR COUNTERS OK\n");

	print_str("DONE\n");
	sim_finish();
	return 0;
}
