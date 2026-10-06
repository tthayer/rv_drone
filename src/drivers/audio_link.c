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

static rvlink_m2s_t tx_frame __attribute__((aligned(64)));
static rvlink_s2m_t rx_frame __attribute__((aligned(64)));
static volatile uint32_t pending;
static volatile audio_link_stats_t st = { .xfer_ticks_min = 0xffffffffu };

static void drq_isr(void)
{
    st.drq_edges++;
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

    uint64_t t0 = rdtime();
    int rc = spi_xfer((const uint8_t *)&tx_frame, (uint8_t *)&rx_frame, RVLINK_FRAME_LEN);
    uint32_t dt = (uint32_t)(rdtime() - t0);
    st.xfer_ticks = dt;
    if (dt < st.xfer_ticks_min)
        st.xfer_ticks_min = dt;

    st.frames++;
    st.seq++;
    if (rc != 0) {
        st.spi_err++;
    } else if (rx_frame.magic != RVLINK_MAGIC_S2M) {
        st.rx_magic_err++;
    } else if (!rvlink_check(&rx_frame)) {
        st.rx_crc_err++;
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
