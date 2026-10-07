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
/* MIDI clock event (ENGINE_CLK_*), t_us = sender timestamp; tempo shows in the header. */
void ui_clock(int kind, uint32_t t_us);
void ui_draw(int display, fb_t *fb, uint64_t now_ms);

/* Presets (last page, PRESET): enc 1 turn = slot 1..16, enc 2 push = load,
 * enc 3 push = save. Files are text, one NAME=position (0..10000) per line,
 * named P01.TXT..P16.TXT; LAST.TXT holds the slot to load at boot. The store
 * maps names to files (FatFs /presets on the Nano, a directory in emu). */
typedef struct {
    int (*read)(const char *name, char *buf, int max);       /* bytes read, or <0 */
    int (*write)(const char *name, const char *buf, int len); /* 0, or <0 */
} ui_store_t;
#define UI_PRESET_SLOTS 16
void ui_set_store(const ui_store_t *store);
int  ui_preset_load(int slot);     /* 0 ok, -1 no store/card, -2 empty, -3 bad file */
int  ui_preset_save(int slot);     /* 0 ok, <0 error */
void ui_boot_preset(void);         /* loads the LAST.TXT slot, if any */
#endif
