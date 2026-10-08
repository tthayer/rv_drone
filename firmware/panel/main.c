// Pico B (panel): 6 encoders (PIO quadrature), 6 switches (1 kHz debounce),
// 3 SSD1306 OLEDs, MIDI in (UART1 RX), rvpanel link to the Nano (UART0).
// MIDI clock/start/stop/continue are forwarded from the UART1 IRQ, timestamped.
//
// Encoder, switch and MIDI events go to the Nano (and the USB CDC console).
// The Nano owns the displays: its PAGE packets go straight into the
// framebuffers. Until the first page arrives, or when the Nano has been
// silent for NANO_TIMEOUT_MS, the boot splash (render_splash) animates; the
// first encoder turn or switch press swaps it for the local stand-in UI.
//
// Core 0: inputs, the Nano link, events, local rendering. Switches are sampled
// by a 1 kHz repeating timer; MIDI bytes are collected by the UART1 RX
// interrupt; encoder counts live in the PIO and are absolute.
// Core 1: the display writer. It owns the I2C buses after boot and streams each
// display's dirty column ranges as fast as the bus allows, so core 0 never
// blocks on I2C. The framebuffers are shared under fb_lock (held only for a
// page copy). I2C runs at 1 MHz if every display ACKs at that speed, else 400 kHz.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/multicore.h"

#include "debounce.h"
#include "encoders.h"
#include "fb.h"
#include "midi_parser.h"
#include "nano_link.h"
#include "panel_state.h"
#include "render.h"
#include "ssd1306.h"

#define PIN_I2C0_SDA    4
#define PIN_I2C0_SCL    5
#define PIN_I2C1_SDA    26
#define PIN_I2C1_SCL    27
#define I2C_HZ_SAFE     400000  // probe and init speed
#define I2C_HZ_FAST     1000000 // Fast-mode Plus, used if every display ACKs at it

#define MIDI_UART       uart1
#define MIDI_UART_IRQ   UART1_IRQ
#define PIN_MIDI_RX     21
#define MIDI_BAUD       31250

#define REFRESH_MS      33      // ~30 Hz local render (the Nano paces its own pages)
#define TEST_PATTERN_MS 1500
#define NANO_TIMEOUT_MS 2500    // the Nano resends every page each second

static const uint8_t sw_pin[N_ENC] = { 18, 19, 20, 8, 22, 28 };

typedef struct {
    const char *bus_name;
    i2c_inst_t *i2c;
    uint8_t addr;
} oled_cfg_t;

static const oled_cfg_t oled_cfg[N_OLED] = {
    { "I2C0", i2c0, 0x3C },
    { "I2C0", i2c0, 0x3D },
    { "I2C1", i2c1, 0x3C },
};

// ---- switches: 1 kHz sample, debounce, queue to the main loop --------------

typedef struct { uint8_t id, down; } sw_evt_t;

#define SW_Q_LEN 32     // power of two
static sw_evt_t sw_q[SW_Q_LEN];
static volatile uint32_t sw_head, sw_tail, sw_dropped;
static debounce_bank_t sw_bank;

static bool sw_timer_cb(repeating_timer_t *t) {
    (void)t;
    uint32_t g = gpio_get_all();
    uint8_t raw = 0;
    for (unsigned i = 0; i < N_ENC; i++)
        if (!((g >> sw_pin[i]) & 1u)) raw |= (uint8_t)(1u << i);      // active low

    uint8_t changed = debounce_bank_step(&sw_bank, N_ENC, raw);
    for (unsigned i = 0; changed; i++, changed >>= 1) {
        if (!(changed & 1u)) continue;
        if (sw_head - sw_tail >= SW_Q_LEN) { sw_dropped++; continue; }
        sw_q[sw_head % SW_Q_LEN] = (sw_evt_t){ (uint8_t)i, sw_bank.ch[i].state };
        sw_head++;
    }
    return true;
}

static void switches_init(void) {
    for (unsigned i = 0; i < N_ENC; i++) {
        gpio_init(sw_pin[i]);
        gpio_set_dir(sw_pin[i], GPIO_IN);
        gpio_pull_up(sw_pin[i]);
    }
    static repeating_timer_t timer;
    // Negative delay: fixed 1 ms period measured from each callback start.
    add_repeating_timer_us(-1000, sw_timer_cb, NULL, &timer);
}

// ---- MIDI: UART1 RX IRQ -> ring -> parser ----------------------------------

#define MIDI_RING_LEN 512       // power of two
static uint8_t midi_ring[MIDI_RING_LEN];
static volatile uint32_t midi_head, midi_tail, midi_overruns;

static void midi_rx_irq(void) {
    while (uart_is_readable(MIDI_UART)) {
        uint8_t b = uart_getc(MIDI_UART);
        // MIDI clock goes to the Nano from here, stamped now: the main loop
        // blocks for ms in I2C flushes, which would jitter the beat.
        switch (b) {
        case 0xF8: nano_link_send_clock(RVPANEL_CLK_TICK, time_us_32()); break;
        case 0xFA: nano_link_send_clock(RVPANEL_CLK_START, time_us_32()); break;
        case 0xFB: nano_link_send_clock(RVPANEL_CLK_CONTINUE, time_us_32()); break;
        case 0xFC: nano_link_send_clock(RVPANEL_CLK_STOP, time_us_32()); break;
        }
        if (midi_head - midi_tail >= MIDI_RING_LEN) { midi_overruns++; continue; }
        midi_ring[midi_head % MIDI_RING_LEN] = b;
        midi_head++;
    }
}

static void midi_init(void) {
    uart_init(MIDI_UART, MIDI_BAUD);
    uart_set_format(MIDI_UART, 8, 1, UART_PARITY_NONE);
    gpio_set_function(PIN_MIDI_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_MIDI_RX);      // idle high with nothing plugged in
    uart_set_fifo_enabled(MIDI_UART, true);
    irq_set_exclusive_handler(MIDI_UART_IRQ, midi_rx_irq);
    irq_set_enabled(MIDI_UART_IRQ, true);
    uart_set_irq_enables(MIDI_UART, true, false);
}

// ---- state, displays ---------------------------------------------------------

static panel_state_t st;
static midi_parser_t midi;
static ssd1306_t oled[N_OLED];
static fb_t oled_fb[N_OLED];
static fb_t scratch;
static bool oled_stale[N_OLED];             // needs a re-render
static bool oled_probe_ok[N_OLED];
static bool bus_ack[2][2];                  // [bus][0x3C, 0x3D]
static bool midi_was_lit;
static spin_lock_t *fb_lock;                // oled_fb[] between core 0 and core 1
static uint32_t i2c_hz = I2C_HZ_SAFE;
static volatile int contrast_req = -1;      // core 0 -> core 1 (CONFIG packet)
static bool nano_owned;                     // displays show the Nano's pages
static bool local_ui;                       // an encoder/switch was used: stand-in UI, not the splash
static uint32_t splash_t0;                  // splash start (ms since boot)
static uint32_t nano_pages;

static void mark_all_stale(void) {
    for (unsigned d = 0; d < N_OLED; d++) oled_stale[d] = true;
}

static void i2c_buses_init(void) {
    i2c_init(i2c0, I2C_HZ_SAFE);
    gpio_set_function(PIN_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C0_SDA);
    gpio_pull_up(PIN_I2C0_SCL);
    i2c_init(i2c1, I2C_HZ_SAFE);
    gpio_set_function(PIN_I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C1_SDA);
    gpio_pull_up(PIN_I2C1_SCL);
}

static void probe_buses(void) {
    i2c_inst_t *bus[2] = { i2c0, i2c1 };
    for (int b = 0; b < 2; b++)
        for (int a = 0; a < 2; a++)
            bus_ack[b][a] = ssd1306_probe(bus[b], (uint8_t)(0x3C + a));
}

static void print_banner(void) {
    printf("\n== rv_drone panel (Pico B) ==\n");
    printf("I2C %lu kHz, display writer on core 1\n", (unsigned long)(i2c_hz / 1000));
    printf("sysclk %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));
    for (int b = 0; b < 2; b++) {
        printf("I2C%d:", b);
        bool any = false;
        for (int a = 0; a < 2; a++) {
            if (!bus_ack[b][a]) continue;
            printf(" 0x%02X", 0x3C + a);
            any = true;
        }
        printf("%s\n", any ? "" : " none");
    }
    for (unsigned d = 0; d < N_OLED; d++) {
        printf("OLED%u (%s 0x%02X): %s\n", d, oled_cfg[d].bus_name, oled_cfg[d].addr,
               oled[d].present ? "ok"
               : oled_probe_ok[d] ? "ACKs but init failed" : "not found");
    }
    if (!oled_probe_ok[1])
        printf("note: OLED1 needs its address moved to 0x3D (README)\n");
    printf("encoders: 6 on PIO (pio0 SM0-3, pio1 SM0-1); switches: 1 kHz, %d ms debounce\n",
           DEBOUNCE_SAMPLES);
    printf("midi: UART1 RX GP%d, %d 8N1\n", PIN_MIDI_RX, MIDI_BAUD);
    printf("nano link: UART0 GP16 TX / GP17 RX, %u baud, rvpanel; displays: %s\n",
           NANO_LINK_BAUD, nano_owned ? "Nano" : "local");
}

static void displays_init(void) {
    probe_buses();
    for (unsigned d = 0; d < N_OLED; d++) {
        oled_probe_ok[d] = bus_ack[oled_cfg[d].i2c == i2c1][oled_cfg[d].addr - 0x3C];
        ssd1306_init(&oled[d], oled_cfg[d].i2c, oled_cfg[d].addr);
    }
    // Test pattern, left up for a moment so wiring is visible.
    for (unsigned d = 0; d < N_OLED; d++) {
        if (!oled[d].present) continue;
        render_test_pattern(&scratch, d, oled_cfg[d].addr);
        fb_update(&oled_fb[d], &scratch);
        fb_mark_all_dirty(&oled_fb[d]);
        while (oled_fb[d].dirty && ssd1306_flush(&oled[d], &oled_fb[d], FB_PAGES)) {}
    }
}

// Raise both buses to 1 MHz and confirm every present display still ACKs;
// otherwise fall back to 400 kHz. Boot only (before core 1 starts).
static void i2c_go_fast(void) {
    i2c_set_baudrate(i2c0, I2C_HZ_FAST);
    i2c_set_baudrate(i2c1, I2C_HZ_FAST);
    bool ok = true;
    for (unsigned d = 0; d < N_OLED; d++)
        if (oled[d].present && !ssd1306_nop(&oled[d])) ok = false;
    if (!ok) {
        i2c_set_baudrate(i2c0, I2C_HZ_SAFE);
        i2c_set_baudrate(i2c1, I2C_HZ_SAFE);
    }
    i2c_hz = ok ? I2C_HZ_FAST : I2C_HZ_SAFE;
}

// Core 1: one dirty range per display per pass, round-robin over pages.
static void core1_display_writer(void) {
    for (;;) {
        bool any = false;
        for (unsigned d = 0; d < N_OLED; d++) {
            ssd1306_t *dev = &oled[d];
            if (!dev->present) continue;
            uint8_t row[FB_W];
            unsigned page = FB_PAGES, lo = 0, hi = 0;
            uint32_t irq = spin_lock_blocking(fb_lock);
            fb_t *fb = &oled_fb[d];
            for (unsigned i = 0; i < FB_PAGES; i++) {
                unsigned p = (dev->next_page + i) % FB_PAGES;
                if (!(fb->dirty & (1u << p))) continue;
                page = p;
                lo = fb->lo[p];
                hi = fb->hi[p];
                memcpy(row + lo, fb->buf[p] + lo, hi - lo + 1);
                fb->dirty &= (uint8_t)~(1u << p);
                break;
            }
            spin_unlock(fb_lock, irq);
            if (page == FB_PAGES) continue;
            any = true;
            dev->next_page = (uint8_t)((page + 1) % FB_PAGES);
            if (!ssd1306_write_range(dev, page, lo, hi, row)) {
                dev->errors++;
                irq = spin_lock_blocking(fb_lock);
                fb_mark_dirty(&oled_fb[d], page, lo, hi);     // retry later
                spin_unlock(fb_lock, irq);
            }
        }
        int c = contrast_req;
        if (c >= 0) {
            contrast_req = -1;
            for (unsigned d = 0; d < N_OLED; d++) ssd1306_set_contrast(&oled[d], (uint8_t)c);
        }
        if (!any) sleep_us(100);
    }
}

static void refresh_displays(uint32_t now) {
    bool lit = render_midi_lit(&st, now);
    if (lit != midi_was_lit) {              // box turned on or timed out
        midi_was_lit = lit;
        mark_all_stale();
    }
    for (unsigned d = 0; d < N_OLED; d++) {
        if (!oled[d].present) continue;
        if (nano_owned) continue;
        if (!local_ui) render_splash(&scratch, d, now - splash_t0);   // animates every tick
        else if (oled_stale[d]) render_display(&scratch, d, &st, now);
        else continue;
        uint32_t irq = spin_lock_blocking(fb_lock);
        fb_update(&oled_fb[d], &scratch);       // only changed ranges become dirty
        spin_unlock(fb_lock, irq);
        oled_stale[d] = false;
    }
}

// ---- events --------------------------------------------------------------------

static void set_midi_text(const midi_msg_t *m) {
    switch (m->type) {
    case MIDI_NOTE_ON:  snprintf(st.midi_text, sizeof st.midi_text, "N%u", m->d1); break;
    case MIDI_NOTE_OFF: snprintf(st.midi_text, sizeof st.midi_text, "n%u", m->d1); break;
    case MIDI_CC:       snprintf(st.midi_text, sizeof st.midi_text, "C%u", m->d1); break;
    case MIDI_PITCH_BEND: snprintf(st.midi_text, sizeof st.midi_text, "PB"); break;
    case MIDI_PROGRAM_CHANGE: snprintf(st.midi_text, sizeof st.midi_text, "P%u", m->d1); break;
    }
}

static void print_midi(const midi_msg_t *m) {
    unsigned ch = m->ch + 1u;       // printed 1..16
    switch (m->type) {
    case MIDI_NOTE_ON:
        printf("midi note_on ch %u note %u vel %u\n", ch, m->d1, m->d2); break;
    case MIDI_NOTE_OFF:
        printf("midi note_off ch %u note %u vel %u\n", ch, m->d1, m->d2); break;
    case MIDI_CC:
        printf("midi cc ch %u num %u val %u\n", ch, m->d1, m->d2); break;
    case MIDI_PITCH_BEND:
        printf("midi pitch_bend ch %u val %d\n", ch, m->value); break;
    case MIDI_PROGRAM_CHANGE:
        printf("midi program ch %u prog %u\n", ch, m->d1); break;
    }
}

static void send_midi(const midi_msg_t *m) {
    uint8_t b[3] = { 0, m->d1, m->d2 };
    unsigned len = 3;
    switch (m->type) {
    case MIDI_NOTE_OFF: b[0] = 0x80; break;
    case MIDI_NOTE_ON:  b[0] = 0x90; break;
    case MIDI_CC:       b[0] = 0xB0; break;
    case MIDI_PROGRAM_CHANGE: b[0] = 0xC0; len = 2; break;
    case MIDI_PITCH_BEND: {
        unsigned v = (unsigned)(m->value + 8192);
        b[0] = 0xE0; b[1] = v & 0x7F; b[2] = (v >> 7) & 0x7F;
        break;
    }
    }
    b[0] |= m->ch & 0x0F;
    nano_link_send_midi(b, len);
}

// ---- Nano pages ------------------------------------------------------------------

static void on_page(unsigned d, unsigned page, const uint8_t *data) {
    if (d >= N_OLED || page >= FB_PAGES) return;
    nano_pages++;
    if (!nano_owned) {
        nano_owned = true;
        printf("nano: owns the displays\n");
    }
    uint32_t irq = spin_lock_blocking(fb_lock);
    fb_set_page(&oled_fb[d], page, data);       // marks the changed column range dirty
    spin_unlock(fb_lock, irq);
}

static void on_config(const uint8_t *p, unsigned len) {
    if (len < 1) return;
    contrast_req = p[0];                        // applied by core 1, which owns the buses
}

static const nano_link_handlers_t nano_handlers = { on_page, on_config };

static void poll_encoders(void) {
    static int32_t prev[N_ENC];
    static bool primed;
    for (unsigned i = 0; i < N_ENC; i++) {
        int32_t pos = encoder_read(i);
        if (!primed) { prev[i] = pos; continue; }
        int32_t delta = (int32_t)((uint32_t)pos - (uint32_t)prev[i]);
        if (!delta) continue;
        prev[i] = pos;
        st.enc_total[i] += delta;
        st.enc_delta[i] = delta;
        oled_stale[i / 2] = true;
        local_ui = true;
        nano_link_send_enc(i, delta);
        printf("enc %u delta %ld total %ld\n", i + 1, (long)delta, (long)st.enc_total[i]);
    }
    primed = true;
}

static void poll_switches(void) {
    while (sw_tail != sw_head) {
        sw_evt_t e = sw_q[sw_tail % SW_Q_LEN];
        sw_tail++;
        st.sw_down[e.id] = e.down;
        oled_stale[e.id / 2] = true;
        local_ui = true;
        nano_link_send_sw(e.id, e.down);
        printf("sw %u %s\n", e.id + 1u, e.down ? "down" : "up");
    }
}

static void poll_midi(uint32_t now) {
    while (midi_tail != midi_head) {
        uint8_t b = midi_ring[midi_tail % MIDI_RING_LEN];
        midi_tail++;
        midi_msg_t m;
        if (!midi_parser_feed(&midi, b, &m)) continue;
        st.midi_last_ms = now ? now : 1;
        set_midi_text(&m);
        send_midi(&m);
        mark_all_stale();
        print_midi(&m);
    }
}

int main(void) {
    stdio_init_all();

    // Give a terminal a moment to attach so boot output is not lost.
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);
    printf("panel: boot\n");
    switches_init();
    printf("panel: switches ok\n");
    encoders_init();
    printf("panel: encoders ok\n");
    midi_parser_init(&midi);
    midi_init();
    nano_link_init();
    i2c_buses_init();
    printf("panel: uarts+i2c ok\n");
    displays_init();
    i2c_go_fast();
    fb_lock = spin_lock_init(spin_lock_claim_unused(true));
    multicore_launch_core1(core1_display_writer);
    printf("panel: displays ok, I2C %lu kHz, display writer on core 1\n", (unsigned long)(i2c_hz / 1000));
    print_banner();

    mark_all_stale();
    uint32_t t_refresh = to_ms_since_boot(get_absolute_time());
    uint32_t t_boot = t_refresh;
    uint32_t t_stat = t_refresh;
    uint32_t last_clock = 0, last_rt = 0, last_drop = 0;
    bool was_connected = stdio_usb_connected();
    bool pattern = true;

    for (;;) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        poll_encoders();
        poll_switches();
        poll_midi(now);
        nano_link_poll(&nano_handlers, now);
        if (nano_owned) {
            nano_link_stats_t ls;
            nano_link_get_stats(&ls);
            if (now - ls.last_rx_ms > NANO_TIMEOUT_MS) {
                nano_owned = false;
                splash_t0 = now;
                mark_all_stale();
                printf("nano: silent for %u ms, local UI\n", NANO_TIMEOUT_MS);
            }
        }

        if (pattern && now - t_boot >= TEST_PATTERN_MS) {
            pattern = false;
            splash_t0 = now;
            mark_all_stale();
        }
        if (!pattern && now - t_refresh >= REFRESH_MS) {
            t_refresh = now;
            refresh_displays(now);
        }

        bool conn = stdio_usb_connected();
        if (conn && !was_connected) print_banner();
        was_connected = conn;

        if (now - t_stat >= 1000) {
            t_stat = now;
            nano_link_send_status();
            nano_link_stats_t ls;
            nano_link_get_stats(&ls);
            printf("oled: bytes %lu/%lu/%lu errors %lu/%lu/%lu\n",
                   (unsigned long)oled[0].bytes, (unsigned long)oled[1].bytes, (unsigned long)oled[2].bytes,
                   (unsigned long)oled[0].errors, (unsigned long)oled[1].errors, (unsigned long)oled[2].errors);
            printf("nano link: %s rx %lu B ok %lu crc %lu cobs %lu drop %lu pages %lu tx %lu\n",
                   nano_owned ? "nano" : "local", (unsigned long)ls.rx_bytes,
                   (unsigned long)ls.rx_ok, (unsigned long)ls.crc_err,
                   (unsigned long)ls.cobs_err, (unsigned long)ls.rx_dropped,
                   (unsigned long)nano_pages, (unsigned long)ls.tx_packets);
            uint32_t drops = sw_dropped + midi_overruns;
            if (midi.clock_count != last_clock || midi.rt_other != last_rt) {
                printf("midi realtime: clock %lu (+%lu) other %lu\n",
                       (unsigned long)midi.clock_count,
                       (unsigned long)(midi.clock_count - last_clock),
                       (unsigned long)midi.rt_other);
                last_clock = midi.clock_count;
                last_rt = midi.rt_other;
            }
            if (drops != last_drop) {
                printf("warn: dropped sw events %lu, midi bytes %lu\n",
                       (unsigned long)sw_dropped, (unsigned long)midi_overruns);
                last_drop = drops;
            }
        }
        sleep_us(200);
    }
}
