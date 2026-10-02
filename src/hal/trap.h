#ifndef TRAP_H
#define TRAP_H
#include <stdint.h>

/* Layout shared with src/boot/trap.S (36 dwords). */
struct trap_frame {
    uint64_t sepc;
    uint64_t x[31];      /* x[i-1] = xi; x[1] (sp) unused */
    uint64_t sstatus, scause, stval, pad;
};

extern void trap_entry(void);
void trap_init(void);                    /* stvec = trap_entry (direct) */
void trap_handler(struct trap_frame *f); /* called from trap_entry */

#endif
