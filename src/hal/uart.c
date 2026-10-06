/* Polled 16550-style UART. Baud/divisor is left as OpenSBI/FSBL set it. */
#include "uart.h"
#include "board.h"
#include "plic.h"

#define UART_THR 0   /* transmit holding */
#define UART_LSR 5   /* line status */
#define UART_RBR 0   /* receive buffer */
#define UART_IER 1   /* interrupt enable */
#define UART_IIR 2   /* interrupt identity (read) */
#define UART_USR 31  /* DW: UART status; reading clears busy-detect */
#define IIR_BUSY 0x7
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

/* TX: polled until uart_async_tx() is on; then a ring drained from the 1 kHz
 * tick, at most 16 bytes per tick into the (empty) TX FIFO: 16 kB/s against the
 * 11.5 kB/s line, so printing never stalls the caller. Single producer
 * (non-ISR code), single consumer (tick). Full ring: drop and count. */
#define TX_N 4096
static volatile uint8_t tx_buf[TX_N];
static volatile uint32_t tx_head, tx_tail;
static volatile int tx_async;
uint32_t uart_tx_dropped;

static void putc_raw(uint8_t c)
{
    while (!(*reg(UART_LSR) & LSR_THRE))
        ;
    *reg(UART_THR) = c;
}

static void putc_one(uint8_t c)
{
    if (!tx_async) {
        putc_raw(c);
        return;
    }
    uint32_t h = tx_head;
    if (h - tx_tail >= TX_N) {
        uart_tx_dropped++;
        return;
    }
    tx_buf[h % TX_N] = c;
    tx_head = h + 1;
}

void uart_putc(char c)
{
    if (c == '\n')
        putc_one('\r');
    putc_one((uint8_t)c);
}

void uart_async_tx(int on) { tx_async = on; }

void uart_tx_drain(void)
{
    if (!(*reg(UART_LSR) & LSR_THRE))           /* FIFO not yet empty */
        return;
    uint32_t t = tx_tail;
    for (int n = 0; n < 16 && t != tx_head; n++, t++)
        *reg(UART_THR) = tx_buf[t % TX_N];
    tx_tail = t;
}

/* Fault path: drop back to polled output and flush what is queued. */
void uart_sync(void)
{
    tx_async = 0;
    while (tx_tail != tx_head) {
        putc_raw(tx_buf[tx_tail % TX_N]);
        tx_tail = tx_tail + 1;
    }
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

uint32_t uart_isr_count, uart_busy_count;

static void uart_rx_isr(void)
{
    uart_isr_count++;
#ifdef BOARD_UART_DW
    if ((*reg(UART_IIR) & 0xf) == IIR_BUSY) {   /* DW busy-detect: clear via USR */
        (void)*reg(UART_USR);
        uart_busy_count++;
    }
#endif
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
#ifdef BOARD_UART_DW
    (void)*reg(UART_USR);                       /* clear stale busy-detect */
#endif
    (void)*reg(UART_IIR);
    while (*reg(UART_LSR) & LSR_DR)
        (void)*reg(UART_RBR);
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

void uart_dump_regs(void)
{
    uart_puts(" ier=");  uart_put_hex(*reg(UART_IER));
    uart_puts(" iir=");  uart_put_hex(*reg(UART_IIR));
    uart_puts(" lsr=");  uart_put_hex(*reg(UART_LSR));
#ifdef BOARD_UART_DW
    uart_puts(" usr=");  uart_put_hex(*reg(UART_USR));
#endif
}
