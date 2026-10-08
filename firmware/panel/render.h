// Local rendering of a panel display into a framebuffer (two encoder halves
// plus a header row). No hardware; the same code runs in the host test.
#pragma once

#include "fb.h"
#include "panel_state.h"

#define MIDI_FLASH_MS   120     // header activity box stays lit this long

// Renders display `oled` (0..2): encoders 2*oled and 2*oled+1.
void render_display(fb_t *out, unsigned oled, const panel_state_t *s, uint32_t now_ms);

// True while the header MIDI box is lit, so the caller knows to re-render
// when it times out.
bool render_midi_lit(const panel_state_t *s, uint32_t now_ms);

// Boot-time test pattern: border, diagonals, name and I2C address.
void render_test_pattern(fb_t *out, unsigned oled, unsigned addr);

// Boot splash while the Nano boots: title, status and a drone waveform that
// scrolls across all three displays as one 384 px strip. t_ms = time since
// the splash started (the wave swells in over the first 1.5 s).
void render_splash(fb_t *out, unsigned oled, uint32_t t_ms);
