/* C906L image: the FSBL releases the little core at 0x83F40000; park it. */
    .section .text
    .globl _start
_start:
    csrw mie, zero
1:  wfi
    j 1b
