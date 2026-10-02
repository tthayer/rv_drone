/* Trap dispatcher. Interrupts stay off inside (SIE cleared by trap entry). */
#include "trap.h"
#include "uart.h"
#include "timer.h"
#include "plic.h"

#define CAUSE_INT   (1ull << 63)
#define IRQ_S_TIMER 5
#define IRQ_S_EXT   9

void trap_init(void)
{
    __asm__ volatile("csrw stvec, %0" :: "r"((uintptr_t)trap_entry));
}

static void fatal(const struct trap_frame *f)
{
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
    fatal(f);
}
