#include "cpuclk.h"
#include "board.h"
#include "timer.h"
#include "trap.h"
#include "uart.h"

#ifdef BOARD_NANO
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define CLK_SEL_0        (BOARD_CLKGEN_BASE + 0x020)   /* [23] 1 = div_clk_c906_0_0 */
#define CLK_BYP_1        (BOARD_CLKGEN_BASE + 0x034)   /* [6] c906_0 bypassed to xtal */
#define DIV_C906_0_0     (BOARD_CLKGEN_BASE + 0x130)   /* src [9:8]: 0 tpll 1 apll 2 mipimpll 3 mpll */
#define DIV_C906_0_1     (BOARD_CLKGEN_BASE + 0x134)   /* src fpll */
#define PLL_G6           0x03002900UL
#define PLL_G6_CTRL      (PLL_G6 + 0x000)              /* [0] mpll_pwd */
#define PLL_G6_STATUS    (PLL_G6 + 0x004)              /* [16] mpll_lock */
#define MPLL_CSR         (PLL_G6 + 0x008)
#define G6_SSC_SYN_CTRL  (PLL_G6 + 0x040)              /* [2] mpll synthesizer clock enable */
#define MPLL_SSC_CTRL    (PLL_G6 + 0x060)              /* [4] synthesizer bypass */
#define MPLL_SSC_SET     (PLL_G6 + 0x064)              /* 6.26 fixed point */

static volatile int probing, trapped;
static const char *note = "not attempted";

int cpuclk_probe_trap(struct trap_frame *f)
{
    if (!probing)
        return 0;
    trapped = 1;
    f->sepc += 4;                        /* skip rdcycle (csrr, 4 bytes) */
    return 1;
}

static int read_cycle(uint64_t *c)
{
    uint64_t v = 0;
    trapped = 0;
    probing = 1;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    probing = 0;
    *c = v;
    return !trapped;
}

uint32_t cpuclk_measure_mhz(void)
{
    uint64_t s, c0, c1;
    __asm__ volatile("csrrci %0, sstatus, 2" : "=r"(s));
    int ok = read_cycle(&c0);
    uint64_t t0 = rdtime();
    while (rdtime() - t0 < BOARD_TIMEBASE_HZ / 400)    /* 2.5 ms */
        ;
    ok = ok && read_cycle(&c1);
    uint64_t dt = rdtime() - t0;
    if (s & 2)
        __asm__ volatile("csrsi sstatus, 2");
    if (!ok || c1 <= c0)
        return 0;
    return (uint32_t)(((c1 - c0) * BOARD_TIMEBASE_HZ / dt + 500000) / 1000000);
}

uint32_t cpuclk_mpll_mhz(void)
{
    if (R32(PLL_G6_CTRL) & 1u)                         /* powered down */
        return 0;
    uint32_t csr = R32(MPLL_CSR);
    uint32_t pre = csr & 0x7f, post = (csr >> 8) & 0x7f, div = (csr >> 17) & 0x7f;
    if (!post || !div)
        return 0;
    uint64_t vco_hz;
    int syn = (R32(G6_SSC_SYN_CTRL) & (1u << 2)) && !(R32(MPLL_SSC_CTRL) & (1u << 4));
    if (syn) {                                         /* fractional: REF = 600 MHz * 2^26 / set */
        uint32_t set = R32(MPLL_SSC_SET);
        if (!set)
            return 0;
        vco_hz = 600000000ull * div * (1ull << 26) / set;
    } else {                                           /* integer: REF = 25 MHz xtal */
        if (!pre)
            return 0;
        vco_hz = 25000000ull * div / pre;
    }
    return (uint32_t)(vco_hz / post / 1000000);
}

uint32_t cpuclk_set_mhz(uint32_t mhz)
{
    uint32_t before = cpuclk_measure_mhz();
    if (mhz != 1000) {
        note = "only 1000 MHz (MPLL/1) is supported";
        return before;
    }
    if (before && before > 950 && before < 1050) {
        note = "already ~1 GHz (FSBL)";
        return before;
    }
    if (R32(CLK_BYP_1) & (1u << 6)) {
        note = "c906_0 bypassed to xtal: left alone";
        return before;
    }
    uint32_t mpll = cpuclk_mpll_mhz();
    if (!(R32(PLL_G6_STATUS) & (1u << 16)) || mpll < 900 || mpll > 1100) {
        note = "MPLL not locked or not ~1000 MHz: left alone";
        return before;
    }
    /* Program the unselected divider (MPLL / 1, factor from register, out of
     * reset), then move the glitch-free select over to it. */
    R32(DIV_C906_0_0) = (1u << 16) | (3u << 8) | (1u << 2) | (1u << 0);
    for (volatile int i = 0; i < 1000; i++)
        ;
    R32(CLK_SEL_0) |= 1u << 23;
    uint32_t after = cpuclk_measure_mhz();
    if (after && (after < mhz * 9 / 10 || after > mhz * 11 / 10)) {
        R32(CLK_SEL_0) &= ~(1u << 23);                 /* not what we asked for: back to fpll/2 */
        note = "switched, measured off target, reverted";
        return cpuclk_measure_mhz();
    }
    note = after ? "switched to MPLL/1" : "switched (cycle counter unavailable to confirm)";
    return after;
}

const char *cpuclk_note(void) { return note; }

static void hex(uint32_t v)
{
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        uart_putc("0123456789abcdef"[(v >> i) & 15]);
}

void cpuclk_dump(void)
{
    uart_puts("cpuclk: clk_sel_0 "); hex(R32(CLK_SEL_0));
    uart_puts(" byp_1 "); hex(R32(CLK_BYP_1));
    uart_puts(" div_0_0 "); hex(R32(DIV_C906_0_0));
    uart_puts(" div_0_1 "); hex(R32(DIV_C906_0_1));
    uart_puts("\ncpuclk: pll_g6 ctrl "); hex(R32(PLL_G6_CTRL));
    uart_puts(" status "); hex(R32(PLL_G6_STATUS));
    uart_puts(" mpll_csr "); hex(R32(MPLL_CSR));
    uart_puts(" ssc_ctrl "); hex(R32(MPLL_SSC_CTRL));
    uart_puts(" ssc_set "); hex(R32(MPLL_SSC_SET));
    uart_puts(" -> MPLL ~"); uart_put_dec(cpuclk_mpll_mhz());
    uart_puts(" MHz\n");
}
#else
uint32_t cpuclk_measure_mhz(void) { return 0; }
uint32_t cpuclk_mpll_mhz(void) { return 0; }
uint32_t cpuclk_set_mhz(uint32_t mhz) { (void)mhz; return 0; }
const char *cpuclk_note(void) { return "n/a"; }
void cpuclk_dump(void) {}
int cpuclk_probe_trap(struct trap_frame *f) { (void)f; return 0; }
#endif
