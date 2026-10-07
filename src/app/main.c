#include <stdint.h>
#include <stddef.h>
#include "board.h"
#include "uart.h"
#include "trap.h"
#include "plic.h"
#include "timer.h"
#include "reset.h"
#include "audio_link.h"
#include "panel_link.h"
#include "panel_ui.h"
#include "engine.h"
#include "osc.h"
#include "vec.h"
#include "ui.h"
#include "preset_fs.h"
#include "sd.h"
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

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

#ifdef BOARD_HAS_SPI_LINK
/* Engine -> rvlink block (main-loop context: float is safe here). Timed for
 * the console and the header's CPU figure. */
#define RENDER_MAX 64
static uint64_t render_ticks_sum, render_blocks, render_last_avg;
static uint32_t render_max_ticks;

static void render_block(int32_t *lr, unsigned frames)
{
    float l[RENDER_MAX], r[RENDER_MAX];
    uint64_t t0 = rdtime();
    engine_render(l, r, (int)frames);
    for (unsigned i = 0; i < frames; i++) {
        float a = l[i] < -1.0f ? -1.0f : l[i] > 1.0f ? 1.0f : l[i];
        float b = r[i] < -1.0f ? -1.0f : r[i] > 1.0f ? 1.0f : r[i];
        lr[2 * i] = (int32_t)(a * 8388607.0f) * 256;       /* 24-bit, left-aligned */
        lr[2 * i + 1] = (int32_t)(b * 8388607.0f) * 256;
    }
    uint32_t dt = (uint32_t)(rdtime() - t0);
    render_ticks_sum += dt;
    render_blocks++;
    if (dt > render_max_ticks)
        render_max_ticks = dt;
}

static uint64_t render_avg_us(void)
{
    uint64_t avg = render_blocks ? render_ticks_sum / render_blocks : 0;
    render_ticks_sum = 0;
    render_blocks = 0;
    render_last_avg = avg;
    return avg * 1000000ull / BOARD_TIMEBASE_HZ;
}

static uint64_t now_ticks(void) { return rdtime(); }

/* Scalar vs RVV oscillator kernel on the same random bank: prints the largest
 * difference. Returns 1 if they agree (< 1e-4). Main-loop context. */
static int simd_selftest(void)
{
    if (!vec_ok()) return 0;
    osc_bank_t a, b;
    uint32_t r = 0x9e3779b9u;
    a.n = OSC_MAX;
    for (int k = 0; k < OSC_MAX; k++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        a.ph[k] = r;
        float dt = 0.0005f + 0.06f * (float)(k + 1) / OSC_MAX;     /* ~24 Hz .. 3 kHz */
        a.inc[k] = (uint32_t)(dt * 4294967296.0f);
        a.dt[k] = dt;
        a.idt[k] = 1.0f / dt;
        a.gl[k] = 0.3f + 0.1f * (float)k;
        a.gr[k] = 1.0f - a.gl[k];
    }
    b = a;
    float l1[64] = { 0 }, r1[64] = { 0 }, l2[64] = { 0 }, r2[64] = { 0 };
    float maxd = 0.0f;
    for (int blk = 0; blk < 8; blk++) {                 /* several blocks: phase wraps, BLEP edges */
        for (int i = 0; i < 64; i++) l1[i] = r1[i] = l2[i] = r2[i] = 0.0f;
        osc_bank_scalar(&a, 0.6f, 64, l1, r1);
        osc_bank_rvv(&b, 0.6f, 64, l2, r2);
        for (int i = 0; i < 64; i++) {
            float d = l1[i] - l2[i], e = r1[i] - r2[i];
            if (d < 0) d = -d;
            if (e < 0) e = -e;
            if (d > maxd) maxd = d;
            if (e > maxd) maxd = e;
        }
        for (int k = 0; k < OSC_MAX; k++)
            if (a.ph[k] != b.ph[k]) maxd = 1e9f;            /* phase bookkeeping must match */
    }
    uart_puts("simd: self-test max |scalar - rvv| = ");
    uart_put_dec((uint64_t)(maxd * 1e9f));
    uart_puts("e-9 -> ");
    int pass = maxd < 1e-4f;
    uart_puts(pass ? "PASS\n" : "FAIL\n");
    return pass;
}

static void print_profile(void)
{
    engine_profile_t p;
    engine_profile_take(&p);
    if (!p.frames) return;
    uint64_t blocks = p.frames / 64 ? p.frames / 64 : 1;
#define US(x) ((x) * 1000000ull / BOARD_TIMEBASE_HZ / blocks)
    uart_puts("prof (us/64-frame block, ");
    uart_puts(engine_simd() ? "rvv" : "scalar");
    uart_puts("): osc "); uart_put_dec(US(p.osc));
    uart_puts("  voice "); uart_put_dec(US(p.voice));
    uart_puts("  chorus "); uart_put_dec(US(p.chorus));
    uart_puts("  delay "); uart_put_dec(US(p.delay));
    uart_puts("  reverb "); uart_put_dec(US(p.reverb));
    uart_puts("  total "); uart_put_dec(US(p.total));
    uart_putc('\n');
#undef US
}

static uint64_t render_load_pct(void)
{
    /* 64 frames at 48 kHz = 1333 us = BOARD_TIMEBASE_HZ / 750 ticks */
    return render_last_avg * 100ull * 750ull / BOARD_TIMEBASE_HZ;
}
#endif

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
    vec_init();
    uart_puts("simd: vector unit ");
    uart_puts(vec_ok() ? "ok, VLEN " : "unavailable (");
    if (vec_ok()) { uart_put_dec((uint64_t)vec_vlen_bits()); uart_puts(", "); }
    uart_puts(vec_how());
    uart_puts(vec_ok() ? "\n" : ")\n");
    engine_init(48000.0f);             /* latches a D2+A2 drone */
    engine_set_timer(now_ticks);
    engine_set_simd(simd_selftest());  /* RVV kernels only if they match scalar */
    uart_puts(engine_simd() ? "simd: engine using RVV kernels ('v' toggles)\n"
                            : "simd: engine using scalar kernels\n");
    ui_init();
    audio_link_set_render(render_block);
    if (audio_link_init() == 0) {
        spi_dump();
        uart_puts("m4: audio link up (SPI2 mode 3, DRQ A27 irq ");
        uart_put_dec(BOARD_GPIO_IRQ);
        uart_puts("); 't' = force one transfer, 'd' = toggle SPI DMA (now ");
        uart_puts(audio_link_dma_active() ? "on)" : "off)");
        uart_puts(", 'p' = test pattern <-> engine\n");
    } else {
        uart_puts("m4: audio link init FAILED (CTRLR0 readback)\n");
    }
    uint32_t last_frames = 0;
#endif

    if (panel_link_init() == 0) {
        uart_puts("m6: panel link up (UART2 A28 TX / A29 RX, 1562500 8N1, irq ");
        uart_put_dec(BOARD_UART2_IRQ_OR_0);
        uart_puts(")\n");
        panel_ui_init();
    } else {
        uart_puts("m6: panel link unavailable\n");
    }

#ifdef BOARD_HAS_SD
    sd_set_idle_hook(audio_link_poll);         /* keep audio fed during SD waits */
    ui_set_store(&preset_fs_store);
    preset_fs_info();
    ui_boot_preset();
    uart_puts("m8: 'i' = SD info, 'F' twice = format card, 'S'/'L' = save/load slot 1\n");
    uint64_t format_armed = 0;
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
        panel_ui_service(t * 1000 / BOARD_TICK_HZ);
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
            uart_puts(")  clock ev "); uart_put_dec(p->clock_events);
            uart_puts(" bpm "); uart_put_dec((uint64_t)(engine_bpm() + 0.5f));
            uart_puts(engine_clock_running() ? " run\n" : " stop\n");
            uart_puts("engine: render avg "); uart_put_dec(render_avg_us());
            uart_puts(" us  max "); uart_put_dec(render_max_ticks * 1000000ull / BOARD_TIMEBASE_HZ);
            uart_puts(" us per 64-frame block (budget 1333), load ");
            uart_put_dec(render_load_pct()); uart_puts("%, voices ");
            uart_put_dec((uint64_t)engine_voices_active()); uart_putc('\n');
            ui_set_load((int)render_load_pct());
            print_profile();
            render_max_ticks = 0;
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
#ifdef BOARD_HAS_SPI_LINK
            if (c == 'v') {                        /* RVV <-> scalar kernels (A/B timing) */
                engine_set_simd(!engine_simd() && vec_ok());
                uart_puts(engine_simd() ? "simd: RVV kernels\n" : "simd: scalar kernels\n");
                continue;
            }
            if (c == 'x') { simd_selftest(); continue; }
            if (c == 'w') {                        /* M7: worst-case load, 4 voices x 7 osc */
                uint8_t cc[3] = { 0xB0, 20 + P_OSCS, 127 };
                ui_midi(cc, 3, 0);
                static const uint8_t chord[4] = { 38, 45, 50, 57 };
                for (int i = 0; i < 4; i++) {
                    uint8_t on[3] = { 0x90, chord[i], 100 };
                    ui_midi(on, 3, 0);
                }
                for (int i = 0; i < 4; i++) {
                    uint8_t off[3] = { 0x80, chord[i], 0 };
                    ui_midi(off, 3, 0);
                }
                uart_puts("engine: worst case (OSCS 7, 4 voices latched)\n");
                continue;
            }
#endif
#ifdef BOARD_HAS_SD
            if (c == 'i') { preset_fs_info(); continue; }
            if (c == 'S' || c == 'L') {
                int r = c == 'S' ? ui_preset_save(1) : ui_preset_load(1);
                uart_puts(c == 'S' ? "preset: save slot 1 -> " : "preset: load slot 1 -> ");
                if (r < 0) { uart_putc('-'); r = -r; }
                uart_put_dec((uint64_t)r);
                uart_putc('\n');
                continue;
            }
            if (c == 'F') {
                if (format_armed && timer_ticks - format_armed < 3 * BOARD_TICK_HZ) {
                    uart_puts("sd: formatting (erases the card)...\n");
                    int r = preset_fs_format();
                    uart_puts(r == 0 ? "sd: format ok\n" : "sd: format FAILED, code ");
                    if (r) { if (r < 0) { uart_putc('-'); r = -r; } uart_put_dec((uint64_t)r); uart_putc('\n'); }
                    preset_fs_info();
                    format_armed = 0;
                } else {
                    format_armed = timer_ticks ? timer_ticks : 1;
                    uart_puts("sd: press 'F' again within 3 s to ERASE the card\n");
                }
                continue;
            }
#endif
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
