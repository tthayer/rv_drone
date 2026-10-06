#include "panel_link.h"
#include "board.h"

#ifndef BOARD_HAS_PANEL_LINK
static const volatile panel_link_stats_t st;
int panel_link_init(void) { return -1; }
int panel_link_next_event(panel_event_t *e) { (void)e; return 0; }
int panel_link_send(uint8_t type, const void *p, unsigned len) { (void)type; (void)p; (void)len; return -1; }
unsigned panel_link_tx_free(void) { return 0; }
void panel_link_poll(void) {}
void panel_link_tick(void) {}
const volatile panel_link_stats_t *panel_link_stats(void) { return &st; }
#else
#include "rvpanel.h"
#include "pinmux.h"
#include "plic.h"

#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define REG(n) R32(BOARD_UART2_BASE + ((uintptr_t)(n) << 2))
/* DW apb uart (16550 + USR) */
#define RBR 0
#define THR 0
#define DLL 0
#define DLH 1
#define IER 1
#define IIR 2
#define FCR 2
#define LCR 3
#define MCR 4
#define LSR 5
#define USR 31
#define LSR_DR    0x01
#define LSR_OE    0x02
#define USR_BUSY  0x01
#define USR_TFNF  0x02
#define IIR_BUSY  0x7
#define CLK_EN_1     0x004          /* bit 18 clk_uart2, bit 19 clk_apb_uart2 */
#define SOFT_RSTN_0  0x000          /* bit 25 UART2 (active low) */

static volatile panel_link_stats_t st;
static rvpanel_rx_t rx;              /* ISR only */

/* Events: SPSC, ISR writes head, main loop writes tail. */
#define EV_N 256
static panel_event_t ev[EV_N];
static volatile uint32_t ev_head, ev_tail;

/* TX ring: producer = main loop (panel_link_send), consumer = drain(), which runs
 * from the tick IRQ and the main loop with SIE off, so it never races itself. */
#define TX_N 8192
static uint8_t tx_buf[TX_N];
static volatile uint32_t tx_head, tx_tail;

static void push_event(uint8_t type, const uint8_t *p, size_t n)
{
    panel_event_t e = { type, 0, 0, 0, 0 };
    if (n > 0) e.a = p[0];
    if (n > 1) e.b = p[1];
    if (n > 2) e.c = p[2];
    if (n > 3) e.d = p[3];
    uint32_t h = ev_head;
    if (h - ev_tail >= EV_N) {
        st.evt_dropped++;
        return;
    }
    ev[h % EV_N] = e;
    ev_head = h + 1;
}

static void handle_packet(const uint8_t *buf, size_t plen)
{
    const uint8_t *p = buf + 1;
    switch (buf[0]) {
    case RVPANEL_ENC:
    case RVPANEL_SW:
        if (plen >= 2) push_event(buf[0], p, 2);
        break;
    case RVPANEL_MIDI:
        if (plen >= 4 && p[0] >= 1 && p[0] <= 3) push_event(buf[0], p, 4);
        break;
    case RVPANEL_STATUS:
        if (plen >= 10) {
            st.peer_rx_ok = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                            (uint32_t)p[3] << 24;
            st.peer_crc_err = (uint16_t)(p[4] | p[5] << 8);
            st.peer_cobs_err = (uint16_t)(p[6] | p[7] << 8);
            st.peer_dropped = (uint16_t)(p[8] | p[9] << 8);
            st.peer_status_count++;
        }
        break;
    }
}

static void uart2_isr(void)
{
    if ((REG(IIR) & 0xf) == IIR_BUSY)
        (void)REG(USR);                    /* clear DW busy-detect */
    uint32_t lsr;
    while ((lsr = REG(LSR)) & LSR_DR) {
        if (lsr & LSR_OE)
            st.rx_overrun++;
        uint8_t b = (uint8_t)REG(RBR);
        st.rx_bytes++;
        size_t plen;
        if (rvpanel_rx_feed(&rx, b, &plen))
            handle_packet(rx.buf, plen);
    }
    st.rx_ok = rx.ok;
    st.rx_crc_err = rx.crc_err;
    st.rx_cobs_err = rx.cobs_err;
}

static void drain(void)
{
    uint32_t t = tx_tail;
    while (t != tx_head && (REG(USR) & USR_TFNF))
        REG(THR) = tx_buf[t++ % TX_N];
    tx_tail = t;
}

void panel_link_tick(void) { drain(); }

void panel_link_poll(void)
{
    uint64_t s;
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(s));
    drain();
    if (s & 2)
        __asm__ volatile("csrsi sstatus, 2");
}

unsigned panel_link_tx_free(void) { return TX_N - (tx_head - tx_tail); }

int panel_link_send(uint8_t type, const void *payload, unsigned len)
{
    uint8_t w[RVPANEL_MAX_WIRE];
    if (len > RVPANEL_MAX_RAW - 3)
        return -1;
    size_t n = rvpanel_encode(type, payload, len, w);
    uint32_t h = tx_head;
    if (TX_N - (h - tx_tail) < n) {
        st.tx_dropped++;
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        tx_buf[(h + i) % TX_N] = w[i];
    __asm__ volatile("" ::: "memory");
    tx_head = h + (uint32_t)n;
    st.tx_packets++;
    return 0;
}

int panel_link_next_event(panel_event_t *e)
{
    uint32_t t = ev_tail;
    if (t == ev_head)
        return 0;
    *e = ev[t % EV_N];
    ev_tail = t + 1;
    return 1;
}

int panel_link_init(void)
{
    R32(BOARD_CLKGEN_BASE + CLK_EN_1) |= (1u << 18) | (1u << 19);
    R32(BOARD_RSTGEN_BASE + SOFT_RSTN_0) |= 1u << 25;       /* out of reset */
    pinmux_set(BOARD_FMUX_UART2_TX, BOARD_FMUX_UART2_FN);
    pinmux_set(BOARD_FMUX_UART2_RX, BOARD_FMUX_UART2_FN);
    rvpanel_rx_init(&rx);

    REG(IER) = 0;
    for (int i = 0; i < 100000 && (REG(USR) & USR_BUSY); i++)
        ;
    uint32_t div = (uint32_t)(BOARD_UART2_CLK_HZ / (16u * RVPANEL_BAUD));   /* = 1 */
    REG(LCR) = 0x80;                        /* DLAB */
    REG(DLL) = div & 0xff;
    REG(DLH) = div >> 8;
    REG(LCR) = 0x03;                        /* 8N1 */
    if (REG(LCR) != 0x03)
        return -1;                          /* clock/reset not on: registers dead */
    REG(FCR) = 0x87;                        /* FIFOs on + reset, RX trigger 1/2 */
    REG(MCR) = 0;
    (void)REG(USR);
    (void)REG(IIR);
    while (REG(LSR) & LSR_DR)
        (void)REG(RBR);
    plic_register(BOARD_UART2_IRQ, uart2_isr);
    REG(IER) = 0x01;                        /* ERBFI (+ char timeout) */
    return 0;
}

const volatile panel_link_stats_t *panel_link_stats(void) { return &st; }
#endif
