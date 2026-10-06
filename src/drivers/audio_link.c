#include "audio_link.h"
#include "board.h"

#ifndef BOARD_HAS_SPI_LINK
static const volatile audio_link_stats_t st;
int audio_link_init(void) { return -1; }
int audio_link_pending(void) { return 0; }
void audio_link_poll(void) {}
void audio_link_kick(void) {}
const volatile audio_link_stats_t *audio_link_stats(void) { return &st; }
#else
#include "rvlink.h"
#include "gpio.h"
#include "pinmux.h"
#include "spi.h"
#include "timer.h"
#include "uart.h"

static rvlink_m2s_t tx_frame __attribute__((aligned(64)));
static rvlink_s2m_t rx_frame __attribute__((aligned(64)));
static volatile uint32_t pending;
static volatile uint64_t xfer_end;     /* rdtime() when the last frame finished */
static volatile int in_xfer;
#define DRQ_HOLDOFF (BOARD_TIMEBASE_HZ / 50000)   /* 20 us */
static volatile audio_link_stats_t st = { .xfer_ticks_min = 0xffffffffu };

static void drq_isr(void)
{
    st.drq_edges++;
    /* Edges latched during a frame (SCK crosstalk, IRQs masked) fire just after
     * it, while Pico A has not yet dropped DRQ. The next real request comes
     * at least ~300 us later. */
    if (in_xfer || rdtime() - xfer_end < DRQ_HOLDOFF) {
        st.spurious++;
        return;
    }
    if (pending)
        st.missed++;
    pending = 1;
}

/* Built right after each transfer so the next DRQ only has to start the SPI. */
static void prepare_next(void)
{
    uint16_t seq = st.seq;
    uint8_t *a = (uint8_t *)tx_frame.audio;
    tx_frame.magic = RVLINK_MAGIC_M2S;
    tx_frame.seq = seq;
    tx_frame.flags = RVLINK_F_TEST;
    for (size_t i = 0; i < RVLINK_AUDIO_LEN; i++)
        a[i] = rvlink_test_byte(seq, i);
    tx_frame.pad = 0;
    rvlink_seal(&tx_frame);
}

int audio_link_init(void)
{
    pinmux_set(BOARD_FMUX_DRQ, BOARD_FMUX_DRQ_FN);
    gpio_set_input(BOARD_DRQ_GPIO_BIT);
    if (spi_init(BOARD_SPI_TARGET_HZ) != 0)
        return -1;
    prepare_next();
    gpio_init();
    gpio_irq_edge(BOARD_DRQ_GPIO_BIT, 1, drq_isr);
    if (gpio_read(BOARD_DRQ_GPIO_BIT))   /* already high: no edge will come */
        pending = 1;
    return 0;
}

int audio_link_pending(void) { return pending != 0; }

void audio_link_kick(void) { pending = 1; }

void audio_link_poll(void)
{
    if (!pending)
        return;
    pending = 0;
    /* DRQ is a level request: SCK crosstalk on the breadboard makes false edges,
     * so start only while DRQ is really high. */
    if (!gpio_read(BOARD_DRQ_GPIO_BIT)) {
        st.spurious++;
        return;
    }

    uint64_t t0 = rdtime();
    in_xfer = 1;
    int rc = spi_xfer((const uint8_t *)&tx_frame, (uint8_t *)&rx_frame, RVLINK_FRAME_LEN);
    uint32_t dt = (uint32_t)(rdtime() - t0);
    xfer_end = rdtime();
    in_xfer = 0;
    pending = 0;            /* edges latched during the frame are crosstalk */
    st.xfer_ticks = dt;
    if (dt < st.xfer_ticks_min)
        st.xfer_ticks_min = dt;

    st.frames++;
    st.seq++;
    if (rc != 0) {
        st.spi_err++;
    } else if (rx_frame.magic != RVLINK_MAGIC_S2M) {
        st.rx_magic_err++;
        static uint64_t next_dump;
        if (rdtime() >= next_dump) {    /* bring-up: what does MISO carry? */
            next_dump = rdtime() + BOARD_TIMEBASE_HZ;
            const uint8_t *b = (const uint8_t *)&rx_frame;
            uart_puts("link: bad rx:");
            for (int i = 0; i < 24; i++) {
                uart_putc(' ');
                uart_putc("0123456789abcdef"[b[i] >> 4]);
                uart_putc("0123456789abcdef"[b[i] & 15]);
            }
            uart_putc('\n');
        }
    } else if (!rvlink_check(&rx_frame)) {
        st.rx_crc_err++;
        static uint64_t next_dump2;
        if (rdtime() >= next_dump2) {   /* bring-up: where does the frame break? */
            next_dump2 = rdtime() + BOARD_TIMEBASE_HZ;
            const uint8_t *b = (const uint8_t *)&rx_frame;
            for (unsigned i = 12; i < RVLINK_FRAME_LEN - 4; i++) {
                uint8_t want = (uint8_t)(0xA5u ^ (i - 12u));
                if (b[i] != want) {
                    uart_puts("link: crc bad, first diff at ");
                    uart_put_dec(i);
                    uart_puts(":");
                    for (unsigned k = i; k < i + 6 && k < RVLINK_FRAME_LEN; k++) {
                        uart_putc(' ');
                        uart_putc("0123456789abcdef"[b[k] >> 4]);
                        uart_putc("0123456789abcdef"[b[k] & 15]);
                    }
                    uart_puts("  want");
                    for (unsigned k = i; k < i + 6; k++) {
                        uint8_t w = (uint8_t)(0xA5u ^ (k - 12u));
                        uart_putc(' ');
                        uart_putc("0123456789abcdef"[w >> 4]);
                        uart_putc("0123456789abcdef"[w & 15]);
                    }
                    uart_putc('\n');
                    break;
                }
            }
        }
    } else {
        st.slave_seq_echo = rx_frame.seq_echo;
        st.slave_ring_fill = rx_frame.ring_fill;
        st.slave_underruns = rx_frame.underruns;
        st.slave_crc_err = rx_frame.crc_errors;
    }
    prepare_next();
}

const volatile audio_link_stats_t *audio_link_stats(void) { return &st; }
#endif
