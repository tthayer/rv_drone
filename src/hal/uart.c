/* Polled 16550-style UART. Baud/divisor is left as OpenSBI/FSBL set it. */
#include "uart.h"
#include "board.h"

#define UART_THR 0   /* transmit holding */
#define UART_LSR 5   /* line status */
#define LSR_THRE 0x20

static inline volatile uint8_t *reg(unsigned n)
{
    return (volatile uint8_t *)(BOARD_UART_BASE + ((uintptr_t)n << BOARD_UART_SHIFT));
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');
    while (!(*reg(UART_LSR) & LSR_THRE))
        ;
    *reg(UART_THR) = (uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

void uart_put_hex(uint64_t v)
{
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        uart_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

void uart_put_dec(uint64_t v)
{
    char buf[21];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        uart_putc(buf[--n]);
}
