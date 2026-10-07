// Panel link to the Nano (UART0, GP16 TX / GP17 RX, 1.5625 Mbaud 8N1), rvpanel
// protocol (common/rvpanel.h). RX bytes are buffered by the UART IRQ (the I2C
// flushes block the main loop for ms) and decoded in nano_link_poll().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rvpanel.h"

#define NANO_LINK_BAUD  RVPANEL_BAUD

// Called from nano_link_poll() for each good PAGE / CONFIG packet.
typedef struct {
    void (*page)(unsigned display, unsigned page, const uint8_t *data);
    void (*config)(const uint8_t *payload, unsigned len);
} nano_link_handlers_t;

typedef struct {
    uint32_t rx_bytes, rx_ok, crc_err, cobs_err, rx_dropped, tx_packets;
    uint32_t last_rx_ms;        // time of the last good packet, 0 = never
} nano_link_stats_t;

void nano_link_init(void);
void nano_link_poll(const nano_link_handlers_t *h, uint32_t now_ms);
void nano_link_send_enc(unsigned id, int32_t delta);    // split into i8 steps
void nano_link_send_sw(unsigned id, bool down);
void nano_link_send_midi(const uint8_t *bytes, unsigned len);
void nano_link_send_status(void);
// MIDI clock (RVPANEL_CLK_*), stamped with time_us_32(). Safe from an IRQ: all
// sends are atomic with respect to each other.
void nano_link_send_clock(unsigned kind, uint32_t t_us);
void nano_link_get_stats(nano_link_stats_t *out);
