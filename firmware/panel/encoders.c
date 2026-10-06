#include "encoders.h"

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "quadrature.pio.h"

// A pin of each pair; B is the next GPIO.
static const uint8_t enc_pin[ENC_COUNT] = { 0, 2, 6, 10, 12, 14 };

static PIO enc_pio[ENC_COUNT];
static uint enc_sm[ENC_COUNT];
static int32_t last[ENC_COUNT];

void encoders_init(void) {
    PIO pios[2] = { pio0, pio1 };
    for (int k = 0; k < 2; k++) {
        uint off = pio_add_program(pios[k], &quadrature_program);
        hard_assert(off == 0);      // the jump table needs offset 0
    }
    for (unsigned i = 0; i < ENC_COUNT; i++) {
        enc_pio[i] = i < 4 ? pio0 : pio1;
        enc_sm[i] = i < 4 ? i : i - 4;
        pio_sm_claim(enc_pio[i], enc_sm[i]);
        quadrature_program_init(enc_pio[i], enc_sm[i], 0, enc_pin[i]);
    }
    sleep_us(200);      // settle: the first pass may count a spurious edge
    for (unsigned i = 0; i < ENC_COUNT; i++) last[i] = encoder_read(i);
}

// The SM pushes its position every pass but drops pushes while the FIFO is
// full, so what is queued can be stale. Drain it, then take the first entry
// pushed after the drain (as pico-examples' quadrature_encoder does), with a
// bounded wait in case the SM is not running.
int32_t encoder_read(unsigned i) {
    PIO pio = enc_pio[i];
    uint sm = enc_sm[i];
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) last[i] = (int32_t)pio_sm_get(pio, sm);
    for (int spin = 0; spin < 2000 && pio_sm_is_rx_fifo_empty(pio, sm); spin++) {}
    if (!pio_sm_is_rx_fifo_empty(pio, sm)) last[i] = (int32_t)pio_sm_get(pio, sm);
    return last[i];
}
