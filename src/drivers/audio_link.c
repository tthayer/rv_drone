#include "audio_link.h"
#include "board.h"

#ifndef BOARD_HAS_SPI_LINK
static const volatile audio_link_stats_t st;
int audio_link_init(void) { return -1; }
int audio_link_pending(void) { return 0; }
void audio_link_poll(void) {}
void audio_link_kick(void) {}
int audio_link_dma_active(void) { return 0; }
void audio_link_set_dma(int on) { (void)on; }
void audio_link_set_render(audio_link_render_fn fn) { (void)fn; }
void audio_link_set_test(int on) { (void)on; }
int audio_link_test_mode(void) { return 0; }
const volatile audio_link_stats_t *audio_link_stats(void) { return &st; }
#else
#include "rvlink.h"
#include "gpio.h"
#include "pinmux.h"
#include "spi.h"
#include "dma.h"
#include "timer.h"
#include "uart.h"

#include "cache.h"
/* DMA buffers: whole 64 B lines, so cache maintenance never touches a neighbour. */
static union { rvlink_m2s_t f; uint8_t line[CACHE_ROUND(sizeof(rvlink_m2s_t))]; }
    tx_u __attribute__((aligned(CACHE_LINE)));
static union { rvlink_s2m_t f; uint8_t line[CACHE_ROUND(sizeof(rvlink_s2m_t))]; }
    rx_u __attribute__((aligned(CACHE_LINE)));
#define tx_frame tx_u.f
#define rx_frame rx_u.f
static volatile uint32_t pending;
static volatile uint64_t xfer_end;     /* rdtime() when the last frame finished */
static volatile int in_xfer;
/* DMA mode: the frame runs under the DMA IRQ; the completion callback only
 * records the result and the main loop does the CRC/stat work (frame_done). */
static int use_dma;                    /* runtime switch (BOARD_SPI_DMA default, 'd' key) */
static volatile int tx_ready;          /* tx_frame holds the next frame (prepare_next ran) */
static volatile int done_flag;         /* DMA frame finished, frame_done() still to run */
static volatile int done_rc;
static volatile uint64_t xfer_start;
static uint32_t dma_fail_streak;
static audio_link_render_fn render;
static int test_mode;
#define DMA_TIMEOUT (BOARD_TIMEBASE_HZ / 100)     /* 10 ms; a frame takes ~0.55 ms */
#define DMA_MAX_FAILS 3
#define DRQ_HOLDOFF (BOARD_TIMEBASE_HZ / 50000)   /* 20 us */
static volatile audio_link_stats_t st = { .xfer_ticks_min = 0xffffffffu };

static int dma_start(void);

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
    if (use_dma && tx_ready && !done_flag) {     /* DMA: start right here, no main loop hop */
        if (!gpio_read(BOARD_DRQ_GPIO_BIT)) {
            st.spurious++;
            return;
        }
        if (dma_start() == 0)
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
    if (render && !test_mode) {
        tx_frame.flags = 0;
        render(tx_frame.audio, RVLINK_BLOCK_FRAMES);
    } else {
        tx_frame.flags = RVLINK_F_TEST;
        for (size_t i = 0; i < RVLINK_AUDIO_LEN; i++)
            a[i] = rvlink_test_byte(seq, i);
    }
    tx_frame.pad = 0;
    rvlink_seal(&tx_frame);
    tx_ready = 1;
}

int audio_link_init(void)
{
    pinmux_set(BOARD_FMUX_DRQ, BOARD_FMUX_DRQ_FN);
    gpio_set_input(BOARD_DRQ_GPIO_BIT);
    if (spi_init(BOARD_SPI_TARGET_HZ) != 0)
        return -1;
    prepare_next();
#if BOARD_SPI_DMA
    use_dma = spi_dma_init() == 0;
    uart_puts(use_dma ? "link: SPI DMA on\n" : "link: SPI DMA unavailable, polled\n");
#endif
    gpio_init();
    gpio_irq_edge(BOARD_DRQ_GPIO_BIT, 1, drq_isr);
    if (gpio_read(BOARD_DRQ_GPIO_BIT))   /* already high: no edge will come */
        pending = 1;
    return 0;
}

int audio_link_pending(void) { return pending != 0 || done_flag != 0; }

int audio_link_dma_active(void) { return use_dma; }

void audio_link_set_dma(int on)
{
    if (in_xfer || done_flag)
        return;
    use_dma = on && spi_dma_init() == 0;
    dma_fail_streak = 0;
}

void audio_link_kick(void) { pending = 1; }

void audio_link_set_render(audio_link_render_fn fn) { render = fn; }
void audio_link_set_test(int on) { test_mode = on; }
int audio_link_test_mode(void) { return test_mode || !render; }

static void frame_done(int rc, uint32_t dt)
{
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

/* DMA IRQ context: CS is already high. Only record; frame_done() runs in the main loop. */
static void dma_done_cb(int rc)
{
    xfer_end = rdtime();
    in_xfer = 0;
    pending = 0;                         /* edges latched during the frame are crosstalk */
    done_rc = rc;
    done_flag = 1;
}

/* Called with DRQ already qualified, from the DRQ ISR or the main loop. */
static int dma_start(void)
{
    if (!tx_ready || in_xfer)
        return -1;
    tx_ready = 0;
    xfer_start = rdtime();
    in_xfer = 1;
    int r = spi_xfer_dma_start((const uint8_t *)&tx_frame, (uint8_t *)&rx_frame,
                               RVLINK_FRAME_LEN, dma_done_cb);
    if (r != 0) {
        in_xfer = 0;
        tx_ready = 1;
    }
    return r;
}

static void poll_dma(void)
{
    if (done_flag) {
        done_flag = 0;
        int rc = done_rc;
        if (rc < 0) {                    /* DMA fault (not just FIFO flags) */
            st.dma_err++;
            dma_fail_streak++;
        } else {
            dma_fail_streak = 0;
        }
        st.dma_frames++;
        frame_done(rc, (uint32_t)(xfer_end - xfer_start));
    } else if (in_xfer && rdtime() - xfer_start > DMA_TIMEOUT) {
        uint64_t sst;
        __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(sst));
        if (!done_flag) {                /* really stuck: cb has not run */
            dma_dump(0);
            dma_dump(1);
            spi_dump_rt();
            spi_xfer_dma_abort();
            cache_inval(&rx_frame, sizeof rx_frame);
            const uint8_t *b = (const uint8_t *)&rx_frame;
            uart_puts("rx[0..15]:");
            for (int i = 0; i < 16; i++) {
                uart_putc(' ');
                uart_putc("0123456789abcdef"[b[i] >> 4]);
                uart_putc("0123456789abcdef"[b[i] & 15]);
            }
            {
                int good = 12;
                while (good < 524 && b[good] == (uint8_t)(0xA5u ^ (good - 12)))
                    good++;
                uart_puts("\nrx valid through byte ");
                uart_put_dec(good);
            }
            uart_puts("\nrx[512..527]:");
            for (int i = 512; i < 528; i++) {
                uart_putc(' ');
                uart_putc("0123456789abcdef"[b[i] >> 4]);
                uart_putc("0123456789abcdef"[b[i] & 15]);
            }
            uart_putc('\n');
            in_xfer = 0;
            xfer_end = rdtime();
            pending = 0;
            st.dma_err++;
            st.spi_err++;
            dma_fail_streak++;
            tx_ready = 0;
            prepare_next();              /* same seq again, as after any lost frame */
        }
        if (sst & 2)
            __asm__ volatile("csrsi sstatus, 2");
    }
    if (dma_fail_streak >= DMA_MAX_FAILS) {
        use_dma = 0;
        uart_puts("link: SPI DMA failing, falling back to polled\n");
        return;                          /* pending (if any) runs polled next loop */
    }
    if (!pending || in_xfer || done_flag)
        return;
    if (!tx_ready)
        return;                          /* keep pending: retry once the frame is ready */
    pending = 0;
    if (!gpio_read(BOARD_DRQ_GPIO_BIT)) {
        st.spurious++;
        return;
    }
    dma_start();
}

void audio_link_poll(void)
{
    if (use_dma) {
        poll_dma();
        return;
    }
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
    frame_done(rc, dt);
}

const volatile audio_link_stats_t *audio_link_stats(void) { return &st; }
#endif
