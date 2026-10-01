/* Entry from OpenSBI fw_dynamic: S-mode, MMU off, a0 = hartid, a1 = FDT. */
    .section .text.start, "ax"
    .globl _start
_start:
    csrci   sstatus, 2              /* SIE=0 */
    csrw    sie, zero
    csrw    sip, zero

    /* Boot-hart lottery: first hart to swap wins, the rest park.
     * (Nano has a single hart; this keeps -smp N on QEMU safe.) */
    lla     t0, _boot_lottery
    li      t1, 1
    amoswap.w t1, t1, (t0)
    bnez    t1, park

    .option push
    .option norelax
    lla     gp, __global_pointer$
    .option pop

    lla     sp, __stack_top

    /* Zero .bss (stack lives inside it, above __bss_start..__stack_bottom
     * only; clear up to __stack_bottom so we don't wipe the live stack). */
    lla     t0, __bss_start
    lla     t1, __stack_bottom
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    /* a0 = hartid, a1 = fdt pass through untouched */
    call    main
park:
    wfi
    j       park

    .section .data
    .align 2
_boot_lottery:
    .word 0
