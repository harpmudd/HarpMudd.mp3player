/* Stage 0 benchmark firmware linker script.
 * Simulation-only memory size (1MB) -- not the real Pocket BRAM budget,
 * just generous enough to hold codec code/tables/test-vector/scratch buffers
 * while we measure decode cycle counts. */

MEMORY {
	ram : ORIGIN = 0x00000000, LENGTH = 0x00100000
}

ENTRY(_start)

SECTIONS {
	.text : {
		*(.text._start)
		*(.text .text.*)
	} > ram

	.rodata : {
		*(.rodata .rodata.*)
	} > ram

	.data : {
		*(.data .data.*)
	} > ram

	.bss : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		__bss_end = .;
	} > ram

	. = ALIGN(16);
	_heap_start = .;
	. = ORIGIN(ram) + LENGTH(ram) - 0x4000; /* reserve top 16KB for stack */
	_heap_end = .;
	_stack_top = ORIGIN(ram) + LENGTH(ram);
}
