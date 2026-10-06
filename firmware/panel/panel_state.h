// What the panel knows locally: encoder totals, switch states, MIDI activity.
// The local renderer (render.c) turns this into the three framebuffers. In M6
// the Nano owns the UI and sends pages instead, so render.c is bypassed and
// only this state is reported upstream.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define N_ENC       6
#define N_OLED      3

typedef struct {
    int32_t enc_total[N_ENC];       // raw quadrature counts since boot
    int32_t enc_delta[N_ENC];       // last non-zero change
    bool sw_down[N_ENC];
    uint32_t midi_last_ms;          // time of last MIDI byte-level activity, 0 = never
    char midi_text[8];              // short description of the last message
} panel_state_t;
