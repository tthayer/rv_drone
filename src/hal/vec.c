#include "vec.h"
#include "board.h"
#include "trap.h"

#ifdef BOARD_NANO
static volatile int probing, trapped;
static int ok, vlen;
static const char *how = "not probed";

int vec_probe_trap(struct trap_frame *f)
{
    if (!probing)
        return 0;
    trapped = 1;
    f->sepc += 4;                       /* skip th.vsetvli */
    return 1;
}

/* vl for e8/m1 with a huge AVL = VLMAX = VLEN / 8. */
static int try_vsetvli(void)
{
    unsigned long vl = 0;
    trapped = 0;
    probing = 1;
    __asm__ volatile("li t0, 4096\n\tth.vsetvli %0, t0, e8, m1" : "=r"(vl) :: "t0");
    probing = 0;
    if (trapped)
        return 0;
    vlen = (int)vl * 8;
    return 1;
}

void vec_init(void)
{
    if (try_vsetvli()) {
        ok = 1;
        how = "already on";
        return;
    }
    __asm__ volatile("csrs sstatus, %0" :: "r"(1ul << 23));   /* T-Head VS = Initial */
    if (try_vsetvli()) {
        ok = 1;
        how = "sstatus.VS[24:23]";
        return;
    }
    __asm__ volatile("csrs sstatus, %0" :: "r"(1ul << 9));    /* RVV 1.0 VS = Initial */
    if (try_vsetvli()) {
        ok = 1;
        how = "sstatus.VS[10:9]";
        return;
    }
    ok = 0;
    vlen = 0;
    how = "traps: unavailable";
}

int vec_ok(void) { return ok; }
int vec_vlen_bits(void) { return vlen; }
const char *vec_how(void) { return how; }
#else
void vec_init(void) {}
int vec_ok(void) { return 0; }
int vec_vlen_bits(void) { return 0; }
const char *vec_how(void) { return "n/a"; }
int vec_probe_trap(struct trap_frame *f) { (void)f; return 0; }
#endif
