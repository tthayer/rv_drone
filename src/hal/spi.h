#ifndef SPI_H
#define SPI_H
#include <stddef.h>
#include <stdint.h>

/* DW APB SSI master (SPI2), mode 3, 8-bit, Motorola, full duplex, polled. */
int spi_init(uint32_t target_hz);          /* 0 ok; leaves SCK <= target_hz (BAUDR even) */
uint32_t spi_clk_in_hz(void);              /* decoded from CLKGEN regs */
uint32_t spi_sck_hz(void);                 /* achieved */
uint32_t spi_fifo_depth(void);
/* One CS-low transaction of n bytes. 0 ok, <0 timeout, >0 FIFO error flags. */
int spi_xfer(const uint8_t *tx, uint8_t *rx, size_t n);
void spi_dump(void);                       /* init-time register summary on UART */
#endif
