#include "render.h"

#include <stdio.h>

#define HALF_W      64
#define TOP         8           // header height
#define BAR_X       4
#define BAR_W       56
#define BAR_Y       46
#define BAR_H       6

bool render_midi_lit(const panel_state_t *s, uint32_t now_ms) {
    return s->midi_last_ms != 0 && (uint32_t)(now_ms - s->midi_last_ms) < MIDI_FLASH_MS;
}

static void draw_half(fb_t *fb, int x0, unsigned enc, const panel_state_t *s) {
    char buf[16];

    snprintf(buf, sizeof buf, "ENC %u", enc + 1);
    fb_text(fb, x0 + 4, TOP + 3, buf, 1, true);

    // Total, large when it fits in the 64 px half (5 chars at 12 px = 60).
    snprintf(buf, sizeof buf, "%ld", (long)s->enc_total[enc]);
    int scale = fb_text_width(buf, 2) <= HALF_W - 4 ? 2 : 1;
    int w = fb_text_width(buf, scale);
    fb_text(fb, x0 + (HALF_W - w) / 2, TOP + 17, buf, scale, true);

    // Position: a marker on a track, one lap per 56 counts (wraps both ways).
    fb_rect(fb, x0 + BAR_X, BAR_Y, BAR_W, BAR_H);
    int pos = (int)(((s->enc_total[enc] % BAR_W) + BAR_W) % BAR_W);
    int mw = 4;
    int mx = BAR_X + pos * (BAR_W - mw) / (BAR_W - 1);
    fb_fill_rect(fb, x0 + mx, BAR_Y + 1, mw, BAR_H - 2, true);

    snprintf(buf, sizeof buf, "d%+ld", (long)s->enc_delta[enc]);
    fb_text(fb, x0 + 4, TOP + 47, buf, 1, true);

    if (s->sw_down[enc]) fb_invert_rect(fb, x0, TOP, HALF_W, FB_H - TOP);
}

void render_display(fb_t *out, unsigned oled, const panel_state_t *s, uint32_t now_ms) {
    char buf[16];

    fb_clear(out);

    snprintf(buf, sizeof buf, "OLED %u", oled);
    fb_text(out, 0, 0, buf, 1, true);

    // MIDI activity: last message text, right-aligned, and a box that is
    // filled while the activity is recent.
    if (s->midi_text[0]) {
        int w = fb_text_width(s->midi_text, 1);
        fb_text(out, FB_W - 10 - w, 0, s->midi_text, 1, true);
    }
    if (render_midi_lit(s, now_ms)) fb_fill_rect(out, FB_W - 7, 1, 6, 6, true);
    else fb_rect(out, FB_W - 7, 1, 6, 6);

    draw_half(out, 0, 2 * oled, s);
    draw_half(out, HALF_W, 2 * oled + 1, s);
}

void render_test_pattern(fb_t *out, unsigned oled, unsigned addr) {
    char buf[24];

    fb_clear(out);
    fb_rect(out, 0, 0, FB_W, FB_H);
    for (int x = 0; x < FB_W; x++) {
        fb_pixel(out, x, x * (FB_H - 1) / (FB_W - 1), true);
        fb_pixel(out, x, (FB_H - 1) - x * (FB_H - 1) / (FB_W - 1), true);
    }
    snprintf(buf, sizeof buf, "OLED %u 0x%02X", oled, addr);
    int w = fb_text_width(buf, 1) + 6;
    int x = (FB_W - w) / 2;
    fb_fill_rect(out, x, 28, w, 10, false);
    fb_text(out, x + 3, 29, buf, 1, true);
}
