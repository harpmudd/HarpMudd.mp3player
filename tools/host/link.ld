/* Flat layout for the rv32sim harness. Nothing here matches the real target
 * memory map and nothing needs to: the point is to run fw/flac.c, which is
 * portable C with no firmware dependencies. */
ENTRY(_start)

MEMORY
{
    ram : ORIGIN = 0x00001000, LENGTH = 0x007F0000
}

SECTIONS
{
    .text : { *(.text._start) *(.text*) } > ram
    .rodata : { *(.rodata*) *(.srodata*) } > ram
    .data : { *(.data*) *(.sdata*) } > ram
    .bss : {
        _bss_start = .;
        *(.bss*) *(.sbss*) *(COMMON)
        _bss_end = .;
    } > ram
    . = ALIGN(16);
    _heap_start = .;
    _stack_top = ORIGIN(ram) + LENGTH(ram) - 16;
}
