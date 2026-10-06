#ifndef UI_H
#define UI_H
#include <stdint.h>

/* M6 UI: the Nano owns the three OLEDs. Panel events (encoders, switches, MIDI)
 * update the state; ui_service() redraws and sends changed pages to Pico B
 * (all pages once a second, as a refresh and keepalive). Main-loop only. */
void ui_init(void);
void ui_service(uint64_t now_ms);
#endif
