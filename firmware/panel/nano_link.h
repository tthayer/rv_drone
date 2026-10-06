// Panel link to the Nano (UART0, GP16 TX / GP17 RX, 1.5625 Mbaud 8N1).
// M3 stub: the UART is up and incoming bytes are drained and counted. The
// rvpanel protocol (COBS + CRC16, PAGE/CONFIG in, ENC/SW/MIDI/STATUS out)
// arrives in M6.
#pragma once

#include <stdint.h>

#define NANO_LINK_BAUD  1562500

void nano_link_init(void);
void nano_link_poll(void);
uint32_t nano_link_rx_bytes(void);
