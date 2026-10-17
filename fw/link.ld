/* Firmware link map.
 * RAM is 256 KB of on-chip BRAM at 0x0000_0000 (cached). MMIO lives at
 * 0x8000_0000 and an uncached alias of this same RAM at 0xC000_0000 -- neither
 * is in the link map; firmware reaches them through volatile pointers.
 *
 * Layout, low -> high:
 *   .text/.rodata/.data/.bss
 *   heap            (Helix mallocs its 23816-byte decoder instance here)
 *   tag buffer      (4 KB, _tag_size -- the ID3 re-probe landing zone)
 *   MP3 ring buffer (24 KB, _ring_size, RESERVED -- APF DMAs into it)
 *   stack           (9 KB, _stack_size)
 *
 * The three tail sizes above are the symbols, not prose: they have drifted
 * before (this comment said 32 KB and 16 KB long after they were 24 and 9),
 * and fw/build.sh now reports the map from the linked symbols so a stale
 * number here cannot mislead anyone about the real headroom.
 *
 * The ring is carved out explicitly rather than hardcoded at some address the
 * heap might one day grow into: it is a DMA target, so a collision would show
 * up as corrupted audio or a wild decoder crash, not a clean allocation failure.
 */

MEMORY {
	ram : ORIGIN = 0x00000000, LENGTH = 0x00040000   /* 256 KB */
}

ENTRY(_start)

/* 9 KB, down from 16 -- and the number is chosen to make overflow
 * ARITHMETICALLY IMPOSSIBLE rather than merely unlikely.
 *
 * Measured 2026-09-10 with -fstack-usage over every TU, flac.c and picojpeg.c
 * included (build.sh compiles those with their own flags, so a default
 * instrumented build misses them):
 *
 *   largest single frame anywhere   656   (flac_decode_frame)
 *   SUM of every frame in the build 8464
 *   measured high-water on hardware ~1580 (MP3 + artwork + browser session)
 *
 * 9216 exceeds 8464, so even if every function in the firmware were nested
 * inside one another simultaneously the stack still fits. That cannot happen
 * -- the MP3 and FLAC paths are mutually exclusive, siblings do not nest, the
 * UI never runs inside the decoder -- which is the point: the guarantee does
 * not depend on any of that reasoning being right.
 *
 * The 7 KB this frees goes to the image, which had 272 bytes of slack and was
 * silently deciding design questions because of it.
 *
 * WHY IT MATTERS THAT THIS IS SAFE: the stack grows DOWN and the ring buffer
 * is directly below it. An overflow would not fault -- it would quietly
 * corrupt compressed audio mid-DMA, and present as intermittent glitching
 * indistinguishable from the v1.4.0 fragment-cache stutter. `stack_ok()` in
 * player.c watches a canary 256 bytes above the bottom as a second line.
 *
 * If a function with a very large frame is ever added, or anything recurses,
 * re-run: EXTRA_CFLAGS=-fstack-usage bash fw/build.sh */
_stack_size = 0x2400;    /* 9 KB -- see above; 8464 is the hard ceiling */
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
/* 16 KB, down from 24, which was itself down from 32 when the FLAC decoder
 * needed the room. Same lever a third time, for the same reason: the ring
 * sits above the image, so shrinking it hands the space straight to the
 * firmware -- 8 KB here, taking headroom from 24,160 to 32,352.
 *
 * The ring exists ONLY to absorb SD stalls; nothing needs a whole frame
 * resident. flac_pull() draws from it incrementally and max_framesize is
 * never read anywhere, so there is no lower bound imposed by frame size, and
 * every gate that reads RING_SIZE is a proportion of it (half for the idle
 * slot and the cushion, three quarters for the resume check) rather than an
 * absolute.
 *
 * What it costs is cover. Measured: the card does 736 KB/s against 105 KB/s
 * for the CD-rip FLAC on the test card, a 7x margin.
 *
 *     320 kbps MP3   40 KB/s   0.41 s of cover
 *     CD-rip FLAC   105 KB/s   0.16 s
 *     1000 kbps     125 KB/s   0.13 s
 *
 * FALSIFIABLE, which is why it is worth trying rather than arguing about:
 * und_edges counts underruns and is already on screen in a UI_SHOW_DIAG
 * build. A soak that records zero has settled it; anything above zero puts
 * this straight back to 0x6000. Watch a dense 24-bit FLAC, which has the
 * least decode headroom and the highest byte rate at once. */
_ring_size  = 0x4000;    /* 16 KB -- ~0.41 s at 320 kbps */
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

/* THE QR EXPORT GOES LAST, IN EVERY SECTION.
 *
 * The instruction and data caches are 4 KB DIRECT-MAPPED -- one way, 128
 * lines of 32 bytes (see ways_0_* in src/fpga/rtl/VexRiscv_Full.v). With a
 * single way there is no associativity to absorb a collision: two hot regions
 * whose addresses differ by a multiple of 4 KB evict each other on every
 * single access, and whether any given pair does is decided purely by where
 * the linker put them.
 *
 * That made image growth a performance risk rather than just a space one.
 * Adding the ~10 KB QR encoder in the middle of .text shifted every function
 * after it; FLAC fell from 15% idle to 0 and the audio broke up, while MP3
 * was barely affected because its working set is tighter. It took a full
 * bisect to find, because the cause was invisible in the diff: poll_input
 * reverted cleanly and .bss shrank back, and neither helped -- the regression
 * tracked nothing but the SIZE of the image ahead of the hot code.
 *
 * Keeping the cold code behind everything else means the decode path keeps
 * the addresses it was tuned at, and anything added here is free. If a future
 * feature is big and cold, give it a section and put it here too. */
SECTIONS {
	.text : {
		KEEP(*(.text._start))
		*(EXCLUDE_FILE (*qrcodegen.o) .text .text.*)
		*(.qrtext .qrtext.*)
		*(.coldtext .coldtext.*)
		*qrcodegen.o(.text .text.*)
	} > ram

	.rodata : {
		*(EXCLUDE_FILE (*qrcodegen.o) .rodata .rodata.*)
		*(EXCLUDE_FILE (*qrcodegen.o) .srodata .srodata.*)
		*qrcodegen.o(.rodata .rodata.* .srodata .srodata.*)
	} > ram
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
