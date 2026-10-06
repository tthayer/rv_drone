/* DW APB SSI master on SPI2 (0x041A0000, "snps,dw-apb-ssi", not DWC_ssi):
 * CTRLR0 DFS[3:0] FRF[5:4] SCPH[6] SCPOL[7] TMOD[9:8] (SDK spi-dw-core.c:270-322).
 * CS: P18 is driven as a GPIO and held low for the whole frame. The controller's
 * own CS blips whenever the TX FIFO momentarily empties, which the Pico slave
 * read as short frames. SER=1 is still needed to clock. spi_xfer() keeps the
 * FIFO topped up and masks S-mode interrupts for the ~0.55 ms frame.
 * spi_xfer_dma_start() is the same frame done by the sysDMA (dma.c) with
 * T-Head cache ops (cache.c): one TX and one RX channel on the SSI's hardware
 * handshake, IRQs stay on, completion arrives as a callback from the DMA IRQ. */
#include "spi.h"
#include "board.h"
#include "pinmux.h"
#include "timer.h"
#include "uart.h"
#ifdef BOARD_HAS_DMA
#include "cache.h"
#include "dma.h"
#endif

#define R32(base, o) (*(volatile uint32_t *)(uintptr_t)((base) + (o)))
#define SSI(o)  R32(BOARD_SPI_BASE, o)
#define CLK(o)  R32(BOARD_CLKGEN_BASE, o)
#define RST(o)  R32(BOARD_RSTGEN_BASE, o)

#define CTRLR0 0x00
#define SSIENR 0x08
#define SER    0x10
#define BAUDR  0x14
#define TXFTLR 0x18
#define RXFTLR 0x1c
#define RXFLR  0x24
#define SR     0x28
#define RISR   0x34
#define ICR    0x48
#define DMACR  0x4c                /* [0] RDMAE [1] TDMAE */
#define DMATDLR 0x50               /* TX request while level <= TDLR */
#define DMARDLR 0x54               /* RX request while level >= RDLR + 1 */
#define IDR    0x58
#define VERSION 0x5c
#define DR     0x60

#define SR_BUSY 1u
#define RISR_ERR 0x0eu             /* TXOI | RXUI | RXOI */

#define CLK_EN_1      0x004        /* bit 11: clk_apb_spi2 */
#define CLK_EN_3      0x00c        /* bit 6: clk_spi */
#define CLK_BYP_0     0x030        /* bit 30: clk_spi bypass to xtal (1 = 25 MHz) */
#define DIV_CLK_SPI   0x100        /* [3] use reg factor else 8; [20:16] factor */
#define FPLL_CSR      0x910        /* [6:0] pre, [14:8] post, [23:17] div */
#define SOFT_RSTN_1   0x004        /* bit 10: SPI2 (active low) */

#define XTAL_HZ     25000000u
#define FPLL_DEF_HZ 1500000000u    /* FSBL comment, platform.c:223 */
#define CTRLR0_MODE3_8BIT_TR  (7u | (3u << 6))   /* DFS=8bit, FRF=SPI, SCPH=1, SCPOL=1, TMOD=TR */

static uint32_t fifo_depth, sck_hz, in_hz;

static void delay_us(unsigned us)
{
    uint64_t t = rdtime() + (uint64_t)us * (BOARD_TIMEBASE_HZ / 1000000u);
    while (rdtime() < t)
        ;
}

uint32_t spi_clk_in_hz(void)
{
    if (CLK(CLK_BYP_0) & (1u << 30))
        return XTAL_HZ;
    uint32_t d = CLK(DIV_CLK_SPI);
    d = (d & 8u) ? (d >> 16) & 0x1f : 8u;
    if (d == 0)
        d = 8;
    uint32_t c = CLK(FPLL_CSR);
    uint32_t pre = c & 0x7f, post = (c >> 8) & 0x7f, div = (c >> 17) & 0x7f;
    uint64_t pll = (pre && post && div) ? (uint64_t)XTAL_HZ * div / (pre * post) : 0;
    if (pll < 400000000u || pll > 2000000000u)   /* decode doesn't look like FPLL */
        pll = FPLL_DEF_HZ;
    return (uint32_t)(pll / d);
}

int spi_init(uint32_t target_hz)
{
    pinmux_set(BOARD_FMUX_SPI_CS, BOARD_SPI_CS_FN);
    R32(BOARD_SPI_CS_GPIO, 0x00) |= 1u << BOARD_SPI_CS_BIT;   /* DR: CS high */
    R32(BOARD_SPI_CS_GPIO, 0x04) |= 1u << BOARD_SPI_CS_BIT;   /* DDR: output */
    pinmux_set(BOARD_FMUX_SPI_MISO, 1);
    pinmux_set(BOARD_FMUX_SPI_MOSI, 1);
    pinmux_set(BOARD_FMUX_SPI_SCK, 1);

    CLK(CLK_EN_1) |= 1u << 11;                    /* gates reset to 1; make sure */
    CLK(CLK_EN_3) |= 1u << 6;
    RST(SOFT_RSTN_1) &= ~(1u << 10);              /* pulse the IP reset */
    delay_us(10);
    RST(SOFT_RSTN_1) |= 1u << 10;
    delay_us(10);

    SSI(SSIENR) = 0;
    SSI(SER) = 0;
    uint32_t f;                                   /* FIFO depth probe (spi-dw-core.c:820) */
    for (f = 1; f < 256; f++) {
        SSI(TXFTLR) = f;
        if (SSI(TXFTLR) != f)
            break;
    }
    SSI(TXFTLR) = 0;
    SSI(RXFTLR) = 0;
    fifo_depth = f;

    in_hz = spi_clk_in_hz();
    uint32_t div = ((in_hz + target_hz - 1) / target_hz + 1) & ~1u;   /* even, SCK <= target */
    if (div < 2)
        div = 2;
    SSI(BAUDR) = div;
    SSI(CTRLR0) = CTRLR0_MODE3_8BIT_TR;
    sck_hz = in_hz / div;
    return SSI(CTRLR0) == CTRLR0_MODE3_8BIT_TR ? 0 : -1;
}

uint32_t spi_sck_hz(void) { return sck_hz; }
uint32_t spi_fifo_depth(void) { return fifo_depth; }

void spi_dump(void)
{
    uart_puts("spi: id="); uart_put_hex(SSI(IDR));
    uart_puts(" ver="); uart_put_hex(SSI(VERSION));
    uart_puts(" ctrlr0="); uart_put_hex(SSI(CTRLR0));
    uart_puts(" baudr="); uart_put_dec(SSI(BAUDR));
    uart_puts(" fifo="); uart_put_dec(fifo_depth);
    uart_puts("\nspi: byp0="); uart_put_hex(CLK(CLK_BYP_0));
    uart_puts(" div="); uart_put_hex(CLK(DIV_CLK_SPI));
    uart_puts(" fpll="); uart_put_hex(CLK(FPLL_CSR));
    uart_puts(" en1="); uart_put_hex(CLK(CLK_EN_1));
    uart_puts(" en3="); uart_put_hex(CLK(CLK_EN_3));
    uart_puts("\nspi: in clk "); uart_put_dec(in_hz);
    uart_puts(" Hz, SCK "); uart_put_dec(sck_hz);
    uart_puts(" Hz\n");
}

int spi_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    uint64_t sst;
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(sst));   /* SIE off for the frame */

    const size_t cap = fifo_depth - 1;       /* in flight: TX fifo + shifter + RX fifo */
    size_t sent = 0, got = 0;
    int rc = 0;

    /* CS low before anything can clock: the DW SSI may start shifting as soon
     * as the FIFO has data, whatever SER says. */
    R32(BOARD_SPI_CS_GPIO, 0x00) &= ~(1u << BOARD_SPI_CS_BIT);
    SSI(SER) = 1;
    SSI(SSIENR) = 1;
    (void)SSI(ICR);
    while (sent < n && sent - got < cap)     /* prefill */
        SSI(DR) = tx[sent++];

    uint64_t dead = rdtime() + BOARD_TIMEBASE_HZ / 100;      /* 10 ms */
    while (got < n) {
        for (uint32_t r = SSI(RXFLR); r; r--)
            rx[got++] = (uint8_t)SSI(DR);
        while (sent < n && sent - got < cap)
            SSI(DR) = tx[sent++];
        if (rdtime() > dead) {
            rc = -1;
            break;
        }
    }
    while (rc == 0 && (SSI(SR) & SR_BUSY))
        ;
    if (rc == 0)
        rc = (int)(SSI(RISR) & RISR_ERR);
    R32(BOARD_SPI_CS_GPIO, 0x00) |= 1u << BOARD_SPI_CS_BIT;     /* CS high */
    SSI(SER) = 0;
    SSI(SSIENR) = 0;
    (void)SSI(ICR);
    if (sst & 2)
        __asm__ volatile("csrsi sstatus, 2");
    return rc;
}

#ifdef BOARD_HAS_DMA
/* DMA channels / handshake slots: RX = 0, TX = 1 (slot -> SPI2 request line via
 * sdma_dma_ch_remap0, TRM: dma_rx_req_spi2 = 20, dma_tx_req_spi2 = 21). */
#define DMA_CH_RX   0u
#define DMA_CH_TX   1u
#define DMA_SPI_WAIT_TICKS (BOARD_TIMEBASE_HZ / 20000)    /* 50 us for BUSY to clear */

static struct {
    spi_done_fn cb;
    uint8_t *rx;
    size_t n;
    volatile int active;
} sd;
static int dma_ready;

static void spi_dma_hw_off(void)
{
    R32(BOARD_SPI_CS_GPIO, 0x00) |= 1u << BOARD_SPI_CS_BIT;     /* CS high */
    SSI(SER) = 0;
    SSI(DMACR) = 0;
    SSI(SSIENR) = 0;
    (void)SSI(ICR);
}

static void spi_dma_finish(int rc)
{
    if (rc == 0 && dma_busy(DMA_CH_TX)) {          /* RX done but TX channel still going */
        dma_abort(DMA_CH_TX);
        rc = -2;
    }
    uint64_t dead = rdtime() + DMA_SPI_WAIT_TICKS;
    while ((SSI(SR) & SR_BUSY) && rdtime() < dead)
        ;
    if (rc == 0)
        rc = (SSI(SR) & SR_BUSY) ? -4 : (int)(SSI(RISR) & RISR_ERR);
    spi_dma_hw_off();
    cache_inval(sd.rx, sd.n);                      /* drop any lines prefetched during the frame */
    spi_done_fn cb = sd.cb;
    sd.active = 0;
    if (cb)
        cb(rc);
}

static void spi_dma_rx_cb(unsigned ch, uint32_t st)
{
    (void)ch;
    if (!sd.active)
        return;
    if (st & DMA_ST_ERR) {
        dma_abort(DMA_CH_TX);
        spi_dma_finish(-3);
    } else if (st & DMA_ST_DONE) {
        spi_dma_finish(0);
    }
}

static void spi_dma_tx_cb(unsigned ch, uint32_t st)
{
    (void)ch;
    if (sd.active && (st & DMA_ST_ERR)) {
        dma_abort(DMA_CH_RX);
        spi_dma_finish(-3);
    }
}

int spi_dma_init(void)
{
    dma_ready = 0;
    cache_init();
    if (!cache_ok() || fifo_depth < 4)
        return -1;
    if (dma_init() != 0)
        return -2;
    dma_map_request(DMA_CH_RX, BOARD_SPI_DMA_RX_REQ);
    dma_map_request(DMA_CH_TX, BOARD_SPI_DMA_TX_REQ);
    dma_ready = 1;
    return 0;
}

int spi_dma_busy(void) { return sd.active; }

int spi_xfer_dma_start(const uint8_t *tx, uint8_t *rx, size_t n, spi_done_fn cb)
{
    if (!dma_ready || sd.active || n == 0 || n > DMA_MAX_BLOCK)
        return -1;
    sd.cb = cb;
    sd.rx = rx;
    sd.n = n;

    cache_clean(tx, n);                            /* device reads TX from DDR */
    cache_flush(rx, n);                            /* no dirty RX line may be written back later */

    /* Enable the SSI (CS low first) BEFORE arming the DMA. With the SSI
     * disabled, its TX request was already asserted, so the DMAC wrote the
     * first 16 bytes into a FIFO held in reset. They were lost, Pico A saw
     * 512-byte frames, and the RX channel waited forever for 16 more. */
    SSI(SSIENR) = 0;
    SSI(DMATDLR) = fifo_depth / 2;
    SSI(DMARDLR) = 0;                        /* RX request at >= 1 entry */
    R32(BOARD_SPI_CS_GPIO, 0x00) &= ~(1u << BOARD_SPI_CS_BIT);  /* CS low */
    SSI(SER) = 1;
    SSI(SSIENR) = 1;                               /* FIFOs live; nothing to send yet */
    SSI(DMACR) = 3;                                /* TDMAE | RDMAE */
    sd.active = 1;

    if (dma_start_p2m(DMA_CH_RX, DMA_CH_RX, BOARD_SPI_BASE + DR, rx, n, spi_dma_rx_cb) != 0 ||
        dma_start_m2p(DMA_CH_TX, DMA_CH_TX, tx, BOARD_SPI_BASE + DR, n, spi_dma_tx_cb) != 0) {
        dma_abort(DMA_CH_RX);
        dma_abort(DMA_CH_TX);
        sd.active = 0;
        spi_dma_hw_off();
        return -2;
    }
    /* TX DMA now fills the live FIFO and clocks start. */
    (void)SSI(ICR);
    return 0;
}

void spi_xfer_dma_abort(void)
{
    uint64_t sst;
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(sst));
    dma_abort(DMA_CH_RX);
    dma_abort(DMA_CH_TX);
    sd.active = 0;
    spi_dma_hw_off();
    if (sst & 2)
        __asm__ volatile("csrsi sstatus, 2");
}
#else
int spi_dma_init(void) { return -1; }
int spi_dma_busy(void) { return 0; }
int spi_xfer_dma_start(const uint8_t *tx, uint8_t *rx, size_t n, spi_done_fn cb)
{ (void)tx; (void)rx; (void)n; (void)cb; return -1; }
void spi_xfer_dma_abort(void) {}
#endif

void spi_dump_rt(void)           /* bring-up: live SSI state */
{
    uart_puts("ssi: en ");  uart_put_hex(SSI(SSIENR));
    uart_puts(" sr ");      uart_put_hex(SSI(SR));
    uart_puts(" txflr ");   uart_put_hex(SSI(0x20));
    uart_puts(" rxflr ");   uart_put_hex(SSI(RXFLR));
    uart_puts(" risr ");    uart_put_hex(SSI(RISR));
    uart_puts(" dmacr ");   uart_put_hex(SSI(DMACR));
    uart_puts(" ser ");     uart_put_hex(SSI(0x10));
    uart_puts(" ctrlr0 ");  uart_put_hex(SSI(0x00));
    uart_putc('\n');
}
