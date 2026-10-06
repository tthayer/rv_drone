#ifndef CACHE_H
#define CACHE_H
#include <stddef.h>
#include <stdint.h>

/* T-Head C906 data-cache maintenance for DMA (no MMU, VA == PA, 64 B lines).
 * Uses the T-Head custom CMO instructions from S-mode: needs mxstatus.THEADISAEE,
 * which the vendor FSBL sets (bl2_entrypoint.S: mxstatus = 0xc0638000) and
 * OpenSBI v1.8.1 leaves alone. cache_init() probes once: an illegal-instruction
 * trap is swallowed (trap.c -> cache_probe_trap) and cache_ok() reports 0.
 * Ranges are rounded OUT to whole lines: keep DMA buffers line-aligned and
 * line-padded or neighbours in the same line get invalidated/cleaned too. */
#define CACHE_LINE 64u
#define CACHE_ROUND(n) (((n) + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u))

struct trap_frame;
void cache_init(void);
int  cache_ok(void);                              /* CMO usable */
void cache_clean(const void *p, size_t n);        /* write back (before device reads) */
void cache_inval(void *p, size_t n);              /* discard (after device writes) */
void cache_flush(void *p, size_t n);              /* write back + invalidate */
int  cache_probe_trap(struct trap_frame *f);      /* illegal-insn hook, 1 = handled */
#endif
