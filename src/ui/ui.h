/* Control surface: pages of 6 parameters on the 6 encoders, MIDI routing, and
 * drawing of the three 128x64 displays. No hardware: the Nano (app/panel_ui.c)
 * and the host (emu/) both drive it. Not thread-safe: call from the same
 * context as engine_render(), or lock around it (emu). */
#ifndef UI_H
#define UI_H
#include <stdint.h>
#include "fb.h"

#define UI_DISPLAYS 3
/* Pico B forwards raw quadrature counts: one full cycle (4 counts) per click on
 * both the KY-040 modules and the bare EC11 (measured 2026-10-06). */
#define UI_COUNTS_PER_DETENT 4

void ui_init(void);                          /* after engine_init(): reads defaults */
void ui_enc(int id, int delta);              /* id 0..5, raw quadrature counts */
void ui_sw(int id, int down);                /* enc 1 push = next page, others = reset */
void ui_midi(const uint8_t *msg, int len, uint64_t now_ms);  /* one channel message */
void ui_set_load(int cpu_pct);               /* shown in the header */
void ui_draw(int display, fb_t *fb, uint64_t now_ms);
#endif
