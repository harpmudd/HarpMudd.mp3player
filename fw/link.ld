/* Firmware link map.
 * RAM is 256 KB of on-chip BRAM at 0x0000_0000 (cached). MMIO lives at
 * 0x8000_0000 and an uncached alias of this same RAM at 0xC000_0000 -- neither
 * is in the link map; firmware reaches them through volatile pointers.
 *
 * Layout, low -> high:
 *   .text/.rodata/.data/.bss
 *   heap            (Helix mallocs its 23816-byte decoder instance here)
 *   MP3 ring buffer (32 KB, RESERVED -- APF DMAs into it)
 *   stack           (16 KB)
 *
 * The ring is carved out explicitly rather than hardcoded at some address the
 * heap might one day grow into: it is a DMA target, so a collision would show
 * up as corrupted audio or a wild decoder crash, not a clean allocation failure.
 */

MEMORY {
	ram : ORIGIN = 0x00000000, LENGTH = 0x00040000   /* 256 KB */
}

ENTRY(_start)

_stack_size = 0x4000;    /* 16 KB */
/* 24 KB, down from 32.
 *
 * The ring sits ABOVE the image, so shrinking it moves _tag_start up and
 * hands 8 KB straight to the firmware -- which is what made room for a second
 * decoder's code. Both decoders' text must be resident; only their buffers
 * can swap.
 *
 * Affordable now in a way it was not when 32 KB was chosen. The ring exists to
 * absorb SD stalls, and the card measured 736 KB/s sequential against 40 KB/s
 * of MP3 demand -- an 18x margin. 24 KB is still ~0.6 s at 320 kbps, and
 * ~0.23 s at the 105 KB/s the CD-rip FLAC on the test card needs. Watch the
 * underrun flag if either ever looks marginal. */
_ring_size  = 0x6000;    /* 24 KB -- ~0.6 s at 320 kbps */
/* 2 KB, down from 4. The only 4 KB consumer was io_bench(), which is behind
 * `#if IO_BENCH` and that is 0 -- so it is compiled out and was reserving
 * space for nothing. Every live read into this buffer is 512 bytes or less
 * EXCEPT the FLAC seektable, and that one clamps itself to TAG_SIZE/18 points
 * (see flac_scan_metadata), so halving the buffer halves the clamp instead of
 * overflowing it: 227 seek points become 113. A coarser seed for the seek
 * bracket costs a probe or two on files with big seektables; it cannot make a
 * seek wrong. Buys 2 KB of image space, which is what the cassette needed. */
/* BACK TO 4 KB. Halving this to buy image space for the cassette meter was a
 * bad trade that was not visible at the time: art_need_bytes() feeds picojpeg
 * in TAG_SIZE chunks, so 2 KB DOUBLED the target reads for every cover -- 43
 * to 86 on a typical 175 KB one, 207 to 414 on the 847 KB outlier. That is
 * paid on every track load, boot included. The ROADMAP's own lever list has
 * "raise _tag_size" as the cheapest artwork win; halving it was the same lever
 * pulled backwards. */
_tag_size   = 0x1000;    /* 4 KB  -- ID3 re-probe landing zone */

SECTIONS {
	.text : {
		KEEP(*(.text._start))
		*(.text .text.*)
	} > ram

	.rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) } > ram
	.data   : { *(.data .data.*) *(.sdata .sdata.*) } > ram

	.bss : {
		__bss_start = .;
		*(.bss .bss.*) *(.sbss .sbss.*) *(COMMON)
		__bss_end = .;
	} > ram

	. = ALIGN(16);
	_heap_start = .;

	_stack_top  = ORIGIN(ram) + LENGTH(ram);
	/* A real ADDRESS symbol, so start.S can paint the region without doing
	 * arithmetic on an absolute symbol's value. Used only by the STACK_PAINT
	 * measurement build; costs nothing when that is off. */
	_stack_bottom = _stack_top - _stack_size;
	_ring_start = _stack_top - _stack_size - _ring_size;

	/* Separate landing zone for the periodic ID3 re-probe. It must NOT share
	 * the ring: the ring holds audio the decoder is actively consuming, and a
	 * probe re-reading offset 0 into it would destroy playback. Carved here
	 * for the same reason as the ring -- it is a DMA target, so an overlap
	 * would appear as corrupt audio rather than a clean allocation failure. */
	_tag_start  = _ring_start - _tag_size;
	_heap_end   = _tag_start;

	/* Fail the link rather than silently overlapping if the image ever grows
	 * past the reserved regions. */
	ASSERT(_heap_start < _tag_start, "firmware image collides with reserved DMA buffers")

	/* The gap between BSS and the DMA buffers IS the heap, and Helix mallocs
	 * 23816 bytes of it for the decoder instance -- so BSS fitting is not the
	 * same as the core working. Growing a static buffer into that space once
	 * left a 15 KB heap: BSS still fit, the linker was satisfied, the build
	 * passed, and MP3InitDecoder() would have returned 0 with no audio at
	 * all. Caught by checking the number, not by any tool.
	 *
	 * 24576 rather than 23816 exactly, to cover malloc per-chunk headers
	 * across the eight allocations and leave a little room. */
	/* 1 KB, not 24. The decoder no longer allocates from here: fw/alloc.c
	 * overrides malloc with a fixed arena, because newlib consumed the whole
	 * 33936-byte heap to serve 23816 bytes of Helix structs and refused to
	 * work at 25232 -- an overhead that could not be sized against.
	 *
	 * The guard that matters now is ARENA_BYTES in alloc.c, which is exact.
	 * This one only keeps a token heap so a stray newlib path that reaches
	 * _sbrk has somewhere to go rather than returning -1 in a corner. */
	/* 256, down from 1024. fw/alloc.c overrides malloc and free with a bump
	 * allocator over its own fixed arena, so _sbrk in sysio.c is unreachable and
	 * this region serves nothing. The guard is kept small rather than removed:
	 * if a newlib routine ever does reach _sbrk, failing the link is a better
	 * outcome than silently overwriting the DMA buffers below. */
	ASSERT(_heap_end - _heap_start >= 256, "no room left for even a token heap")
}
