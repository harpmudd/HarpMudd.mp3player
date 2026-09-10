# Minimal bare-metal entry point for the Stage 0 PicoRV32 benchmark firmware.
# Zeroes every register (gp/x3 included -- picorv32 does NOT zero registers
# at reset, and this build has no real __global_pointer$ setup, so leaving
# gp as reset-time garbage caused a wild gp-relative store the very first
# time GCC used one; -mno-relax is also set at compile time as a second
# guard against that class of bug). Then sets sp, clears bss, calls main().

	.section .text._start
	.global _start
_start:
	addi x1, zero, 0
	addi x2, zero, 0
	addi x3, zero, 0
	addi x4, zero, 0
	addi x5, zero, 0
	addi x6, zero, 0
	addi x7, zero, 0
	addi x8, zero, 0
	addi x9, zero, 0
	addi x10, zero, 0
	addi x11, zero, 0
	addi x12, zero, 0
	addi x13, zero, 0
	addi x14, zero, 0
	addi x15, zero, 0
	addi x16, zero, 0
	addi x17, zero, 0
	addi x18, zero, 0
	addi x19, zero, 0
	addi x20, zero, 0
	addi x21, zero, 0
	addi x22, zero, 0
	addi x23, zero, 0
	addi x24, zero, 0
	addi x25, zero, 0
	addi x26, zero, 0
	addi x27, zero, 0
	addi x28, zero, 0
	addi x29, zero, 0
	addi x30, zero, 0
	addi x31, zero, 0

	la sp, _stack_top

	# Stack canary, 256 bytes above the bottom. The ring buffer sits directly
	# below the stack, so an overflow corrupts audio mid-DMA rather than
	# faulting -- this gives 256 bytes of runway in which to notice.
	la a0, _stack_bottom
	li a1, 0x5A5A5A5A
	sw a1, 256(a0)

#ifdef STACK_PAINT
	# MEASUREMENT BUILD ONLY. Fill the stack with a known pattern so the
	# high-water mark can be read back later: the deepest untouched word
	# tells us how much was ever actually used.
	#
	# Safe here and nowhere else -- main has not been called, so nothing is
	# on the stack yet and painting right up to sp cannot clobber a live
	# frame. Doing this from C would already have a frame to step around.
	la a0, _stack_bottom
	mv a2, sp
	li a3, 0xA5A5A5A5
5:
	bge a0, a2, 6f
	sw a3, 0(a0)
	addi a0, a0, 4
	j 5b
6:
#endif

	la a0, __bss_start
	la a1, __bss_end
1:
	bge a0, a1, 2f
	sw zero, 0(a0)
	addi a0, a0, 4
	j 1b
2:
	call main
	ebreak
loop:
	j loop
