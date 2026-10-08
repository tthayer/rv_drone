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

// ---- boot splash -------------------------------------------------------------

#define SPLASH_MID      42      // wave centre row
#define SPLASH_SWELL_MS 1500    // amplitude ramps up over this long

// Integer sine for a 32-bit phase (2^32 = one turn): a parabola per half turn,
// -1024..1024. Close enough for 64 pixels, and no libm.
static int isin(uint32_t ph) {
    uint32_t u = (ph >> 22) & 511u;             // position within the half turn
    int s = (int)((u * (512u - u)) >> 6);       // 0..1024
    return (ph & 0x80000000u) ? -s : s;
}

// Wave row at canvas column x (0..383, all three displays side by side).
static int splash_y(int x, uint32_t t_ms) {
    uint32_t ux = (uint32_t)x;
    int s1 = isin(ux * 22369621u + t_ms * 1789569u);        // 192 px, 2.4 s
    int s2 = isin(ux * 57266230u - t_ms * 2526451u);        // 75 px, 1.7 s
    int s3 = isin(ux * 138547333u + t_ms * 4772185u);       // 31 px, 0.9 s
    int env = t_ms >= SPLASH_SWELL_MS ? 1024 : (int)(t_ms * 1024u / SPLASH_SWELL_MS);
    env = env * (768 + isin(t_ms * 1073742u) / 4) / 1024;   // breathes every 4 s
    int sum = 12 * s1 + 6 * s2 + 3 * s3;                    // up to 21 px
    return SPLASH_MID + sum * env / (1024 * 1024);          // fits int32: 21504 * 1024
}

void render_splash(fb_t *out, unsigned oled, uint32_t t_ms) {
    char buf[16];
    fb_clear(out);
    if (oled == 1) {
        fb_text(out, (FB_W - fb_text_width("RV DRONE", 2)) / 2, 0, "RV DRONE", 2, true);
    } else if (oled == 0) {
        unsigned dots = (t_ms / 350u) % 4u;
        snprintf(buf, sizeof buf, "booting%.*s", (int)dots, "...");
        fb_text(out, 4, 4, buf, 1, true);
    } else {
        snprintf(buf, sizeof buf, "%lus", (unsigned long)(t_ms / 1000u));
        fb_text(out, FB_W - 4 - fb_text_width(buf, 1), 4, buf, 1, true);
    }
    for (int x = 0; x < FB_W; x += 4) fb_pixel(out, x, SPLASH_MID, true);
    int x0 = (int)oled * FB_W;
    int prev = splash_y(x0 - 1, t_ms);
    for (int x = 0; x < FB_W; x++) {
        int y = splash_y(x0 + x, t_ms);
        int lo = y < prev ? y : prev, hi = y < prev ? prev : y;
        if (hi > lo) lo++;                      // join to the previous column
        fb_fill_rect(out, x, lo, 1, hi - lo + 1, true);
        prev = y;
    }
}
