#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "uart.h"

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

static inline uint64_t rdtime(void)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

static void delay_ms(uint64_t ms)
{
    uint64_t end = rdtime() + ms * (BOARD_TIMEBASE_HZ / 1000);
    while (rdtime() < end)
        ;
}

void main(uint64_t hartid, uint64_t fdt)
{
    uart_puts("rv_drone hello (hart ");
    uart_put_dec(hartid);
    uart_puts(", fdt ");
    uart_put_hex(fdt);
    uart_puts(")\n");

    for (uint64_t n = 0;; n++) {
        delay_ms(1000);
        uart_puts("heartbeat ");
        uart_put_dec(n);
        uart_putc('\n');
    }
}
