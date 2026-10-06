#include "ui.h"
#include "engine.h"
#include "params.h"

#define HALF_W       64
#define HDR_H        9           /* row a: inverted header */
#define NAME_Y       12          /* row b */
#define VALUE_Y      26          /* row c */
#define BAR_Y        50          /* row d */
#define BAR_H        7
#define COUNTS_PER_DETENT 2      /* quadrature counts per click (KY-040 / EC11 on Pico B PIO) */
#define MIDI_FLASH_MS 120
#define TOUCH_MS     400         /* a half is highlighted this long after a change */

static float norm[P_COUNT];
static int page;
static int sw_down[6];
static int enc_accum[6];
static int load_pct = -1;
static char midi_text[8];
static uint64_t midi_last, now_cached;
static uint64_t touched_at[6];

static int page_param(int enc) { return page * PAGE_PARAMS + enc; }

static void apply(int id)
{
    engine_set_param(id, param_to_value(id, norm[id]));
}

void ui_init(void)
{
    for (int i = 0; i < P_COUNT; i++)
        norm[i] = param_to_norm(i, engine_param(i));
    page = 0;
}

void ui_enc(int id, int delta)
{
    if (id < 0 || id > 5) return;
    enc_accum[id] += delta;
    int det = enc_accum[id] / COUNTS_PER_DETENT;
    if (!det) return;
    enc_accum[id] -= det * COUNTS_PER_DETENT;
    int p = page_param(id);
    const param_desc_t *d = param_desc(p);
    int mag = det < 0 ? -det : det;
    float step = d->step * (mag >= 3 ? 4.0f : 1.0f);   /* fast turns accelerate */
    float n = norm[p] + (float)det * step;
    if (d->curve == CURVE_ENUM || d->curve == CURVE_INT) {
        /* move at least one whole value per detent */
        float span = d->max - d->min;
        float v = param_to_value(p, norm[p]) + (float)(det > 0 ? 1 : -1) * (mag >= 3 ? 2 : 1);
        if (d->curve == CURVE_ENUM && v > d->max) v = d->min;      /* enums wrap */
        if (d->curve == CURVE_ENUM && v < d->min) v = d->max;
        n = (v - d->min) / span;
    }
    norm[p] = n < 0 ? 0 : n > 1 ? 1 : n;
    apply(p);
    touched_at[id] = now_cached ? now_cached : 1;
}

void ui_sw(int id, int down)
{
    if (id < 0 || id > 5) return;
    sw_down[id] = down;
    if (!down) return;
    if (id == 0) {
        page = (page + 1) % PAGE_COUNT;
        return;
    }
    int p = page_param(id);
    norm[p] = param_to_norm(p, param_desc(p)->def);
    apply(p);
}

static char *put_dec(char *b, int v)
{
    char t[8];
    int n = 0;
    if (v < 0) { *b++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *b++ = t[--n];
    *b = 0;
    return b;
}

void ui_midi(const uint8_t *m, int len, uint64_t now)
{
    if (len < 1) return;
    uint8_t st = m[0] & 0xF0;
    char c = 0;
    int num = len > 1 ? m[1] : 0;
    if (st == 0x90 && len == 3 && m[2]) { engine_note_on(m[1], m[2]); c = 'N'; }
    else if ((st == 0x80 || st == 0x90) && len == 3) { engine_note_off(m[1]); c = 'n'; }
    else if (st == 0xB0 && len == 3) {
        c = 'C';
        if (m[1] == 123) engine_all_off();                  /* all notes off */
        int p = param_for_cc(m[1]);
        if (p >= 0) {
            norm[p] = (float)m[2] * (1.0f / 127.0f);
            apply(p);
        }
    }
    else if (st == 0xC0) { c = 'P'; }
    else if (st == 0xE0) { c = 'B'; num = -1; }
    if (!c) return;
    midi_text[0] = c;
    if (num >= 0) put_dec(midi_text + 1, num);
    else midi_text[1] = 0;
    midi_last = now ? now : 1;
}

void ui_set_load(int pct) { load_pct = pct; }

static void draw_half(fb_t *fb, int x0, int enc, uint64_t now)
{
    int p = page_param(enc);
    const param_desc_t *d = param_desc(p);
    char buf[16];

    fb_text(fb, x0 + 3, NAME_Y, d->name, 1, true);

    param_format(p, param_to_value(p, norm[p]), buf);
    int scale = fb_text_width(buf, 2) <= HALF_W - 4 ? 2 : 1;
    int w = fb_text_width(buf, scale);
    fb_text(fb, x0 + (HALF_W - w) / 2, scale == 2 ? VALUE_Y : VALUE_Y + 4, buf, scale, true);

    /* bar: fill to the knob position */
    fb_rect(fb, x0 + 3, BAR_Y, HALF_W - 6, BAR_H);
    int fill = (int)(norm[p] * (float)(HALF_W - 10) + 0.5f);
    if (fill > 0)
        fb_fill_rect(fb, x0 + 5, BAR_Y + 2, fill, BAR_H - 4, true);

    if (touched_at[enc] && now - touched_at[enc] < TOUCH_MS)
        fb_rect(fb, x0 + 1, HDR_H + 1, HALF_W - 2, FB_H - HDR_H - 1);   /* "just moved" frame */
    if (sw_down[enc])
        fb_invert_rect(fb, x0, HDR_H, HALF_W, FB_H - HDR_H);
}

void ui_draw(int disp, fb_t *fb, uint64_t now)
{
    char buf[20];
    now_cached = now;
    fb_clear(fb);
    fb_fill_rect(fb, 0, 0, FB_W, HDR_H - 1, true);
    switch (disp) {
    case 0: {                                   /* page name + x/N */
        fb_text(fb, 2, 0, page_names[page], 1, false);
        char *b = put_dec(buf, page + 1);
        *b++ = '/';
        put_dec(b, PAGE_COUNT);
        fb_text(fb, FB_W - 2 - fb_text_width(buf, 1), 0, buf, 1, false);
        break;
    }
    case 1: {                                   /* MIDI: last message + activity box */
        fb_text(fb, 2, 0, "MIDI", 1, false);
        if (midi_text[0])
            fb_text(fb, 32, 0, midi_text, 1, false);
        fb_fill_rect(fb, FB_W - 9, 1, 6, 6, false);
        if (!(midi_last && now - midi_last < MIDI_FLASH_MS))
            fb_fill_rect(fb, FB_W - 8, 2, 4, 4, true);
        break;
    }
    default: {                                  /* voices + CPU load */
        char *b = buf;
        *b++ = 'V';
        b = put_dec(b, engine_voices_active());
        fb_text(fb, 2, 0, buf, 1, false);
        if (load_pct >= 0) {
            b = buf;
            b = put_dec(b, load_pct);
            *b++ = '%';
            *b = 0;
            fb_text(fb, 30, 0, "CPU", 1, false);
            fb_text(fb, FB_W - 2 - fb_text_width(buf, 1), 0, buf, 1, false);
        }
        break;
    }
    }
    draw_half(fb, 0, 2 * disp, now);
    draw_half(fb, HALF_W, 2 * disp + 1, now);
    fb_fill_rect(fb, HALF_W - 1, HDR_H, 1, FB_H - HDR_H, true);
}
