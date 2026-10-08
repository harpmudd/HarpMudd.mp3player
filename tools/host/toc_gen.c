#include "hostio.h"
static unsigned char xing_toc[100]={0,3,6,7,10,12,14,17,19,21,23,26,27,30,31,34,36,38,41,44,46,49,53,55,58,60,63,66,68,70,73,75,79,82,84,87,90,93,96,98,101,104,106,109,112,113,116,119,120,123,125,127,130,131,134,137,139,141,144,146,149,152,154,157,159,161,164,166,170,173,175,179,182,184,187,190,193,196,198,202,205,207,210,214,216,219,222,224,227,228,231,234,236,238,241,243,246,249,251,254};
static unsigned char xing_toc_ok=1;
static uint32_t track_secs=221u;
static uint32_t track_bytes=5441173u;
static uint32_t audio_start=0u;
static uint32_t xing_byte_at(uint32_t secs)
{
    if (!xing_toc_ok || !track_secs || !track_bytes) return 0;
    if (secs >= track_secs) return 0;

    uint32_t num = secs * 100u;
    uint32_t idx = num / track_secs;          /* 0..99 */
    uint32_t rem = num - idx * track_secs;    /* how far into that entry */
    if (idx > 99u) return 0;

    uint32_t a = xing_toc[idx];
    uint32_t b = (idx < 99u) ? xing_toc[idx + 1u] : 256u;
    uint32_t v = (b > a) ? a + ((b - a) * rem) / track_secs : a;

    /* 64-bit because the product overflows 32: a 50 MB file times 256 is
     * 12.8 G. Dividing first would throw away up to 255 bytes of offset,
     * which is a frame and a half. */
    uint32_t off = (uint32_t)(((uint64_t)track_bytes * v) >> 8);
    return audio_start + off;
}
int main(void){
 for(uint32_t s=0;s<221;s+=5u){hputu(xing_byte_at(s));hnl();}
 return 0;}
