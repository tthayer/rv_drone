#ifndef UART_H
#define UART_H
#include <stdint.h>

void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex(uint64_t v);   /* "0x" + 16 digits */
void uart_put_dec(uint64_t v);

void uart_enable_rx_irq(void);   /* IER.ERBFI + PLIC registration */
int uart_getc_nonblock(void);    /* -1 if empty */

#endif
