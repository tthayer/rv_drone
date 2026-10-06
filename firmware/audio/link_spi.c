// SPI0 slave (mode 3, 8 bit) receiving rvlink_m2s_t and sending rvlink_s2m_t.
//
// Pins: GP16 RX (Nano MOSI), GP17 CSn, GP18 SCK, GP19 TX (Nano MISO), GP20 DRQ.
//
// Arming sequence (link_arm), always run with CS high between frames:
//   1. abort both DMA channels, ack the RX IRQ
//   2. reset the SPI0 block (clears BOTH FIFOs, SR flags, shifter): a lost or
//      extra byte can never leave stale data behind
//   3. [after a completed frame only] validate it, build+seal the next reply
//   4. configure RX DMA (528 B, SPI RX DREQ -> rx buffer) and TX DMA (528 B,
//      tx buffer -> SPI TX, DREQ-paced); start TX first so it pre-fills the
//      8-entry TX FIFO while SSE is still off, then RX
//   5. SSE on, then link_armed = true
// DRQ is raised only from link_spi_on_block() and only while armed with zero
// bytes received. DRQ timing: raised once per block period (750 Hz), dropped
// when the frame completes (RX DMA IRQ). If the previous request was never
// served, DRQ is pulsed low for 2 us and raised again so the Nano always sees a
// fresh rising edge.
//
// Desync: a rising CS edge (GPIO IRQ) while the RX DMA is mid-frame
// (0 < received < 528) is a short frame: count short_err, redo steps 1,2,4,5
// (the prepared reply is reused). A frame longer than 528 B completes the DMA
// early; the surplus is a partial frame in the next arming and is caught the
// same way at the next CS rise.
#include "link_spi.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/resets.h"
#include "hardware/resets.h"
#include "hardware/spi.h"

#define PIN_RX    16
#define PIN_CS    17
#define PIN_SCK   18
#define PIN_TX    19
#define PIN_DRQ   20

#define LINK_IRQ         DMA_IRQ_1
#define PRIO_AUDIO       0x00
#define PRIO_LINK        0x80

static rvlink_m2s_t rx_buf __attribute__((aligned(4)));
static rvlink_s2m_t tx_buf __attribute__((aligned(4)));
static link_stats_t stats;
static const volatile uint32_t *underruns_src;
static int rx_ch, tx_ch;
static volatile bool link_armed;

static inline spi_hw_t *hw(void) { return spi_get_hw(spi0); }

static inline uint32_t rx_remaining(void) {
    return dma_channel_hw_addr(rx_ch)->transfer_count & 0x0FFFFFFFu;
}

static void spi_hw_reset(void) {
    reset_block_num(RESET_SPI0);
    unreset_block_num_wait_blocking(RESET_SPI0);
    hw()->cpsr = 2;
    hw()->cr0 = (7u << SPI_SSPCR0_DSS_LSB) | SPI_SSPCR0_SPO_BITS | SPI_SSPCR0_SPH_BITS;
    hw()->cr1 = SPI_SSPCR1_MS_BITS;                 // slave, SSE still off
    hw()->dmacr = SPI_SSPDMACR_RXDMAE_BITS | SPI_SSPDMACR_TXDMAE_BITS;
}

static void dma_setup_and_enable(void) {
    dma_channel_abort(rx_ch);
    dma_channel_abort(tx_ch);
    dma_hw->ints1 = 1u << rx_ch;
    spi_hw_reset();

    dma_channel_config c = dma_channel_get_default_config(tx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, true));
    dma_channel_configure(tx_ch, &c, &hw()->dr, &tx_buf, RVLINK_FRAME_LEN, true);

    c = dma_channel_get_default_config(rx_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, spi_get_dreq(spi0, false));
    dma_channel_configure(rx_ch, &c, &rx_buf, &hw()->dr, RVLINK_FRAME_LEN, true);

    hw()->cr1 = SPI_SSPCR1_MS_BITS | SPI_SSPCR1_SSE_BITS;
    link_armed = true;
}

static void link_arm(bool new_reply) {
    link_armed = false;
    if (new_reply)
        link_build_reply(&stats, *underruns_src, &tx_buf);
    dma_setup_and_enable();
}

// Frame complete (528 bytes received).
static void __isr link_dma_irq(void) {
    if (!(dma_hw->ints1 & (1u << rx_ch))) return;
    dma_hw->ints1 = 1u << rx_ch;
    link_armed = false;
    gpio_put(PIN_DRQ, 0);
    link_validate(&stats, &rx_buf);
    link_arm(true);
}

// CS rising edge: normally the end of a frame (RX DMA done or about to be).
static void cs_gpio_cb(uint gpio, uint32_t events) {
    (void)events;
    if (gpio != PIN_CS || !link_armed) return;
    busy_wait_us_32(2);                 // let the last byte reach memory via DMA
    if (!gpio_get(PIN_CS)) return;              // CS low again: a blip, not frame end
    if (!dma_channel_is_busy(rx_ch)) return;    // completed; DMA IRQ handles it
    uint32_t rem = rx_remaining();
    if (rem == RVLINK_FRAME_LEN) return;        // CS glitch, nothing received
    link_armed = false;
    gpio_put(PIN_DRQ, 0);
    stats.short_err++;
    link_arm(false);
}

void link_spi_on_block(void) {
    if (!link_armed || !dma_channel_is_busy(rx_ch)) return;
    if (rx_remaining() != RVLINK_FRAME_LEN) return;     // frame in flight
    if (gpio_get_out_level(PIN_DRQ)) {
        gpio_put(PIN_DRQ, 0);           // previous request unserved: new edge
        busy_wait_us_32(2);
    }
    gpio_put(PIN_DRQ, 1);
}

void link_spi_get_stats(link_stats_t *out) { *out = stats; }

void link_spi_init(const volatile uint32_t *src) {
    underruns_src = src;
    memset(&stats, 0, sizeof stats);

    gpio_init(PIN_DRQ);
    gpio_set_dir(PIN_DRQ, GPIO_OUT);
    gpio_put(PIN_DRQ, 0);

    spi_hw_reset();
    gpio_set_function(PIN_RX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_TX, GPIO_FUNC_SPI);

    rx_ch = dma_claim_unused_channel(true);
    tx_ch = dma_claim_unused_channel(true);
    dma_channel_set_irq1_enabled(rx_ch, true);
    irq_set_exclusive_handler(LINK_IRQ, link_dma_irq);
    irq_set_priority(LINK_IRQ, PRIO_LINK);
    irq_set_enabled(LINK_IRQ, true);
    irq_set_priority(DMA_IRQ_0, PRIO_AUDIO);            // I2S refill preempts the link

    link_arm(true);

    gpio_set_irq_enabled_with_callback(PIN_CS, GPIO_IRQ_EDGE_RISE, true, cs_gpio_cb);
    irq_set_priority(IO_IRQ_BANK0, PRIO_LINK);
}
