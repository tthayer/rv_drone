#ifndef PANEL_UI_H
#define PANEL_UI_H
#include <stdint.h>

/* Glue between the panel link (Pico B) and the platform-independent UI
 * (src/ui): panel events -> ui_enc/ui_sw/ui_midi; ui_draw() -> changed pages
 * (all pages once a second) -> PAGE packets. Main-loop only. */
void panel_ui_init(void);
void panel_ui_service(uint64_t now_ms);
#endif
