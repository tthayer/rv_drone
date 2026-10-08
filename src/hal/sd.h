#ifndef SD_H
#define SD_H
#include <stdint.h>

/* SD0 microSD, polled SDHCI (no DMA, no IRQ), 4-bit at ~23 MHz, SDHC/SDXC and
 * SDSC. Block size 512. All calls are blocking; long waits call the idle hook
 * so the caller can keep audio running. Main-loop context only. */
typedef struct {
    uint32_t rca;
    uint64_t sectors;       /* 512-byte blocks */
    int      sdhc;          /* block addressing */
    int      bus4;
    uint32_t cid[4], csd[4];
    uint32_t clk_hz;        /* SD clock after init, from base_hz */
    uint32_t base_hz;       /* clk_sd0 as decoded from CLKGEN (TRM field layout) */
} sd_info_t;

int  sd_init(void);                         /* 0 = card ready; <0 = step that failed */
int  sd_read(uint32_t lba, uint8_t *buf, uint32_t count);
int  sd_write(uint32_t lba, const uint8_t *buf, uint32_t count);
const sd_info_t *sd_info(void);
void sd_set_idle_hook(void (*fn)(void));
void sd_dump(void);                         /* controller + clock registers to UART */
#endif
