#include "cache.h"
#include "board.h"
#include "trap.h"

#ifdef BOARD_NANO
/* XTheadCmo, custom-0 (0x0b), funct12 in the imm field, rs1 = address:
 * 0x025 th.dcache.cva (clean), 0x026 th.dcache.iva (invalidate),
 * 0x027 th.dcache.civa (clean+inv), 0x01b th.sync.is (as the SDK Linux does,
 * linux_5.10/arch/riscv/mm/cacheflush.c:92-117). Emitted with .insn so no
 * -march=..._xtheadcmo is needed. */
#define CMO(imm, p) __asm__ volatile(".insn i 0x0b, 0, x0, %0, " #imm :: "r"(p) : "memory")
#define SYNC_IS() __asm__ volatile(".insn i 0x0b, 0, x0, x0, 0x01b" ::: "memory")   /* th.sync.is */

static volatile int probing;
static volatile int probe_trapped;
static int ok;
static uint8_t probe_line[CACHE_LINE] __attribute__((aligned(CACHE_LINE)));

int cache_probe_trap(struct trap_frame *f)
{
    if (!probing)
        return 0;
    probe_trapped = 1;
    f->sepc += 4;                       /* skip the 4-byte custom instruction */
    return 1;
}

void cache_init(void)
{
    probe_trapped = 0;
    probing = 1;
    CMO(0x025, probe_line);
    SYNC_IS();
    probing = 0;
    ok = !probe_trapped;
}

int cache_ok(void) { return ok; }

static inline uintptr_t lo(const void *p) { return (uintptr_t)p & ~(uintptr_t)(CACHE_LINE - 1); }

void cache_clean(const void *p, size_t n)
{
    if (!ok)
        return;
    for (uintptr_t a = lo(p), e = (uintptr_t)p + n; a < e; a += CACHE_LINE)
        CMO(0x025, a);
    SYNC_IS();
}

void cache_inval(void *p, size_t n)
{
    if (!ok)
        return;
    for (uintptr_t a = lo(p), e = (uintptr_t)p + n; a < e; a += CACHE_LINE)
        CMO(0x026, a);
    SYNC_IS();
}

void cache_flush(void *p, size_t n)
{
    if (!ok)
        return;
    for (uintptr_t a = lo(p), e = (uintptr_t)p + n; a < e; a += CACHE_LINE)
        CMO(0x027, a);
    SYNC_IS();
}
#else  /* qemu: coherent, nothing to do */
void cache_init(void) {}
int cache_ok(void) { return 0; }
void cache_clean(const void *p, size_t n) { (void)p; (void)n; }
void cache_inval(void *p, size_t n) { (void)p; (void)n; }
void cache_flush(void *p, size_t n) { (void)p; (void)n; }
int cache_probe_trap(struct trap_frame *f) { (void)f; return 0; }
#endif
