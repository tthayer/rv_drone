/* SG2002 sysDMA driver (DW_axi_dmac), S-mode, bare metal.
 * Base 0x04330000, PLIC 29 on the big C906 (TRM interrupt map; SDK
 * cv181x_base_riscv.dtsi:141 "dma@0x4330000 interrupts = <29>"), clk_sdma_axi
 * = CLK_EN_1[1], reset = SOFT_RSTN_0[18] (TRM clock/reset chapters). The
 * controller's IRQ is muxed per CPU by sdma_dma_int_mux (0x03000298): bits
 * [18:10] route {cmnreg, ch[7:0]} to the big C906 (SDK dts sets 0x7FC00).
 * Register layout follows linux_5.10/drivers/dma/cvitek/cvitek-dma.h
 * (struct dw_dma_regs / dw_dma_chan_regs: common regs 0x00-0x58, channel n at
 * 0x100 + 0x100*n, 64-bit registers). The vendor driver only ever uses linked
 * list transfers with the LLI in DDR; DMA_USE_LLI=1 (default) does the same
 * with one descriptor, DMA_USE_LLI=0 programs the channel registers directly
 * (contiguous multi-block type, TRM "Basic Transfer"). Both UNVERIFIED here. */
#include "dma.h"
#include "board.h"

#ifndef BOARD_HAS_DMA
volatile uint32_t dma_irqs, dma_errs;
int dma_init(void) { return -1; }
void dma_map_request(unsigned slot, unsigned periph_req) { (void)slot; (void)periph_req; }
int dma_start_m2p(unsigned ch, unsigned slot, const void *mem, uintptr_t reg, size_t n, dma_fn cb)
{ (void)ch; (void)slot; (void)mem; (void)reg; (void)n; (void)cb; return -1; }
int dma_start_p2m(unsigned ch, unsigned slot, uintptr_t reg, void *mem, size_t n, dma_fn cb)
{ (void)ch; (void)slot; (void)reg; (void)mem; (void)n; (void)cb; return -1; }
void dma_abort(unsigned ch) { (void)ch; }
void dma_dump(unsigned ch) { (void)ch; }
int dma_busy(unsigned ch) { (void)ch; return 0; }
uint32_t dma_id(void) { return 0; }
#else
#include "cache.h"
#include "uart.h"
#include "plic.h"
#include "timer.h"

#ifndef DMA_USE_LLI
#define DMA_USE_LLI 1
#endif

#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define R64(a) (*(volatile uint64_t *)(uintptr_t)(a))
#define D(o)      R64(BOARD_DMAC_BASE + (o))
#define CH(c, o)  R64(BOARD_DMAC_BASE + 0x100u + 0x100u * (c) + (o))
#define SYS(o)    R32(BOARD_SYSCTL_BASE + (o))

/* common */
#define D_ID        0x00
#define D_CFG       0x10            /* [0] DMAC_EN [1] INT_EN */
#define D_CHEN      0x18            /* [n] EN, [8+n] EN write-enable, [32+n] abort, [40+n] abort we */
#define D_INTSTATUS 0x30            /* [n] ch n, [16] common */
#define D_COMM_CLR  0x38
#define D_RESET     0x58
/* channel */
#define C_SAR   0x00
#define C_DAR   0x08
#define C_BLK   0x10                /* BLOCK_TS = items - 1 */
#define C_CTL   0x18
#define C_CFG   0x20
#define C_LLP   0x28
#define C_ISE   0x80                /* INTSTATUS_ENABLEREG */
#define C_IST   0x88                /* INTSTATUS */
#define C_ISG   0x90                /* INTSIGNAL_ENABLEREG */
#define C_ICL   0x98                /* INTCLEARREG */

#define SYS_REMAP0   0x154          /* ch n at [8n+5:8n]; bit 31 update (W1T) */
#define SYS_INT_MUX  0x298          /* cpu1 (big C906) enables: [18:10] */
#define CLK_EN_1     0x004          /* bit 1: clk_sdma_axi */
#define SOFT_RSTN_0  0x000          /* bit 18: SDMA (active low) */

/* CHx_CTL */
#define CTL_SMS(m)      ((uint64_t)(m) << 0)
#define CTL_DMS(m)      ((uint64_t)(m) << 2)
#define CTL_SINC_FIX    (1ull << 4)
#define CTL_DINC_FIX    (1ull << 6)
#define CTL_SRC_MSIZE(x) ((uint64_t)(x) << 14)     /* 0:1 1:4 2:8 3:16 items */
#define CTL_DST_MSIZE(x) ((uint64_t)(x) << 18)
#define CTL_IOC_BLK     (1ull << 58)
#define CTL_LLI_LAST    (1ull << 62)
#define CTL_LLI_VALID   (1ull << 63)
/* CHx_CFG */
#define CFG_TT_FC(x)    ((uint64_t)(x) << 32)      /* 1 mem->per, 2 per->mem (DMAC flow ctl) */
#define CFG_SRC_PER(x)  ((uint64_t)(x) << 39)
#define CFG_DST_PER(x)  ((uint64_t)(x) << 44)
#define CFG_PRIO(x)     ((uint64_t)(x) << 49)
#define CFG_SRC_OSR(x)  ((uint64_t)(x) << 55)
#define CFG_DST_OSR(x)  ((uint64_t)(x) << 59)
#define CFG_MBLK_LLI    0xfull                     /* SRC/DST_MULTBLK_TYPE = linked list */

#define MSIZE_8   2u               /* matches SSI DMATDLR/DMARDLR thresholds of fifo/2 */

struct lli {                        /* 64 B, cvitek-dma.h struct dw_lli (LLI mode) */
    uint64_t sar, dar, block_ts, llp, ctl, sstat_dstat, llp_status, rsvd;
};

#if DMA_USE_LLI
static struct lli lli_tab[8] __attribute__((aligned(64)));
#endif
static dma_fn cbs[8];
volatile uint32_t dma_irqs, dma_errs;

static void delay_us(unsigned us)
{
    uint64_t t = rdtime() + (uint64_t)us * (BOARD_TIMEBASE_HZ / 1000000u);
    while (rdtime() < t)
        ;
}

static void dma_isr(void)
{
    uint64_t st = D(D_INTSTATUS);
    dma_irqs++;
    if (st & (1u << 16)) {                       /* common-register error */
        D(D_COMM_CLR) = 0x10f;
        dma_errs++;
    }
    for (unsigned c = 0; c < 8; c++) {
        if (!(st & (1u << c)))
            continue;
        uint32_t s = (uint32_t)CH(c, C_IST);
        CH(c, C_ICL) = s;
        if (s & DMA_ST_ERR)
            dma_errs++;
        if (cbs[c])
            cbs[c](c, s);
    }
}

uint32_t dma_id(void) { return (uint32_t)D(D_ID); }

int dma_init(void)
{
    R32(BOARD_CLKGEN_BASE + CLK_EN_1) |= 1u << 1;
    R32(BOARD_RSTGEN_BASE + SOFT_RSTN_0) &= ~(1u << 18);       /* pulse the IP reset */
    delay_us(10);
    R32(BOARD_RSTGEN_BASE + SOFT_RSTN_0) |= 1u << 18;
    delay_us(10);

    SYS(SYS_INT_MUX) |= 0x1ffu << 10;                           /* sysDMA IRQs -> big C906 */

    D(D_RESET) = 1;                                             /* self-clearing */
    for (unsigned i = 0; i < 1000 && (D(D_RESET) & 1); i++)
        delay_us(1);
    D(D_CHEN) = 0xff00;                                         /* all channels off */
    for (unsigned c = 0; c < 8; c++) {
        CH(c, C_ISE) = 0;
        CH(c, C_ISG) = 0;
        CH(c, C_ICL) = 0xffffffffu;
        cbs[c] = 0;
    }
    D(D_COMM_CLR) = 0x10f;
    D(D_CFG) = 3;                                               /* DMAC_EN | INT_EN */
    if ((D(D_CFG) & 3) != 3)
        return -1;
    plic_register(BOARD_DMAC_IRQ, dma_isr);
    return 0;
}

void dma_map_request(unsigned slot, unsigned periph_req)
{
    uint32_t o = SYS_REMAP0 + 4u * (slot / 4), sh = 8u * (slot % 4);
    uint32_t v = SYS(o) & ~(1u << 31);
    v = (v & ~(0x3fu << sh)) | ((periph_req & 0x3fu) << sh);
    SYS(o) = v | (1u << 31);                                    /* update */
}

static int start(unsigned ch, unsigned slot, uintptr_t src, uintptr_t dst, size_t n,
                 int m2p, dma_fn cb)
{
    if (ch >= 8 || slot >= 8 || n == 0 || n > DMA_MAX_BLOCK)
        return -1;
    if (D(D_CHEN) & (1u << ch))
        return -2;

    uint64_t ctl = CTL_SMS(1) | CTL_DMS(1)                      /* vendor dts: dmas <&dmac N 1 1> */
                 | (m2p ? CTL_DINC_FIX : CTL_SINC_FIX)          /* 8-bit src/dst width = 0 */
                 | CTL_SRC_MSIZE(0) | CTL_DST_MSIZE(0) | CTL_IOC_BLK;   /* 1-item bursts */
    /* P2M: write memory at the widest width the buffer allows (64-bit), as the
     * vendor driver does (cvitek-dma.c: DST_WIDTH(mem_width)). With 8-bit
     * memory writes, the DMAC kept the last 16 bytes and never finished the
     * block. BLOCK_TS stays in source (8-bit) units. */
    if (!m2p && (dst % 8) == 0 && (n % 8) == 0)
        ctl |= (uint64_t)3 << 11;                               /* DST_TR_WIDTH = 64-bit */
    uint64_t cfg = CFG_TT_FC(m2p ? 1 : 2) | CFG_SRC_PER(slot) | CFG_DST_PER(slot)
                 | CFG_SRC_OSR(0) | CFG_DST_OSR(0) | CFG_PRIO(m2p ? 0 : 1);

    cbs[ch] = cb;
    CH(ch, C_ICL) = 0xffffffffu;
    CH(ch, C_ISE) = DMA_ST_DONE | DMA_ST_ERR;
    CH(ch, C_ISG) = DMA_ST_DONE | DMA_ST_ERR;
#if DMA_USE_LLI
    struct lli *l = &lli_tab[ch];
    l->sar = src;
    l->dar = dst;
    l->block_ts = n - 1;
    l->llp = 0;
    l->ctl = ctl | CTL_LLI_LAST | CTL_LLI_VALID;
    cache_clean(l, sizeof *l);
    CH(ch, C_CFG) = cfg | CFG_MBLK_LLI;
    CH(ch, C_LLP) = (uintptr_t)l;
#else
    CH(ch, C_SAR) = src;
    CH(ch, C_DAR) = dst;
    CH(ch, C_BLK) = n - 1;
    CH(ch, C_CTL) = ctl;
    CH(ch, C_CFG) = cfg;                                        /* multi-block type 0 = contiguous */
#endif
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    D(D_CHEN) = (1ull << ch) | (1ull << (ch + 8));
    return 0;
}

int dma_start_m2p(unsigned ch, unsigned slot, const void *mem, uintptr_t reg, size_t n, dma_fn cb)
{
    return start(ch, slot, (uintptr_t)mem, reg, n, 1, cb);
}

int dma_start_p2m(unsigned ch, unsigned slot, uintptr_t reg, void *mem, size_t n, dma_fn cb)
{
    return start(ch, slot, reg, (uintptr_t)mem, n, 0, cb);
}

void dma_abort(unsigned ch)
{
    if (ch >= 8)
        return;
    cbs[ch] = 0;
    if (D(D_CHEN) & (1u << ch)) {
        D(D_CHEN) = (1ull << (ch + 40)) | (1ull << (ch + 32)) | (1ull << (ch + 8)); /* abort */
        for (unsigned i = 0; i < 100 && (D(D_CHEN) & (1u << ch)); i++)
            delay_us(1);
    }
    CH(ch, C_ICL) = 0xffffffffu;
}

int dma_busy(unsigned ch) { return ch < 8 && (D(D_CHEN) & (1u << ch)) != 0; }
void dma_dump(unsigned ch)       /* bring-up */
{
    uart_puts("dma: cfg ");   uart_put_hex(D(D_CFG));
    uart_puts(" chen ");      uart_put_hex(D(D_CHEN));
    uart_puts(" ist ");       uart_put_hex(D(D_INTSTATUS));
    uart_puts(" irqs ");      uart_put_dec(dma_irqs);
    uart_puts("\n  ch ");     uart_put_dec(ch);
    uart_puts(" sar ");       uart_put_hex(CH(ch, C_SAR));
    uart_puts(" dar ");       uart_put_hex(CH(ch, C_DAR));
    uart_puts(" blk ");       uart_put_hex(CH(ch, C_BLK));
    uart_puts("\n  ctl ");    uart_put_hex(CH(ch, C_CTL));
    uart_puts(" cfg ");       uart_put_hex(CH(ch, C_CFG));
    uart_puts(" llp ");       uart_put_hex(CH(ch, C_LLP));
    uart_puts(" ist ");       uart_put_hex(CH(ch, C_IST));
    uart_puts(" status ");    uart_put_hex(CH(ch, 0x30));   /* [21:0] items done */
    uart_puts("\n  remap ");  uart_put_hex(SYS(SYS_REMAP0));
    uart_puts(" intmux ");    uart_put_hex(SYS(SYS_INT_MUX));
    uart_putc('\n');
}
#endif
