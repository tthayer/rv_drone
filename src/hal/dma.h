#ifndef DMA_H
#define DMA_H
#include <stddef.h>
#include <stdint.h>

/* System DMA (SG2002 "sysDMA" = Synopsys DW_axi_dmac, 8 channels, 64-bit regs).
 * Single-block transfers with hardware handshaking, 8-bit beats, DMAC flow
 * control. One interrupt (PLIC) for the whole controller; per-channel callbacks
 * run from it. Boards without BOARD_HAS_DMA get stubs that fail with -1. */
#define DMA_MAX_BLOCK     1024u        /* items per block (vendor dts block_size) */

#define DMA_ST_DONE       (1u << 1)    /* DMA transfer done */
#define DMA_ST_ERR        0x17fe0u     /* any CHx_INTSTATUS error bit (5-14, 16) */

/* ch: channel number; st: CHx_INTSTATUS snapshot (DMA_ST_DONE / DMA_ST_ERR bits). */
typedef void (*dma_fn)(unsigned ch, uint32_t st);

int  dma_init(void);                                   /* 0 ok; clock, reset, IRQ route, PLIC */
/* Handshake slot (== channel by convention) <- peripheral request line, see
 * system-control sdma_dma_ch_remap0/1. Slots must map distinct peripherals. */
void dma_map_request(unsigned slot, unsigned periph_req);
/* memory -> peripheral register (fixed address). 0 started, <0 bad arg / busy. */
int  dma_start_m2p(unsigned ch, unsigned slot, const void *mem, uintptr_t reg, size_t n, dma_fn cb);
/* peripheral register -> memory. */
int  dma_start_p2m(unsigned ch, unsigned slot, uintptr_t reg, void *mem, size_t n, dma_fn cb);
void dma_dump(unsigned ch);                           /* bring-up register dump */
void dma_abort(unsigned ch);                           /* stop + disable, no callback */
int  dma_busy(unsigned ch);                            /* CHx_EN */
uint32_t dma_id(void);                                 /* DMAC_IDREG (bring-up dump) */
extern volatile uint32_t dma_irqs, dma_errs;           /* counters */
#endif
