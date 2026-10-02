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

    for (uint64_t n = 0;; n++) {
        delay_ms(500);
        led_toggle();
        delay_ms(500);
        led_toggle();
        uart_puts("heartbeat ");
        uart_put_dec(n);
        uart_putc('\n');
    }
}
