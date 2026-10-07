/* Trap dispatcher. Interrupts stay off inside (SIE cleared by trap entry). */
#include "trap.h"
#include "vec.h"
#include "cpuclk.h"
#include "uart.h"
#include "timer.h"
#include "plic.h"
#include "cache.h"

#define CAUSE_INT   (1ull << 63)
#define IRQ_S_TIMER 5
#define IRQ_S_EXT   9
#define EXC_ILLEGAL 2

void trap_init(void)
{
    __asm__ volatile("csrw stvec, %0" :: "r"((uintptr_t)trap_entry));
}

static void fatal(const struct trap_frame *f)
{
    uart_sync();
    uart_puts("\n*** TRAP scause=");
    uart_put_hex(f->scause);
    uart_puts(" sepc=");
    uart_put_hex(f->sepc);
    uart_puts(" stval=");
    uart_put_hex(f->stval);
    uart_puts("\nhalted\n");
    for (;;)
        __asm__ volatile("wfi");
}

void trap_handler(struct trap_frame *f)
{
    if (f->scause & CAUSE_INT) {
        switch (f->scause & ~CAUSE_INT) {
        case IRQ_S_TIMER: timer_handler(); return;
        case IRQ_S_EXT:   plic_dispatch(); return;
        }
    }
    else if (f->scause == EXC_ILLEGAL && cache_probe_trap(f))
        return;                          /* T-Head CMO not enabled: probe in cache_init() */
    else if (f->scause == EXC_ILLEGAL && vec_probe_trap(f))
        return;                          /* vector unit off: probe in vec_init() */
    else if (f->scause == EXC_ILLEGAL && cpuclk_probe_trap(f))
        return;                          /* rdcycle not allowed in S-mode: cpuclk_measure_mhz() */
    fatal(f);
}
