/* SD0 driver: standard SDHCI register interface (DWC MSHC), polled PIO.
 * Platform setup follows the vendor Linux driver (sdhci-cv181x.c): clock gates,
 * pads (func 0) + pulls, PWRSW 3.3 V, then after every reset the MSHC/PHY
 * defaults for default speed. The base clock is 375 MHz (DTS src-frequency);
 * if it is lower, every SD clock here only gets slower (safe). */
#include "sd.h"
#include "board.h"

#ifndef BOARD_HAS_SD
static sd_info_t info;
int sd_init(void) { return -1; }
int sd_read(uint32_t l, uint8_t *b, uint32_t c) { (void)l; (void)b; (void)c; return -1; }
int sd_write(uint32_t l, const uint8_t *b, uint32_t c) { (void)l; (void)b; (void)c; return -1; }
const sd_info_t *sd_info(void) { return &info; }
void sd_set_idle_hook(void (*fn)(void)) { (void)fn; }
void sd_dump(void) {}
#else
#include "timer.h"
#include "uart.h"

#define R32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
#define R16(a)  (*(volatile uint16_t *)(uintptr_t)(a))
#define R8(a)   (*(volatile uint8_t *)(uintptr_t)(a))
#define SD(o)   (BOARD_SD_BASE + (o))

/* SDHCI */
#define BLKSIZE   0x04
#define BLKCNT    0x06
#define ARG       0x08
#define XFERMODE  0x0C          /* 32-bit write with CMD at 0x0E */
#define RESP0     0x10
#define BUF       0x20
#define PRESENT   0x24
#define HOSTCTL   0x28
#define PWRCTL    0x29
#define CLKCTL    0x2C
#define TIMEOUT   0x2E
#define SWRST     0x2F
#define NISTS     0x30          /* normal int status (16) + error status (16) */
#define NISTSEN   0x34
#define NISIGEN   0x38
#define HOSTCTL2  0x3E
#define VERSION   0xFE
/* vendor */
#define MSHC_CTRL     0x200
#define PHY_TX_RX_DLY 0x240
#define PHY_CONFIG    0x24C

#define PS_CMD_INHIBIT 0x1
#define PS_DAT_INHIBIT 0x2
#define PS_BWEN        (1u << 10)
#define PS_BREN        (1u << 11)
#define PS_CARD_IN     (1u << 16)
#define IS_CMD_DONE    0x1
#define IS_XFER_DONE   0x2
#define IS_BWR         0x10
#define IS_BRR         0x20
#define IS_ERR         0x8000

/* command flags (CMD register bits 7:0) */
#define RSP_NONE  0x00
#define RSP_136   0x09          /* 136-bit, CRC */
#define RSP_48    0x1A          /* 48-bit, CRC + index */
#define RSP_48B   0x1B          /* 48-bit busy, CRC + index */
#define RSP_R3    0x02          /* 48-bit, no CRC/index (OCR) */
#define DATA      0x20

/* clocks / pads (FMUX base + offsets), PWRSW */
#define CLK_EN_0      (BOARD_CLKGEN_BASE + 0x000)    /* 18 axi4_sd0, 19 sd0, 20 100k_sd0 */
#define CLK_DIV_SD0   (BOARD_CLKGEN_BASE + 0x070)
#define CLK_BYP_0     (BOARD_CLKGEN_BASE + 0x030)
#define SD_PWRSW_CTRL (BOARD_SYSCTL_BASE + 0x1F4)
#define FMUX(o)       (BOARD_FMUX_BASE + (o))

static sd_info_t info;
static void (*idle)(void);

void sd_set_idle_hook(void (*fn)(void)) { idle = fn; }
const sd_info_t *sd_info(void) { return &info; }

static void delay_us(uint32_t us)
{
    uint64_t t = rdtime() + (uint64_t)us * (BOARD_TIMEBASE_HZ / 1000000);
    while (rdtime() < t)
        ;
}

/* Waits until (reg & mask) == want, calling the idle hook. 0 or -1 on timeout. */
static int wait16(uint32_t off, uint16_t mask, uint16_t want, uint32_t ms)
{
    uint64_t end = rdtime() + (uint64_t)ms * (BOARD_TIMEBASE_HZ / 1000);
    while ((R16(SD(off)) & mask) != want) {
        if (rdtime() > end)
            return -1;
        if (idle)
            idle();
    }
    return 0;
}

static void put_hex32(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        uart_putc("0123456789abcdef"[(v >> i) & 15]);
}

void sd_dump(void)
{
    uart_puts("sd: ver "); put_hex32(R16(SD(VERSION)));
    uart_puts(" present "); put_hex32(R32(SD(PRESENT)));
    uart_puts(" clk "); put_hex32(R16(SD(CLKCTL)));
    uart_puts(" ists "); put_hex32(R32(SD(NISTS)));
    uart_puts(" cap "); put_hex32(R32(SD(0x40)));
    uart_puts("\nsd: CLK_EN_0 "); put_hex32(R32(CLK_EN_0));
    uart_puts(" DIV_SD0 "); put_hex32(R32(CLK_DIV_SD0));
    uart_puts(" BYP_0 "); put_hex32(R32(CLK_BYP_0));
    uart_puts(" PWRSW "); put_hex32(R32(SD_PWRSW_CTRL));
    uart_puts(" mshc "); put_hex32(R32(SD(MSHC_CTRL)));
    uart_putc('\n');
}

static void vendor_defaults(void)
{
    R32(SD(MSHC_CTRL)) |= (1u << 1) | (1u << 8) | (1u << 9);
    R32(SD(PHY_TX_RX_DLY)) = 0x01000100;
    R32(SD(PHY_CONFIG)) = 1;
}

static int reset(uint8_t what)
{
    R16(SD(NISIGEN)) = 0;
    R8(SD(SWRST)) = what;
    uint64_t end = rdtime() + BOARD_TIMEBASE_HZ / 10;
    while (R8(SD(SWRST)) & what)
        if (rdtime() > end)
            return -1;
    vendor_defaults();
    return 0;
}

/* SD clock = base / (2 * div), div 1..1023 (0 = base). */
static int set_clock(uint32_t hz)
{
    uint32_t div = (BOARD_SD_BASE_HZ + 2 * hz - 1) / (2 * hz);
    if (div > 1023) div = 1023;
    R16(SD(CLKCTL)) = 0;
    uint16_t v = (uint16_t)(((div & 0xff) << 8) | ((div >> 8) << 6) | 0x1);   /* internal on */
    R16(SD(CLKCTL)) = v;
    if (wait16(CLKCTL, 0x2, 0x2, 20))
        return -1;
    R16(SD(CLKCTL)) = v | 0x4;                       /* SD clock on */
    info.clk_hz = BOARD_SD_BASE_HZ / (2 * div);
    delay_us(100);
    return 0;
}

/* Sends a command; for data commands the caller has set BLKSIZE/BLKCNT. */
static int cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint16_t mode, uint32_t *resp)
{
    uint32_t inhibit = PS_CMD_INHIBIT | ((flags & DATA) || (flags & 3) == 3 ? PS_DAT_INHIBIT : 0);
    uint64_t end = rdtime() + BOARD_TIMEBASE_HZ / 10;
    while (R32(SD(PRESENT)) & inhibit)
        if (rdtime() > end)
            return -10;
    R32(SD(NISTS)) = 0xffffffffu;                    /* clear all */
    R32(SD(ARG)) = arg;
    R32(SD(XFERMODE)) = (uint32_t)mode | ((idx << 8 | flags) << 16);
    if (wait16(NISTS, IS_CMD_DONE | IS_ERR, IS_CMD_DONE, 100)) {
        uint32_t st = R32(SD(NISTS));
        R32(SD(NISTS)) = st;
        reset(0x6);                                  /* CMD + DAT lines */
        return (st & 0x10000) ? -11 : -12;           /* -11: command timeout */
    }
    R16(SD(NISTS)) = IS_CMD_DONE;
    if (resp) {
        resp[0] = R32(SD(RESP0));
        if ((flags & 3) == 1) {                      /* R2: bits 127:8 in RESP0..3 */
            resp[1] = R32(SD(RESP0 + 4));
            resp[2] = R32(SD(RESP0 + 8));
            resp[3] = R32(SD(RESP0 + 12));
        }
    }
    if ((flags & 3) == 3 && !(flags & DATA)) {       /* R1b: wait for busy end */
        if (wait16(NISTS, IS_XFER_DONE | IS_ERR, IS_XFER_DONE, 500))
            return -13;
        R16(SD(NISTS)) = IS_XFER_DONE;
    }
    return 0;
}

static int acmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t *resp)
{
    int r = cmd(55, info.rca << 16, RSP_48, 0, 0);
    if (r) return r;
    return cmd(idx, arg, flags, 0, resp);
}

static void pads_and_power(void)
{
    R32(CLK_EN_0) |= (1u << 18) | (1u << 19) | (1u << 20);
    for (uint32_t o = 0x1C; o <= 0x34; o += 4)       /* CLK CMD D0-D3 CD: func 0 */
        R32(FMUX(o)) = 0;
    /* SD0_PWR_EN (0x38) stays the LED GPIO. Pulls: CD up, CLK down, CMD/D0-3 up. */
    R32(FMUX(0x900)) = (R32(FMUX(0x900)) & ~0xCu) | 0x4;
    R32(FMUX(0xA00)) = (R32(FMUX(0xA00)) & ~0xCu) | 0x8;
    for (uint32_t o = 0xA04; o <= 0xA14; o += 4)
        R32(FMUX(o)) = (R32(FMUX(o)) & ~0xCu) | 0x4;
    R32(SD_PWRSW_CTRL) = (R32(SD_PWRSW_CTRL) & ~0xFu) | 0x9;    /* 3.3 V, enabled */
    delay_us(1000);
}

int sd_init(void)
{
    uint32_t r[4];
    info = (sd_info_t){ 0 };
    pads_and_power();
    if (reset(0x1))
        return -1;
    R8(SD(PWRCTL)) = 0x0F;                           /* 3.3 V, bus power on */
    R8(SD(TIMEOUT)) = 0x0E;
    R16(SD(HOSTCTL2)) = 0;
    R8(SD(HOSTCTL)) = 0;                             /* 1-bit, default speed, no DMA */
    R32(SD(NISTSEN)) = 0xffffffffu;                  /* latch everything; no signals */
    R32(SD(NISIGEN)) = 0;
    if (set_clock(400000))
        return -2;
    delay_us(2000);                                  /* >= 74 clocks */

    cmd(0, 0, RSP_NONE, 0, 0);
    int v2 = cmd(8, 0x1AA, RSP_48, 0, r) == 0 && (r[0] & 0xFFF) == 0x1AA;
    uint64_t end = rdtime() + BOARD_TIMEBASE_HZ;     /* ACMD41: up to 1 s */
    do {
        if (acmd(41, (v2 ? 0x40000000u : 0) | 0x00FF8000u, RSP_R3, r))
            return -3;
        if (rdtime() > end)
            return -4;
    } while (!(r[0] & 0x80000000u));
    info.sdhc = (r[0] & 0x40000000u) != 0;
    if (cmd(2, 0, RSP_136, 0, info.cid))
        return -5;
    if (cmd(3, 0, RSP_48, 0, r))
        return -6;
    info.rca = r[0] >> 16;
    if (cmd(9, info.rca << 16, RSP_136, 0, info.csd))
        return -7;
    /* CSD (SDHCI drops the CRC byte: csd bit n = CSD bit n+8) */
    uint32_t structure = info.csd[3] >> 22 & 3;
    if (structure == 1) {                            /* v2: C_SIZE [69:48] -> [61:40] */
        uint32_t c_size = (info.csd[1] >> 8) & 0x3FFFFF;
        info.sectors = ((uint64_t)c_size + 1) * 1024;
    } else {                                         /* v1 */
        uint32_t read_bl_len = info.csd[2] >> 8 & 0xF;
        uint32_t c_size = (info.csd[2] & 0x3) << 10 | info.csd[1] >> 22;
        uint32_t mult = info.csd[1] >> 7 & 0x7;
        info.sectors = ((uint64_t)(c_size + 1) << (mult + 2)) << read_bl_len >> 9;
    }
    if (cmd(7, info.rca << 16, RSP_48B, 0, 0))
        return -8;
    if (!info.sdhc && cmd(16, 512, RSP_48, 0, 0))
        return -9;
    if (acmd(6, 2, RSP_48, 0) == 0) {                /* 4-bit bus */
        R8(SD(HOSTCTL)) |= 0x2;
        info.bus4 = 1;
    }
    if (set_clock(25000000))
        return -2;
    return 0;
}

static int xfer(int write, uint32_t lba, uint8_t *buf, uint32_t count)
{
    if (!count) return 0;
    uint32_t addr = info.sdhc ? lba : lba * 512;
    R16(SD(BLKSIZE)) = 512;
    R16(SD(BLKCNT)) = (uint16_t)count;
    uint16_t mode = (uint16_t)((write ? 0 : 0x10) | (count > 1 ? 0x22 | 0x04 : 0));  /* multi+cnt+auto12 */
    uint32_t idx = write ? (count > 1 ? 25 : 24) : (count > 1 ? 18 : 17);
    int r = cmd(idx, addr, RSP_48 | DATA, mode, 0);
    if (r) return r;
    uint16_t flag = write ? IS_BWR : IS_BRR;
    for (uint32_t b = 0; b < count; b++) {
        if (wait16(NISTS, flag | IS_ERR, flag, 500))
            goto fail;
        R16(SD(NISTS)) = flag;
        uint32_t *w = (uint32_t *)(void *)(buf + 512 * b);
        if (write)
            for (int i = 0; i < 128; i++) R32(SD(BUF)) = w[i];
        else
            for (int i = 0; i < 128; i++) w[i] = R32(SD(BUF));
    }
    if (wait16(NISTS, IS_XFER_DONE | IS_ERR, IS_XFER_DONE, write ? 2000 : 500))
        goto fail;
    R16(SD(NISTS)) = IS_XFER_DONE;
    return 0;
fail: {
        uint32_t st = R32(SD(NISTS));
        R32(SD(NISTS)) = st;
        reset(0x6);
        if (count > 1)
            cmd(12, 0, RSP_48B, 0, 0);               /* stop a stuck multi-block */
        return -20;
    }
}

int sd_read(uint32_t lba, uint8_t *buf, uint32_t count)
{
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        int r = xfer(0, lba, buf, n);
        if (r) return r;
        lba += n; buf += 512 * n; count -= n;
    }
    return 0;
}

int sd_write(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        int r = xfer(1, lba, (uint8_t *)buf, n);
        if (r) return r;
        lba += n; buf += 512 * n; count -= n;
    }
    return 0;
}
#endif
