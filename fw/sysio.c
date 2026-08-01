/* Minimal newlib syscall stubs. Helix's MP3InitDecoder() mallocs its ~34 KB
 * decoder instance, so a working _sbrk is mandatory; the rest are stubs that
 * exist only to satisfy the linker. */

#include <stdint.h>
#include <errno.h>

extern char _heap_start, _heap_end;
static char *heap_ptr = &_heap_start;

void *_sbrk(int incr)
{
    char *prev = heap_ptr;
    if (heap_ptr + incr > &_heap_end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_ptr += incr;
    return prev;
}

unsigned int heap_used(void) { return (unsigned int)(heap_ptr - &_heap_start); }

int  _write(int fd, const char *buf, int len) { (void)fd; (void)buf; return len; }
int  _read(int fd, char *buf, int len)  { (void)fd; (void)buf; (void)len; return 0; }
int  _close(int fd)                     { (void)fd; return -1; }
int  _fstat(int fd, void *st)           { (void)fd; (void)st; return -1; }
int  _isatty(int fd)                    { (void)fd; return 1; }
int  _lseek(int fd, int o, int w)       { (void)fd; (void)o; (void)w; return 0; }
int  _kill(int pid, int sig)            { (void)pid; (void)sig; return -1; }
int  _getpid(void)                      { return 1; }
void _exit(int code)                    { (void)code; for (;;) { } }
