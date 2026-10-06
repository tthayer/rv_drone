// Pico B (panel): 6 encoders (PIO quadrature), 6 switches (1 kHz debounce),
// 3 SSD1306 OLEDs, MIDI in (UART1 RX), link stub to the Nano (UART0).
//
// Events go to the USB CDC console. The local UI is a stand-in: each OLED
// shows its two encoders' totals. fb_t + ssd1306_flush() are independent of
// render.c, so in M6 pages received from the Nano replace local rendering.
//
// Core 0 only. Switches are sampled by a 1 kHz repeating timer; MIDI bytes are
// collected by the UART1 RX interrupt so the (blocking) I2C flushes cannot
// overflow the 32-byte UART FIFO; encoder counts live in the PIO and are
// absolute, so nothing is lost while the loop is busy.

#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

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
#define I2C_HZ          400000

#define MIDI_UART       uart1
#define MIDI_UART_IRQ   UART1_IRQ
#define PIN_MIDI_RX     21
#define MIDI_BAUD       31250

#define REFRESH_MS      33      // ~30 Hz
#define PAGES_PER_TICK  3       // I2C budget per display per refresh
#define TEST_PATTERN_MS 1500

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

static void mark_all_stale(void) {
    for (unsigned d = 0; d < N_OLED; d++) oled_stale[d] = true;
}

static void i2c_buses_init(void) {
    i2c_init(i2c0, I2C_HZ);
    gpio_set_function(PIN_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C0_SDA);
    gpio_pull_up(PIN_I2C0_SCL);
    i2c_init(i2c1, I2C_HZ);
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
    printf("\n== rv_drone panel (Pico B), M3 ==\n");
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
    printf("nano link: UART0 GP16/GP17, %u baud (stub)\n", NANO_LINK_BAUD);
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

static void refresh_displays(uint32_t now) {
    bool lit = render_midi_lit(&st, now);
    if (lit != midi_was_lit) {              // box turned on or timed out
        midi_was_lit = lit;
        mark_all_stale();
    }
    for (unsigned d = 0; d < N_OLED; d++) {
        if (!oled[d].present) continue;
        if (oled_stale[d]) {
            render_display(&scratch, d, &st, now);
            fb_update(&oled_fb[d], &scratch);   // only changed pages become dirty
            oled_stale[d] = false;
        }
        ssd1306_flush(&oled[d], &oled_fb[d], PAGES_PER_TICK);
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
        mark_all_stale();
        print_midi(&m);
    }
}

int main(void) {
    stdio_init_all();

    switches_init();
    encoders_init();
    midi_parser_init(&midi);
    midi_init();
    nano_link_init();
    i2c_buses_init();

    // Give a terminal a moment to attach so the banner is not lost.
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);
    displays_init();
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
        nano_link_poll();

        if (pattern && now - t_boot >= TEST_PATTERN_MS) {
            pattern = false;
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
