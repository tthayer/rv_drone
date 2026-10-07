#ifndef PANEL_LINK_H
#define PANEL_LINK_H
#include <stdint.h>

/* rvpanel (common/rvpanel.h) over UART2 to Pico B, 1.5625 Mbaud 8N1.
 * RX: the UART2 IRQ decodes packets and queues events for the main loop.
 * TX: a ring drained by the UART2 THR-empty interrupt (programmable threshold,
 * 16 of 64 FIFO bytes), so bursts stream at the full 1.5625 Mbaud line rate.
 * Boards without BOARD_HAS_PANEL_LINK (qemu): init returns -1, the rest no-ops. */
typedef struct {
    uint8_t type;            /* RVPANEL_ENC / _SW / _MIDI / _CLOCK */
    uint8_t a, b, c, d;      /* ENC: id, delta(i8) | SW: id, down | MIDI: len, bytes[3] | CLOCK: kind */
    uint32_t t;              /* CLOCK: Pico B timestamp, us */
} panel_event_t;

typedef struct {
    uint32_t rx_bytes, rx_ok, rx_crc_err, rx_cobs_err, rx_overrun, evt_dropped;
    uint32_t tx_packets, tx_dropped, tx_irqs;
    /* Pico B's last STATUS: what it received from us */
    uint32_t peer_rx_ok;
    uint16_t peer_crc_err, peer_cobs_err, peer_dropped;
    uint32_t peer_status_count;
    uint32_t clock_events;
} panel_link_stats_t;

int  panel_link_init(void);
int  panel_link_next_event(panel_event_t *e);   /* 1 = got one */
/* Queues one packet; returns 0, or -1 if the TX ring lacks room (dropped). */
int  panel_link_send(uint8_t type, const void *payload, unsigned len);
unsigned panel_link_tx_free(void);              /* bytes free in the TX ring */
void panel_link_poll(void);                     /* main loop safety net: re-arms TX if data is pending */
const volatile panel_link_stats_t *panel_link_stats(void);
#endif
