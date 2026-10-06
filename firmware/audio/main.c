// Pico A (audio): plays the Nano's rvlink audio out of PIO I2S, 48 kHz, 64 fs.
//
// sysclk 153.6 MHz -> PIO clkdiv 25 -> 6.144 MHz = 48 kHz * 64 bits * 2 cycles.
// Two DMA channels, chained A -> B -> A, each moving one block of 64 stereo
// frames into the PIO TX FIFO. The completion IRQ of a channel re-arms its read
// pointer and refills its buffer from the rvlink ring (audio_ring.h) while the
// other channel plays. Silence while the ring primes or after an underrun.

#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"

#include "audio_i2s.pio.h"
#include "rv_audio.h"
#include "link_spi.h"
#include "audio_ring.h"

#define PIN_BCK   10    // LRCK is GP11 (side-set), see audio_i2s.pio
#define PIN_DIN   12

// Clock plan: 12 MHz XOSC / 1 * 128 = 1536 MHz VCO, / (5 * 2) = 153.6 MHz.
#define VCO_HZ        1536000000u
#define POSTDIV1      5u
#define POSTDIV2      2u
#define SYS_HZ        (VCO_HZ / (POSTDIV1 * POSTDIV2))
#define PIO_HZ        (RV_SAMPLE_RATE_HZ * 64u * 2u)    // 2 cycles per bit

static_assert(XOSC_HZ == 12000000, "clock plan assumes a 12 MHz crystal");
static_assert(SYS_HZ == 153600000u, "sysclk must be 153.6 MHz");
static_assert(SYS_HZ % PIO_HZ == 0, "PIO divider must be integral");
#define PIO_CLKDIV    (SYS_HZ / PIO_HZ)
static_assert(PIO_CLKDIV == 25, "expected PIO clkdiv 25");

static int32_t blocks[2][RV_BLOCK_WORDS] __attribute__((aligned(4)));
static int dma_ch[2];

static volatile uint32_t block_count;   // DMA block completions
static volatile uint32_t late_count;    // refills that missed their deadline

static audio_ring_t ring;

static void __isr dma_irq_handler(void) {
    for (int i = 0; i < 2; i++) {
        uint32_t bit = 1u << dma_ch[i];
        if (!(dma_hw->ints0 & bit)) continue;
        dma_hw->ints0 = bit;                    // ack
        // The chain has already started the other channel. If this channel
        // is busy again, it was re-triggered with a stale read pointer: late.
        if (dma_channel_is_busy(dma_ch[i])) late_count++;
        dma_channel_set_read_addr(dma_ch[i], blocks[i], false);
        audio_ring_pop(&ring, blocks[i]);
        block_count++;
        link_spi_on_block();
    }
}

int main(void) {
    // 153.6 MHz is above the 150 MHz default, so give the core a bit more margin.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    // The SDK's solver must find an exact solution for 153600 kHz (it may pick
    // other dividers; we apply ours below). The achieved clock is checked later.
    uint sdk_vco, sdk_pd1, sdk_pd2;
    bool pll_ok = check_sys_clock_khz(SYS_HZ / 1000, &sdk_vco, &sdk_pd1, &sdk_pd2);
    set_sys_clock_pll(VCO_HZ, POSTDIV1, POSTDIV2);
    stdio_init_all();

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t peri_hz = clock_get_hz(clk_peri);

    // Wait for a terminal so the startup lines are not lost (3 s max).
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);

    printf("\nrv_drone audio (Pico A)\n");
    printf("sysclk %lu Hz (peri %lu Hz) PLL %s: VCO %u / %u / %u\n",
           (unsigned long)sys_hz, (unsigned long)peri_hz,
           pll_ok ? "ok" : "FAILED", VCO_HZ, POSTDIV1, POSTDIV2);
    printf("PIO clkdiv %u.0 -> %u Hz (= %u Hz * 64 * 2)\n",
           (unsigned)PIO_CLKDIV, (unsigned)(sys_hz / PIO_CLKDIV), RV_SAMPLE_RATE_HZ);
    if (!pll_ok || sys_hz != SYS_HZ) {
        printf("FATAL: sysclk is not %u Hz\n", SYS_HZ);
        for (;;) tight_loop_contents();
    }

    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &audio_i2s_program);
    audio_i2s_program_init(pio, sm, offset, PIN_DIN, PIN_BCK, PIO_CLKDIV);

    static_assert(RV_BLOCK_WORDS == AUDIO_RING_WORDS, "block size");
    // Both blocks start silent (static); the ring primes from the Nano.

    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
        channel_config_set_chain_to(&c, dma_ch[1 - i]);
        dma_channel_configure(dma_ch[i], &c, &pio->txf[sm], blocks[i],
                              RV_BLOCK_WORDS, false);
        dma_channel_set_irq0_enabled(dma_ch[i], true);
    }
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    link_spi_init(&ring);                   // armed before the first DRQ

    dma_channel_start(dma_ch[0]);           // block 0, then chain to 1, 0, ...
    pio_sm_set_enabled(pio, sm, true);
    printf("I2S running from the rvlink ring (%u blocks, prime %u): BCK GP%d LRCK GP%d DIN GP%d\n",
           AUDIO_RING_BLOCKS, AUDIO_RING_TARGET, PIN_BCK, PIN_BCK + 1, PIN_DIN);

    uint32_t last_blocks = 0, last_ok = 0;
    for (;;) {
        sleep_ms(1000);
        uint32_t b = block_count;
        printf("blocks/s %lu (expect %u)  late %lu  ring fill %lu %s underruns %lu overflows %lu catchups %lu\n",
               (unsigned long)(b - last_blocks), RV_BLOCKS_PER_SEC,
               (unsigned long)late_count, (unsigned long)audio_ring_fill(&ring),
               ring.playing ? "playing" : "priming", (unsigned long)ring.underruns,
               (unsigned long)ring.overflows, (unsigned long)link_spi_catchups());
        last_blocks = b;
        link_stats_t ls;
        link_spi_get_stats(&ls);
        printf("link: ok %lu/s crc %lu magic %lu pattern %lu short %lu gaps %lu\n",
               (unsigned long)(ls.frames_ok - last_ok), (unsigned long)ls.crc_err,
               (unsigned long)ls.magic_err, (unsigned long)ls.pattern_err,
               (unsigned long)ls.short_err, (unsigned long)ls.seq_gaps);
        last_ok = ls.frames_ok;
    }
}
