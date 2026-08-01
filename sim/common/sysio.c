#include <stdint.h>
#include <stddef.h>
#include <errno.h>

/* MMIO MUST live at >= 0x8000_0000. VexRiscv decides cacheability purely on
 * physical address bit 31 (isIoAccess -> stageB_bypassCache), so any peripheral
 * below that goes through the D-cache and a polled status register would return
 * a stale value forever. RAM stays at 0x0000_0000 so it IS cached -- that is
 * what gives the measured 1.65 CPI. Keep sim and hardware on the same map. */
#define OUTPORT 0x80000000
#define SIMEXIT 0x80000004

static void out_chr(char ch) {
	*((volatile uint32_t *)OUTPORT) = (uint32_t)(unsigned char)ch;
}

/* Portable end-of-simulation signal. picorv32 traps on ebreak, but VexRiscv
 * vectors ebreak to its trap handler instead, so the testbench needs an
 * explicit store to know the run is over. */
void sim_finish(void) {
	*((volatile uint32_t *)SIMEXIT) = 1;
}

void print_str(const char *p) {
	while (*p) out_chr(*p++);
}

void print_dec(unsigned int val) {
	char buf[10];
	int n = 0;
	if (val == 0) buf[n++] = '0';
	while (val) { buf[n++] = '0' + (val % 10); val /= 10; }
	while (n--) out_chr(buf[n]);
}

void print_hex(unsigned int val, int digits) {
	for (int i = (4 * digits) - 4; i >= 0; i -= 4)
		out_chr("0123456789ABCDEF"[(val >> i) & 0xF]);
}

extern char _heap_start, _heap_end;
static char *heap_ptr = &_heap_start;

/* Bytes of heap handed out so far -- used to measure the Helix decoder
 * instance's real footprint rather than estimating it. */
unsigned int heap_used(void) {
	return (unsigned int)(heap_ptr - &_heap_start);
}

void *_sbrk(int incr) {
	char *prev = heap_ptr;
	if (heap_ptr + incr > &_heap_end) {
		errno = ENOMEM;
		return (void *)-1;
	}
	heap_ptr += incr;
	return prev;
}

int _write(int fd, const char *buf, int len) {
	(void)fd;
	for (int i = 0; i < len; i++) out_chr(buf[i]);
	return len;
}

int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, void *st) { (void)fd; (void)st; return -1; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
void _exit(int code) { (void)code; while (1) __asm__ volatile("ebreak"); }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }
