// PIO SPI slave (pio1, spi_slave.pio; mode 3, 8 bit) receiving rvlink_m2s_t and
// sending rvlink_s2m_t. The PL022 slave was replaced: on hardware it returned
// every byte after byte 0 one bit early and saw short frames.
//
// Pins: GP16 MOSI in, GP17 CSn, GP18 SCK, GP19 MISO out, GP20 DRQ.
//
// Arming sequence (link_arm), always run with CS high between frames:
//   1. disable the SM, abort both DMA channels, ack the RX IRQ
//   2. pio_sm_clear_fifos + pio_sm_restart (clears ISR/OSR shift counters, so a
//      partial byte is discarded) + exec "jmp idle"
//   3. [after a completed frame only] validate it, build+seal the next reply
//   4. start TX DMA (528 B, tx buffer -> TXF, DREQ-paced; fills the 4-deep TX
//      FIFO), wait for the first byte to land, exec "pull" to preload OSR, then
//      start RX DMA (528 B, RXF -> rx buffer), enable the SM
//   5. link_armed = true
// Byte lanes (RP2350 datasheet, PIO chapter, TXF/RXF FIFO register access; same
// usage as pico-examples pio/spi pio_spi.c): a narrow write to TXF is
// replicated across all byte lanes, so an 8-bit DMA write puts the byte in bits
// 31:24, which is where a left-shifting OSR takes its next bit from. With IN
// shift left + autopush 8, the byte lands in ISR[7:0], so an 8-bit DMA read of
// RXF (byte lane 0, little endian) returns it.
// DRQ is raised only while armed with zero bytes received and the ring below
// AUDIO_RING_TARGET: at each block tick (750 Hz, after the I2S pop), and right
// after a frame if the ring is still short (priming, or a lost frame). It is
// dropped when the frame completes (RX DMA IRQ). If the previous request was
// never served, DRQ is pulsed low for 2 us and raised again so the Nano always
// sees a fresh rising edge.
//
// Desync: a rising CS edge (GPIO IRQ) while the RX DMA is mid-frame
// (0 < received < 528) is a short frame: count short_err, redo the arming (the
// prepared reply is reused). A frame longer than 528 B completes the DMA early;
// the surplus is a partial frame in the next arming and is caught the same way
// at the next CS rise.
#include "link_spi.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "spi_slave.pio.h"

#define PIN_RX    16    // in_base: +1 = CS, +2 = SCK
#define PIN_CS    17
#define PIN_SCK   18
#define PIN_TX    19
#define PIN_DRQ   20

#define LINK_IRQ         DMA_IRQ_1
#define PRIO_AUDIO       0x00
#define PRIO_LINK        0x80
#define CATCHUP_DELAY_US 30u     // > the Nano's 20 us DRQ hold-off

static rvlink_m2s_t rx_buf __attribute__((aligned(4)));
static rvlink_s2m_t tx_buf __attribute__((aligned(4)));
static link_stats_t stats;
static audio_ring_t *ring;
static int rx_ch, tx_ch;
static volatile bool link_armed;
static uint32_t stats_catchups;

static PIO link_pio;
static uint link_sm, link_off;

static inline uint32_t rx_remaining(void) {
    return dma_channel_hw_addr(rx_ch)->transfer_count & 0x0FFFFFFFu;
}

static void dma_setup_and_enable(void) {
    pio_sm_set_enabled(link_pio, link_sm, false);
    dma_channel_abort(rx_ch);
    dma_channel_abort(tx_ch);
    dma_hw->ints1 = 1u << rx_ch;
    pio_sm_clear_fifos(link_pio, link_sm);
    pio_sm_restart(link_pio, link_sm);
    pio_sm_exec(link_pio, link_sm, pio_encode_jmp(link_off + spi_slave_offset_idle));

    dma_channel_config c = dma_channel_get_default_config(tx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(link_pio, link_sm, true));
    dma_channel_configure(tx_ch, &c, &link_pio->txf[link_sm], &tx_buf,
                          RVLINK_FRAME_LEN, true);

    c = dma_channel_get_default_config(rx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(link_pio, link_sm, false));
    dma_channel_configure(rx_ch, &c, &rx_buf, (const void *)&link_pio->rxf[link_sm],
                          RVLINK_FRAME_LEN, true);

    // Preload OSR with byte 0 once the TX DMA has delivered it (a few cycles).
    for (int i = 0; i < 1000 && pio_sm_is_tx_fifo_empty(link_pio, link_sm); i++)
        tight_loop_contents();
    pio_sm_exec(link_pio, link_sm, pio_encode_pull(false, true));

    pio_sm_set_enabled(link_pio, link_sm, true);
    link_armed = true;
}

static uint32_t last_rem = RVLINK_FRAME_LEN;   // rx remaining at the last tick

static void link_arm(bool new_reply) {
    last_rem = RVLINK_FRAME_LEN;
    link_armed = false;
    if (new_reply)
        link_build_reply(&stats, ring->underruns, audio_ring_fill(ring), &tx_buf);
    dma_setup_and_enable();
}

// Frame complete (528 bytes received).
static void __isr link_dma_irq(void) {
    if (!(dma_hw->ints1 & (1u << rx_ch))) return;
    dma_hw->ints1 = 1u << rx_ch;
    uint32_t t_end = time_us_32();
    link_armed = false;
    gpio_put(PIN_DRQ, 0);
    if (link_validate(&stats, &rx_buf) == LINK_OK && !(rx_buf.flags & RVLINK_F_TEST))
        audio_ring_push(ring, (const int32_t *)rx_buf.audio);
    link_arm(true);
    // Still short: ask again now rather than at the next tick, so priming and
    // lost frames catch up. The Nano ignores DRQ edges for 20 us after a frame.
    if (audio_ring_fill(ring) < AUDIO_RING_TARGET) {
        while (time_us_32() - t_end < CATCHUP_DELAY_US)
            tight_loop_contents();
        stats_catchups++;
        if (gpio_get_out_level(PIN_DRQ)) {  // a tick raised it inside the hold-off
            gpio_put(PIN_DRQ, 0);
            busy_wait_us_32(2);
        }
        gpio_put(PIN_DRQ, 1);
    }
}

// CS rising edge: normally the end of a frame (RX DMA done or about to be).
// Short frames are detected here, at the block tick, instead of with a CS
// edge IRQ: on the breadboard, CS edge events fired mid-frame and restarted
// the SM while the Nano was still clocking. A frame takes ~0.54 ms at 7.8 MHz
// and ticks are 1.33 ms apart, so no progress since the last tick means the
// frame is stale. Clear it before raising the next request.
void link_spi_on_block(void) {
    if (!link_armed || !dma_channel_is_busy(rx_ch)) return;
    uint32_t rem = rx_remaining();
    if (rem != RVLINK_FRAME_LEN) {                      // partial frame
        if (rem == last_rem) {                          // stalled for a whole tick
            link_armed = false;
            gpio_put(PIN_DRQ, 0);
            stats.short_err++;
            last_rem = RVLINK_FRAME_LEN;
            link_arm(false);
        } else {
            last_rem = rem;                             // still moving: in flight
        }
        return;
    }
    last_rem = RVLINK_FRAME_LEN;
    if (audio_ring_fill(ring) >= AUDIO_RING_TARGET) return;
    if (gpio_get_out_level(PIN_DRQ)) {
        gpio_put(PIN_DRQ, 0);           // previous request unserved: new edge
        busy_wait_us_32(2);
    }
    gpio_put(PIN_DRQ, 1);
}

void link_spi_get_stats(link_stats_t *out) { *out = stats; }

uint32_t link_spi_catchups(void) { return stats_catchups; }

void link_spi_init(audio_ring_t *r) {
    ring = r;
    memset(&stats, 0, sizeof stats);

    gpio_init(PIN_DRQ);
    gpio_set_dir(PIN_DRQ, GPIO_OUT);
    gpio_put(PIN_DRQ, 0);

    link_pio = pio1;                    // pio0 is I2S
    link_sm = pio_claim_unused_sm(link_pio, true);
    link_off = pio_add_program(link_pio, &spi_slave_program);
    spi_slave_program_init(link_pio, link_sm, link_off, PIN_RX, PIN_TX);

    rx_ch = dma_claim_unused_channel(true);
    tx_ch = dma_claim_unused_channel(true);
    dma_channel_set_irq1_enabled(rx_ch, true);
    irq_set_exclusive_handler(LINK_IRQ, link_dma_irq);
    irq_set_priority(LINK_IRQ, PRIO_LINK);
    irq_set_enabled(LINK_IRQ, true);
    irq_set_priority(DMA_IRQ_0, PRIO_AUDIO);            // I2S refill preempts the link

    link_arm(true);

}
