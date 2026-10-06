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
/* DMA variant (sysDMA + T-Head CMO), needs BOARD_HAS_DMA. Buffers: 64 B aligned and
 * padded to a whole line (rx is invalidated by line). Interrupts stay enabled; done_cb(rc)
 * runs from the DMA IRQ once CS is back high (rc as spi_xfer, -2/-3/-4 DMA faults). */
typedef void (*spi_done_fn)(int rc);
int  spi_dma_init(void);                   /* 0 ok; <0 no CMO / no DMAC (keep polling) */
int  spi_xfer_dma_start(const uint8_t *tx, uint8_t *rx, size_t n, spi_done_fn done_cb);
int  spi_dma_busy(void);
void spi_xfer_dma_abort(void);             /* watchdog: stop, CS high, no callback */
void spi_dump(void);
void spi_dump_rt(void);                    /* live SSI state (bring-up) */                       /* init-time register summary on UART */
#endif
