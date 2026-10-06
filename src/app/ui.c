#include "ui.h"
#include "fb.h"
#include "panel_link.h"
#include "rvpanel.h"
#include "uart.h"

#define N_ENC        6
#define N_OLED       3
#define HALF_W       64
#define TOP          10          /* header height (header row is inverted) */
#define BAR_X        4
#define BAR_W        56
#define BAR_Y        48
#define BAR_H        6
#define FRAME_MS     33          /* ~30 Hz redraw */
#define REFRESH_MS   1000        /* resend every page: lost packets, Pico B reboot */
#define MIDI_FLASH_MS 120

static int32_t enc_total[N_ENC];
static uint8_t sw_down[N_ENC];
static char midi_text[8];
static uint64_t midi_last_ms;
static uint32_t events;

static fb_t drawn;               /* scratch for one display */
static uint8_t sent[N_OLED][FB_PAGES][FB_W];
static uint8_t sent_valid[N_OLED];   /* bit p: sent[d][p] reached the TX ring */
static uint64_t next_frame, next_refresh;

static int fmt_int(char *b, int32_t v, int plus)
{
    char t[12];
    int n = 0, o = 0;
    uint32_t u = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
    if (v < 0) b[o++] = '-';
    else if (plus) b[o++] = '+';
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) b[o++] = t[--n];
    b[o] = 0;
    return o;
}

static void midi_describe(const uint8_t *m, unsigned len)
{
    uint8_t s = m[0] & 0xF0;
    char c = '?';
    int num = len > 1 ? m[1] : -1;
    if (s == 0x90 && len == 3 && m[2]) c = 'N';
    else if (s == 0x80 || s == 0x90) c = 'n';
    else if (s == 0xB0) c = 'C';
    else if (s == 0xC0) c = 'P';
    else if (s == 0xE0) { c = 'B'; num = -1; }
    midi_text[0] = c;
    if (num >= 0) fmt_int(midi_text + 1, num, 0);
    else midi_text[1] = 0;
}

static void handle_event(const panel_event_t *e, uint64_t now)
{
    events++;
    switch (e->type) {
    case RVPANEL_ENC:
        if (e->a < N_ENC) enc_total[e->a] += (int8_t)e->b;
        uart_puts("panel: enc "); uart_put_dec(e->a + 1u);
        uart_puts((int8_t)e->b < 0 ? " -" : " +");
        uart_put_dec((uint64_t)((int8_t)e->b < 0 ? -(int8_t)e->b : (int8_t)e->b));
        uart_puts(" total ");
        if (enc_total[e->a % N_ENC] < 0) { uart_putc('-'); uart_put_dec((uint64_t)-(int64_t)enc_total[e->a % N_ENC]); }
        else uart_put_dec((uint64_t)enc_total[e->a % N_ENC]);
        uart_putc('\n');
        break;
    case RVPANEL_SW:
        if (e->a < N_ENC) sw_down[e->a] = e->b;
        uart_puts("panel: sw "); uart_put_dec(e->a + 1u);
        uart_puts(e->b ? " down\n" : " up\n");
        break;
    case RVPANEL_MIDI: {
        uint8_t m[3] = { e->b, e->c, e->d };
        midi_describe(m, e->a);
        midi_last_ms = now ? now : 1;
        uart_puts("panel: midi");
        for (unsigned i = 0; i < e->a && i < 3; i++) {
            uart_putc(' ');
            uart_putc("0123456789abcdef"[m[i] >> 4]);
            uart_putc("0123456789abcdef"[m[i] & 15]);
        }
        uart_putc('\n');
        break;
    }
    }
}

static void draw_half(fb_t *fb, int x0, unsigned enc)
{
    char buf[16] = "E";
    fmt_int(buf + 1, (int32_t)enc + 1, 0);
    fb_text(fb, x0 + 4, TOP + 2, buf, 1, true);

    fmt_int(buf, enc_total[enc], 0);
    int scale = fb_text_width(buf, 2) <= HALF_W - 4 ? 2 : 1;
    int w = fb_text_width(buf, scale);
    fb_text(fb, x0 + (HALF_W - w) / 2, TOP + 18, buf, scale, true);

    /* Position marker, one lap per 56 counts. */
    fb_rect(fb, x0 + BAR_X, BAR_Y, BAR_W, BAR_H);
    int pos = (int)(((enc_total[enc] % BAR_W) + BAR_W) % BAR_W);
    fb_fill_rect(fb, x0 + BAR_X + pos * (BAR_W - 4) / (BAR_W - 1), BAR_Y + 1, 4, BAR_H - 2, true);

    if (sw_down[enc])
        fb_invert_rect(fb, x0, TOP, HALF_W, FB_H - TOP);
}

static void draw_display(fb_t *fb, unsigned d, uint64_t now)
{
    char buf[16] = "NANO ";
    fb_clear(fb);
    fb_fill_rect(fb, 0, 0, FB_W, TOP - 1, true);   /* inverted header: Nano-drawn */
    fmt_int(buf + 5, (int32_t)d, 0);
    fb_text(fb, 2, 1, buf, 1, false);
    if (midi_text[0]) {
        int w = fb_text_width(midi_text, 1);
        fb_text(fb, FB_W - 10 - w, 1, midi_text, 1, false);
    }
    /* MIDI activity box (dark on the lit header): solid while recent, else an outline. */
    fb_fill_rect(fb, FB_W - 8, 1, 6, 6, false);
    if (!(midi_last_ms && now - midi_last_ms < MIDI_FLASH_MS))
        fb_fill_rect(fb, FB_W - 7, 2, 4, 4, true);
    draw_half(fb, 0, 2 * d);
    draw_half(fb, HALF_W, 2 * d + 1);
    fb_fill_rect(fb, HALF_W - 1, TOP, 1, FB_H - TOP, true);
}

static int page_equal(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < FB_W; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

void ui_init(void)
{
    next_frame = 0;
    next_refresh = 0;
}

void ui_service(uint64_t now)
{
    panel_event_t e;
    while (panel_link_next_event(&e))
        handle_event(&e, now);
    if (now < next_frame)
        return;
    next_frame = now + FRAME_MS;
    if (now >= next_refresh) {
        next_refresh = now + REFRESH_MS;
        for (unsigned d = 0; d < N_OLED; d++)
            sent_valid[d] = 0;
    }
    for (unsigned d = 0; d < N_OLED; d++) {
        draw_display(&drawn, d, now);
        for (unsigned p = 0; p < FB_PAGES; p++) {
            if ((sent_valid[d] >> p & 1) && page_equal(sent[d][p], drawn.buf[p]))
                continue;
            if (panel_link_tx_free() < RVPANEL_MAX_WIRE)
                return;                    /* ring full: rest goes next frame */
            uint8_t pkt[2 + FB_W];
            pkt[0] = (uint8_t)d;
            pkt[1] = (uint8_t)p;
            for (unsigned i = 0; i < FB_W; i++)
                pkt[2 + i] = sent[d][p][i] = drawn.buf[p][i];
            if (panel_link_send(RVPANEL_PAGE, pkt, sizeof pkt) == 0)
                sent_valid[d] |= (uint8_t)(1u << p);
        }
    }
}
