// Switch debounce: a switch changes state only after the raw input has
// disagreed with the stable state for DEBOUNCE_SAMPLES consecutive samples.
// Called at 1 kHz, that is 5 ms. Pure C, tested on the host.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DEBOUNCE_SAMPLES 5

typedef struct {
    bool state;             // stable state, true = pressed
    uint8_t count;          // consecutive samples that differ from `state`
} debounce_t;

// Returns true when the stable state just changed.
static inline bool debounce_step(debounce_t *d, bool raw) {
    if (raw == d->state) {
        d->count = 0;
        return false;
    }
    if (++d->count < DEBOUNCE_SAMPLES) return false;
    d->state = raw;
    d->count = 0;
    return true;
}

#define DEBOUNCE_MAX_CH 8

typedef struct {
    debounce_t ch[DEBOUNCE_MAX_CH];
} debounce_bank_t;

// raw_mask: bit i set = channel i pressed right now. Returns the mask of
// channels whose stable state changed this sample; read the new state from
// bank->ch[i].state.
static inline uint8_t debounce_bank_step(debounce_bank_t *b, unsigned n, uint8_t raw_mask) {
    uint8_t changed = 0;
    for (unsigned i = 0; i < n && i < DEBOUNCE_MAX_CH; i++)
        if (debounce_step(&b->ch[i], (raw_mask >> i) & 1u)) changed |= (uint8_t)(1u << i);
    return changed;
}
