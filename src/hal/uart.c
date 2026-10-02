/* Polled 16550-style UART. Baud/divisor is left as OpenSBI/FSBL set it. */
#include "uart.h"
#include "board.h"
#include "plic.h"

#define UART_THR 0   /* transmit holding */
#define UART_LSR 5   /* line status */
#define UART_RBR 0   /* receive buffer */
#define UART_IER 1   /* interrupt enable */
#define IER_ERBFI 0x01
#define LSR_DR   0x01
#define LSR_THRE 0x20

#define RX_N 64      /* power of 2; SPSC: ISR writes head, main writes tail */
static volatile uint8_t rx_buf[RX_N];
static volatile uint32_t rx_head, rx_tail;

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

static void uart_rx_isr(void)
{
    while (*reg(UART_LSR) & LSR_DR) {
        uint8_t c = *reg(UART_RBR);
        uint32_t h = rx_head;
        if (h - rx_tail < RX_N) {   /* drop on full */
            rx_buf[h % RX_N] = c;
            rx_head = h + 1;
        }
    }
}

void uart_enable_rx_irq(void)
{
    plic_register(BOARD_UART_IRQ, uart_rx_isr);
    *reg(UART_IER) |= IER_ERBFI;
}

int uart_getc_nonblock(void)
{
    uint32_t t = rx_tail;
    if (t == rx_head)
        return -1;
    int c = rx_buf[t % RX_N];
    rx_tail = t + 1;
    return c;
}
