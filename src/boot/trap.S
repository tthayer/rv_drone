/* S-mode trap entry (stvec direct). Frame is 36 dwords on the current stack:
 * [0]=sepc, [1..31]=x1..x31 (x2 slot unused), [32]=sstatus, [33]=scause,
 * [34]=stval, [35]=pad. Must match struct trap_frame in trap.h. */
#define FRAME 288

    .section .text.trap, "ax"
    .balign 4
    .globl trap_entry
trap_entry:
    addi    sp, sp, -FRAME
    .irp r, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    sd      x\r, \r*8(sp)
    .endr
    csrr    t0, sepc
    csrr    t1, sstatus
    csrr    t2, scause
    csrr    t3, stval
    sd      t0, 0(sp)
    sd      t1, 32*8(sp)
    sd      t2, 33*8(sp)
    sd      t3, 34*8(sp)
    mv      a0, sp
    call    trap_handler
    ld      t0, 0(sp)
    ld      t1, 32*8(sp)
    csrw    sepc, t0
    csrw    sstatus, t1
    .irp r, 1,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ld      x\r, \r*8(sp)
    .endr
    addi    sp, sp, FRAME
    sret
