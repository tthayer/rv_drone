#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "uart.h"
#include "trap.h"
#include "plic.h"
#include "timer.h"
#include "reset.h"
#include "audio_link.h"
#include "tone.h"
#include "panel_link.h"
#include "ui.h"
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

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
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
    uart_async_tx(1);                  /* from here prints never block */

#ifdef BOARD_HAS_SPI_LINK
    __asm__ volatile("csrs sstatus, %0" :: "r"(1u << 13));   /* FS=Initial: float in main loop */
    tone_init(330.0f, 0.5f);           /* E4, -6 dBFS: not Pico A's old 440 Hz */
    audio_link_set_render(tone_render);
    if (audio_link_init() == 0) {
        spi_dump();
        uart_puts("m4: audio link up (SPI2 mode 3, DRQ A27 irq ");
        uart_put_dec(BOARD_GPIO_IRQ);
        uart_puts("); 't' = force one transfer, 'd' = toggle SPI DMA (now ");
        uart_puts(audio_link_dma_active() ? "on)" : "off)");
        uart_puts(", 'p' = test pattern <-> 330 Hz sine\n");
    } else {
        uart_puts("m4: audio link init FAILED (CTRLR0 readback)\n");
    }
    uint32_t last_frames = 0;
#endif

    if (panel_link_init() == 0) {
        uart_puts("m6: panel link up (UART2 A28 TX / A29 RX, 1562500 8N1, irq ");
        uart_put_dec(BOARD_UART2_IRQ_OR_0);
        uart_puts(")\n");
        ui_init();
    } else {
        uart_puts("m6: panel link unavailable\n");
    }

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
        ui_service(t * 1000 / BOARD_TICK_HZ);
        panel_link_poll();
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
            uart_puts(", ring "); uart_put_dec(l->slave_ring_fill);
            uart_puts(")  "); uart_puts(audio_link_test_mode() ? "pattern" : "audio");
            uart_puts("  total "); uart_put_dec(fr);
            uart_puts(" drq "); uart_put_dec(l->drq_edges);
            uart_puts(" spi_err "); uart_put_dec(l->spi_err);
            uart_puts(" txdrop "); uart_put_dec(uart_tx_dropped);
            uart_putc('\n');
            last_frames = fr;
            const volatile panel_link_stats_t *p = panel_link_stats();
            uart_puts("panel: rx ok "); uart_put_dec(p->rx_ok);
            uart_puts(" crc "); uart_put_dec(p->rx_crc_err);
            uart_puts(" cobs "); uart_put_dec(p->rx_cobs_err);
            uart_puts(" ovr "); uart_put_dec(p->rx_overrun);
            uart_puts("  tx pkts "); uart_put_dec(p->tx_packets);
            uart_puts(" drop "); uart_put_dec(p->tx_dropped);
            uart_puts("  peer(status "); uart_put_dec(p->peer_status_count);
            uart_puts(", rx ok "); uart_put_dec(p->peer_rx_ok);
            uart_puts(", crc "); uart_put_dec(p->peer_crc_err);
            uart_puts(", cobs "); uart_put_dec(p->peer_cobs_err);
            uart_puts(", drop "); uart_put_dec(p->peer_dropped);
            uart_puts(")\n");
            uart_puts("dma: "); uart_puts(audio_link_dma_active() ? "on" : "off");
            uart_puts(" frames "); uart_put_dec(l->dma_frames);
            uart_puts(" err "); uart_put_dec(l->dma_err);
            uart_putc('\n');
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
                uart_sync();
                board_reset();
            }
            if (c == 'd') {                        /* M5: DMA <-> polled SPI frames */
                audio_link_set_dma(!audio_link_dma_active());
                uart_puts(audio_link_dma_active() ? "link: SPI DMA on\n" : "link: SPI polled\n");
                continue;
            }
            if (c == 'p') {                        /* M5: test pattern <-> rendered audio */
                audio_link_set_test(!audio_link_test_mode());
                uart_puts(audio_link_test_mode() ? "link: test pattern\n" : "link: audio\n");
                continue;
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
