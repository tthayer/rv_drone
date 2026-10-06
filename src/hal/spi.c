/* DW APB SSI master on SPI2 (0x041A0000, "snps,dw-apb-ssi", not DWC_ssi):
 * CTRLR0 DFS[3:0] FRF[5:4] SCPH[6] SCPOL[7] TMOD[9:8] (SDK spi-dw-core.c:270-322).
 * CS: P18 is driven as a GPIO and held low for the whole frame. The controller's
 * own CS blips whenever the TX FIFO momentarily empties, which the Pico slave
 * read as short frames. SER=1 is still needed to clock. spi_xfer() keeps the
 * FIFO topped up and masks S-mode interrupts for the ~0.55 ms frame.
 * TODO(DMA): later via the DW AXI DMAC at 0x04330000 (+ T-Head cache ops); then
 * the frame is a single descriptor and the IRQ masking goes away. */
#include "spi.h"
#include "board.h"
#include "pinmux.h"
#include "timer.h"
#include "uart.h"

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
