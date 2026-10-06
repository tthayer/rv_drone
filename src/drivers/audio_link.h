#ifndef AUDIO_LINK_H
#define AUDIO_LINK_H
#include <stdint.h>

/* rvlink master (Nano side): one 528 B full-duplex SPI2 transaction per DRQ rising
 * edge from Pico A. Each frame carries the next block from the render callback
 * (main-loop context), or the M4 test pattern (RVLINK_F_TEST) when none is set or
 * test mode is on. With BOARD_SPI_DMA the
 * frame is run by the sysDMA under IRQ (main loop only post-processes); else polled. Not available on
 * boards without BOARD_HAS_SPI_LINK (qemu): init returns -1, everything else no-ops. */
typedef struct {
    uint32_t frames;          /* completed transactions */
    uint32_t drq_edges;       /* DRQ IRQs seen */
    uint32_t missed;          /* DRQ while a previous one was still unserviced */
    uint32_t spurious;        /* DRQ edge but line low at service time (crosstalk) */
    uint32_t rx_crc_err, rx_magic_err, spi_err;
    uint32_t xfer_ticks;      /* last transaction, rdtime ticks (setup + 528 B) */
    uint32_t xfer_ticks_min;
    /* slave's last good reply */
    uint16_t slave_seq_echo, slave_crc_err, slave_underruns;
    uint8_t  slave_ring_fill;
    uint16_t seq;             /* next seq to send */
    uint32_t dma_frames;      /* frames completed through the DMA path */
    uint32_t dma_err;         /* DMA faults + watchdog aborts (3 in a row -> polled) */
} audio_link_stats_t;

int  audio_link_init(void);
int  audio_link_pending(void);        /* a DRQ edge is waiting for audio_link_poll() */
void audio_link_poll(void);           /* run the pending transaction, if any (main loop) */
void audio_link_kick(void);           /* force one transaction without DRQ (scope/debug) */
int  audio_link_dma_active(void);     /* 1 = frames run by DMA, 0 = polled spi_xfer() */
void audio_link_set_dma(int on);      /* runtime switch (ignored mid-frame); on needs CMO + DMAC */
typedef void (*audio_link_render_fn)(int32_t *lr, unsigned frames);
void audio_link_set_render(audio_link_render_fn fn);   /* call before audio_link_init() */
void audio_link_set_test(int on);     /* 1 = test pattern, 0 = rendered audio (next frame on) */
int  audio_link_test_mode(void);
const volatile audio_link_stats_t *audio_link_stats(void);
#endif
