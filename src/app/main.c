#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "uart.h"
#include "trap.h"
#include "plic.h"
#include "timer.h"

/* Freestanding helpers; GCC may emit calls to these even with -ffreestanding. */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static void led_init(void)
{
#ifdef BOARD_LED_GPIO_BASE
    REG32(BOARD_LED_FMUX) = BOARD_LED_FMUX_GPIO;
    REG32(BOARD_LED_GPIO_BASE + 0x04) |= 1u << BOARD_LED_BIT;
#endif
}

static void led_toggle(void)
{
#ifdef BOARD_LED_GPIO_BASE
    REG32(BOARD_LED_GPIO_BASE + 0x00) ^= 1u << BOARD_LED_BIT;
#endif
}

void main(uint64_t hartid, uint64_t fdt)
{
    led_init();
    uart_puts("rv_drone hello (hart ");
    uart_put_dec(hartid);
    uart_puts(", fdt ");
    uart_put_hex(fdt);
    uart_puts(")\n");

    trap_init();
    uart_puts("m1: trap ok\n");
    plic_init();
    uart_puts("m1: plic ok\n");
    uart_enable_rx_irq();
    uart_puts("m1: uart irq ok");
    uart_dump_regs();
    uart_putc('\n');
    timer_init();
    uart_puts("m1: timer armed, sip=");
    { uint64_t v; __asm__ volatile("csrr %0, sip" : "=r"(v)); uart_put_hex(v); }
    uart_putc('\n');
    __asm__ volatile("csrs sie, %0" :: "r"((1u << 5) | (1u << 9)));  /* STIE|SEIE */
    __asm__ volatile("csrsi sstatus, 2");                            /* SIE */
    uart_puts("m1: irqs on\n");

#ifdef M1_FAULT_TEST
    (void)*(volatile uint32_t *)0;   /* load access fault -> trap dump */
#endif

    uint64_t last_sec = 0;
    for (;;) {
        __asm__ volatile("wfi");
        uint64_t t = timer_ticks;
        if (t / BOARD_TICK_HZ != last_sec) {
            last_sec = t / BOARD_TICK_HZ;
            led_toggle();
            uart_puts("tick ");
            uart_put_dec(t);
            uart_puts(" time ");
            uart_put_dec(rdtime());
            uart_puts(" claims ");
            uart_put_dec(plic_claims);
            uart_puts(" spur ");
            uart_put_dec(plic_spurious);
            uart_puts(" busy ");
            uart_put_dec(uart_busy_count);
            uart_putc('\n');
        }
        int c;
        while ((c = uart_getc_nonblock()) >= 0) {
            uart_puts("rx: '");
            uart_putc(c >= 32 && c < 127 ? (char)c : '.');
            uart_puts("' (0x");
            uart_putc("0123456789abcdef"[c >> 4]);
            uart_putc("0123456789abcdef"[c & 15]);
            uart_puts(")\n");
        }
    }
}
