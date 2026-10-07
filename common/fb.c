#include "fb.h"

#include <string.h>

#include "font5x7.h"

void fb_mark_dirty(fb_t *fb, unsigned p, unsigned lo, unsigned hi) {
    if (p >= FB_PAGES || lo > hi || hi >= FB_W) return;
    uint8_t bit = (uint8_t)(1u << p);
    if (fb->dirty & bit) {
        if (lo < fb->lo[p]) fb->lo[p] = (uint8_t)lo;
        if (hi > fb->hi[p]) fb->hi[p] = (uint8_t)hi;
    } else {
        fb->lo[p] = (uint8_t)lo;
        fb->hi[p] = (uint8_t)hi;
        fb->dirty |= bit;
    }
}

void fb_clear(fb_t *fb) {
    for (unsigned p = 0; p < FB_PAGES; p++) {
        int lo = -1, hi = -1;
        for (unsigned x = 0; x < FB_W; x++) {
            if (fb->buf[p][x]) {
                if (lo < 0) lo = (int)x;
                hi = (int)x;
            }
        }
        if (lo >= 0) fb_mark_dirty(fb, p, (unsigned)lo, (unsigned)hi);
    }
    memset(fb->buf, 0, sizeof fb->buf);
}

void fb_set_page(fb_t *fb, unsigned page, const uint8_t data[FB_W]) {
    if (page >= FB_PAGES) return;
    uint8_t *row = fb->buf[page];
    unsigned lo = 0, hi = FB_W;
    while (lo < FB_W && row[lo] == data[lo]) lo++;
    if (lo == FB_W) return;                          // unchanged
    while (hi > lo && row[hi - 1] == data[hi - 1]) hi--;
    memcpy(row + lo, data + lo, hi - lo);
    fb_mark_dirty(fb, page, lo, hi - 1);
}

void fb_mark_all_dirty(fb_t *fb) {
    for (unsigned p = 0; p < FB_PAGES; p++) fb_mark_dirty(fb, p, 0, FB_W - 1);
}

void fb_update(fb_t *dst, const fb_t *src) {
    for (unsigned p = 0; p < FB_PAGES; p++)
        fb_set_page(dst, p, src->buf[p]);
}

void fb_pixel(fb_t *fb, int x, int y, bool on) {
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H) return;
    uint8_t m = (uint8_t)(1u << (y & 7));
    uint8_t *b = &fb->buf[y >> 3][x];
    if (on) *b |= m; else *b &= (uint8_t)~m;
}

void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_pixel(fb, x + i, y + j, on);
}

void fb_rect(fb_t *fb, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    fb_fill_rect(fb, x, y, w, 1, true);
    fb_fill_rect(fb, x, y + h - 1, w, 1, true);
    fb_fill_rect(fb, x, y, 1, h, true);
    fb_fill_rect(fb, x + w - 1, y, 1, h, true);
}

void fb_invert_rect(fb_t *fb, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            if (px < 0 || px >= FB_W || py < 0 || py >= FB_H) continue;
            fb->buf[py >> 3][px] ^= (uint8_t)(1u << (py & 7));
        }
    }
}

int fb_text_width(const char *s, int scale) {
    return (int)strlen(s) * FONT_ADV * scale;
}

int fb_text(fb_t *fb, int x, int y, const char *s, int scale, bool on) {
    for (; *s; s++) {
        const uint8_t *g = font5x7_glyph(*s);
        for (int cx = 0; cx < FONT_W; cx++) {
            for (int cy = 0; cy < FONT_H; cy++) {
                if (!((g[cx] >> cy) & 1)) continue;
                fb_fill_rect(fb, x + cx * scale, y + cy * scale, scale, scale, on);
            }
        }
        x += FONT_ADV * scale;
    }
    return x;
}
