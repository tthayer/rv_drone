#ifndef AUDIO_LINK_H
#define AUDIO_LINK_H
#include <stdint.h>

/* rvlink master (Nano side): one 528 B full-duplex SPI2 transaction per DRQ rising
 * edge from Pico A. M4 sends the test pattern (RVLINK_F_TEST). Not available on
 * boards without BOARD_HAS_SPI_LINK (qemu): init returns -1, everything else no-ops. */
typedef struct {
    uint32_t frames;          /* completed transactions */
    uint32_t drq_edges;       /* DRQ IRQs seen */
    uint32_t missed;          /* DRQ while a previous one was still unserviced */
    uint32_t rx_crc_err, rx_magic_err, spi_err;
    uint32_t xfer_ticks;      /* last transaction, rdtime ticks (setup + 528 B) */
    uint32_t xfer_ticks_min;
    /* slave's last good reply */
    uint16_t slave_seq_echo, slave_crc_err, slave_underruns;
    uint8_t  slave_ring_fill;
    uint16_t seq;             /* next seq to send */
} audio_link_stats_t;

int  audio_link_init(void);
int  audio_link_pending(void);        /* a DRQ edge is waiting for audio_link_poll() */
void audio_link_poll(void);           /* run the pending transaction, if any (main loop) */
void audio_link_kick(void);           /* force one transaction without DRQ (scope/debug) */
const volatile audio_link_stats_t *audio_link_stats(void);
#endif
