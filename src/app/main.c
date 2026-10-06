#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "uart.h"
#include "trap.h"
#include "plic.h"
#include "timer.h"
#include "reset.h"
#include "audio_link.h"
#ifdef BOARD_HAS_SPI_LINK
#include "spi.h"
#endif

/* Freestanding helpers; GCC may emit calls to these even with -ffreestanding. */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static void led_init(void)
{
#ifdef BOARD_LED_GPIO_BASE
    REG32(BOARD_LED_FMUX) = BOARD_LED_FMUX_GPIO;
    REG32(BOARD_LED_GPIO_BASE + 0x04) |= 1u << BOARD_LED_BIT;
#endif
}

static void led_toggle(void)
{
#ifdef BOARD_LED_GPIO_BASE
    REG32(BOARD_LED_GPIO_BASE + 0x00) ^= 1u << BOARD_LED_BIT;
#endif
}

void main(uint64_t hartid, uint64_t fdt)
{
    led_init();
    uart_puts("rv_drone hello (hart ");
    uart_put_dec(hartid);
    uart_puts(", fdt ");
    uart_put_hex(fdt);
    uart_puts(")\n");

    trap_init();
    uart_puts("m1: trap ok\n");
    plic_init();
    uart_puts("m1: plic ok\n");
    uart_enable_rx_irq();
    uart_puts("m1: uart irq ok");
    uart_dump_regs();
    uart_putc('\n');
    timer_init();
    uart_puts("m1: timer armed, sip=");
    { uint64_t v; __asm__ volatile("csrr %0, sip" : "=r"(v)); uart_put_hex(v); }
    uart_putc('\n');
    __asm__ volatile("csrs sie, %0" :: "r"((1u << 5) | (1u << 9)));  /* STIE|SEIE */
    __asm__ volatile("csrsi sstatus, 2");                            /* SIE */
    uart_puts("m1: irqs on\n");

#ifdef BOARD_HAS_SPI_LINK
    if (audio_link_init() == 0) {
        spi_dump();
        uart_puts("m4: audio link up (SPI2 mode 3, DRQ A27 irq ");
        uart_put_dec(BOARD_GPIO_IRQ);
        uart_puts("); 't' = force one transfer\n");
    } else {
        uart_puts("m4: audio link init FAILED (CTRLR0 readback)\n");
    }
    uint32_t last_frames = 0;
#endif

#ifdef M1_FAULT_TEST
    (void)*(volatile uint32_t *)0;   /* load access fault -> trap dump */
#endif

    uint64_t last_sec = 0;
    for (;;) {
        /* SIE off across check+wfi so a DRQ IRQ can't slip between them */
        __asm__ volatile("csrci sstatus, 2");
        if (!audio_link_pending())
            __asm__ volatile("wfi");
        __asm__ volatile("csrsi sstatus, 2");
        audio_link_poll();
        uint64_t t = timer_ticks;
        if (t / BOARD_TICK_HZ != last_sec) {
            last_sec = t / BOARD_TICK_HZ;
            led_toggle();
            uart_puts("tick ");
            uart_put_dec(t);
            uart_puts(" time ");
            uart_put_dec(rdtime());
            uart_puts(" claims ");
            uart_put_dec(plic_claims);
            uart_puts(" spur ");
            uart_put_dec(plic_spurious);
            uart_puts(" busy ");
            uart_put_dec(uart_busy_count);
            uart_putc('\n');
#ifdef BOARD_HAS_SPI_LINK
            const volatile audio_link_stats_t *l = audio_link_stats();
            uint32_t fr = l->frames;
            uart_puts("link: frames "); uart_put_dec(fr - last_frames);
            uart_puts("/s  rx_crc "); uart_put_dec(l->rx_crc_err);
            uart_puts("  rx_magic "); uart_put_dec(l->rx_magic_err);
            uart_puts("  missed "); uart_put_dec(l->missed); uart_puts("  spur "); uart_put_dec(l->spurious);
            uart_puts("  slave(seq "); uart_put_dec(l->slave_seq_echo);
            uart_puts(", crc_err "); uart_put_dec(l->slave_crc_err);
            uart_puts(", underruns "); uart_put_dec(l->slave_underruns);
            uart_puts(")  total "); uart_put_dec(fr);
            uart_puts(" drq "); uart_put_dec(l->drq_edges);
            uart_puts(" spi_err "); uart_put_dec(l->spi_err);
            uart_putc('\n');
            last_frames = fr;
            if (l->xfer_ticks) {
                /* 528 B = 4224 bits; ticks are 25 MHz rdtime. Gaps/overhead show as
                 * eff < ideal (ideal = SCK). */
                uart_puts("spi: xfer "); uart_put_dec(l->xfer_ticks);
                uart_puts(" ticks (min "); uart_put_dec(l->xfer_ticks_min);
                uart_puts(") = "); uart_put_dec(4224ull * (BOARD_TIMEBASE_HZ / 1000) / l->xfer_ticks);
                uart_puts(" kbit/s eff, SCK "); uart_put_dec(spi_sck_hz() / 1000);
                uart_puts(" kHz\n");
            }
#endif
        }
        int c;
        while ((c = uart_getc_nonblock()) >= 0) {
            if (c == 0x12) {                       /* Ctrl-R: reset (usbboot) */
                uart_puts("reset\n");
                board_reset();
            }
            if (c == 't') {                        /* M4: scope trigger, no DRQ needed */
                audio_link_kick();
                continue;
            }
            uart_puts("rx: '");
            uart_putc(c >= 32 && c < 127 ? (char)c : '.');
            uart_puts("' (0x");
            uart_putc("0123456789abcdef"[c >> 4]);
            uart_putc("0123456789abcdef"[c & 15]);
            uart_puts(")\n");
        }
    }
}
