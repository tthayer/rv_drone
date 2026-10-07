#include "nano_link.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define NANO_UART       uart0
#define NANO_UART_IRQ   UART0_IRQ
#define PIN_TX          16
#define PIN_RX          17

// 8 KB covers ~50 ms of a saturated link while an I2C flush blocks the loop.
#define RX_RING_LEN 8192        // power of two
static uint8_t rx_ring[RX_RING_LEN];
static volatile uint32_t rx_head, rx_tail, rx_dropped, rx_bytes;
static rvpanel_rx_t dec;
static uint32_t tx_packets, last_rx_ms;

static void nano_rx_irq(void) {
    while (uart_is_readable(NANO_UART)) {
        uint8_t b = (uint8_t)uart_get_hw(NANO_UART)->dr;
        rx_bytes++;
        if (rx_head - rx_tail >= RX_RING_LEN) { rx_dropped++; continue; }
        rx_ring[rx_head % RX_RING_LEN] = b;
        rx_head++;
    }
}

void nano_link_init(void) {
    rvpanel_rx_init(&dec);
    uart_init(NANO_UART, NANO_LINK_BAUD);
    uart_set_format(NANO_UART, 8, 1, UART_PARITY_NONE);
    gpio_set_function(PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_RX);       // keep an unconnected RX idle-high
    uart_set_fifo_enabled(NANO_UART, true);
    irq_set_exclusive_handler(NANO_UART_IRQ, nano_rx_irq);
    irq_set_enabled(NANO_UART_IRQ, true);
    uart_set_irq_enables(NANO_UART, true, false);
}

void nano_link_poll(const nano_link_handlers_t *h, uint32_t now_ms) {
    while (rx_tail != rx_head) {
        uint8_t b = rx_ring[rx_tail % RX_RING_LEN];
        rx_tail++;
        size_t len;
        if (!rvpanel_rx_feed(&dec, b, &len)) continue;
        last_rx_ms = now_ms ? now_ms : 1;
        const uint8_t *p = dec.buf + 1;
        switch (dec.buf[0]) {
        case RVPANEL_PAGE:
            if (len == 2 + RVPANEL_PAGE_LEN && h->page) h->page(p[0], p[1], p + 2);
            break;
        case RVPANEL_CONFIG:
            if (h->config) h->config(p, (unsigned)len);
            break;
        }
    }
}

// Interrupts off for the whole packet: the MIDI IRQ sends CLOCK packets, and
// two packets must never interleave on the wire. <= 16 B here: ~100 us.
static void send(uint8_t type, const void *payload, unsigned len) {
    uint8_t w[RVPANEL_MAX_WIRE];
    size_t n = rvpanel_encode(type, payload, len, w);
    uint32_t irq = save_and_disable_interrupts();
    uart_write_blocking(NANO_UART, w, n);
    tx_packets++;
    restore_interrupts(irq);
}

void nano_link_send_clock(unsigned kind, uint32_t t) {
    uint8_t p[5] = { (uint8_t)kind, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24) };
    send(RVPANEL_CLOCK, p, sizeof p);
}

void nano_link_send_enc(unsigned id, int32_t delta) {
    while (delta) {
        int32_t step = delta > 127 ? 127 : delta < -127 ? -127 : delta;
        uint8_t p[2] = { (uint8_t)id, (uint8_t)(int8_t)step };
        send(RVPANEL_ENC, p, 2);
        delta -= step;
    }
}

void nano_link_send_sw(unsigned id, bool down) {
    uint8_t p[2] = { (uint8_t)id, down };
    send(RVPANEL_SW, p, 2);
}

void nano_link_send_midi(const uint8_t *bytes, unsigned len) {
    uint8_t p[4] = { (uint8_t)len, 0, 0, 0 };
    for (unsigned i = 0; i < len && i < 3; i++) p[1 + i] = bytes[i];
    send(RVPANEL_MIDI, p, 4);
}

static uint16_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }

void nano_link_send_status(void) {
    uint16_t crc = sat16(dec.crc_err), cobs = sat16(dec.cobs_err), drop = sat16(rx_dropped);
    uint8_t p[10] = {
        (uint8_t)dec.ok, (uint8_t)(dec.ok >> 8), (uint8_t)(dec.ok >> 16), (uint8_t)(dec.ok >> 24),
        (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)cobs, (uint8_t)(cobs >> 8),
        (uint8_t)drop, (uint8_t)(drop >> 8),
    };
    send(RVPANEL_STATUS, p, sizeof p);
}

void nano_link_get_stats(nano_link_stats_t *out) {
    out->rx_bytes = rx_bytes;
    out->rx_ok = dec.ok;
    out->crc_err = dec.crc_err;
    out->cobs_err = dec.cobs_err;
    out->rx_dropped = rx_dropped;
    out->tx_packets = tx_packets;
    out->last_rx_ms = last_rx_ms;
}
