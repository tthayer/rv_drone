// 128x64 1-bpp framebuffer in SSD1306 page layout (8 pages x 128 columns,
// bit 0 of each byte is the top row of its page) plus a dirty-page mask.
// No hardware: rendering code draws into one of these, and ssd1306.c flushes
// the dirty pages. Shared: the Nano UI draws with it too, and Pico B puts the
// Nano's PAGE packets straight into fb_set_page().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FB_W        128
#define FB_H        64
#define FB_PAGES    (FB_H / 8)

typedef struct {
    uint8_t buf[FB_PAGES][FB_W];
    uint8_t dirty;                  // bit p set = page p needs flushing
    uint8_t lo[FB_PAGES], hi[FB_PAGES];  // dirty column range of page p (valid while dirty)
} fb_t;

void fb_clear(fb_t *fb);                                // all off; marks changed pages
void fb_set_page(fb_t *fb, unsigned page, const uint8_t data[FB_W]);
void fb_mark_all_dirty(fb_t *fb);
// Marks columns lo..hi of page p dirty, widening any range already pending.
void fb_mark_dirty(fb_t *fb, unsigned page, unsigned lo, unsigned hi);

// Copies src into dst, marking only the pages (and column ranges) that differ.
void fb_update(fb_t *dst, const fb_t *src);

void fb_pixel(fb_t *fb, int x, int y, bool on);         // clipped
void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, bool on);
void fb_rect(fb_t *fb, int x, int y, int w, int h);     // 1 px outline, on
void fb_invert_rect(fb_t *fb, int x, int y, int w, int h);

// Text with the built-in 5x7 font in a 6x8 cell (x advances 6*scale).
// Returns the x after the last character. Draws "on" pixels only, or the
// opposite if `on` is false.
int fb_text(fb_t *fb, int x, int y, const char *s, int scale, bool on);
int fb_text_width(const char *s, int scale);
