/* Minimal entry for rv32sim harnesses: set the stack, clear bss, call main. */
    .section .text._start
    .globl _start
_start:
    la   sp, _stack_top
    la   t0, _bss_start
    la   t1, _bss_end
1:  bgeu t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b
2:  call main
    li   t0, 0xF0000004
    sw   a0, 0(t0)
3:  j    3b
