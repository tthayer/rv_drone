#ifndef VEC_H
#define VEC_H

/* C906 vector unit (RVV 0.7.1 = XTheadVector). vec_init() probes once with
 * th.vsetvli, catching the illegal-instruction trap (trap.c -> vec_probe_trap).
 * If the first try traps, it sets the vector-state field in sstatus and retries:
 * first T-Head's position (bits 24:23), then the RVV 1.0 position (bits 10:9).
 * Vector registers are not saved by the trap entry: use them in main-loop
 * context only, like FP. */
struct trap_frame;
void vec_init(void);
int  vec_ok(void);                 /* vector instructions usable */
int  vec_vlen_bits(void);          /* VLEN (128 on the C906), 0 if unusable */
const char *vec_how(void);         /* how it was enabled, for the console */
int  vec_probe_trap(struct trap_frame *f);
#endif
