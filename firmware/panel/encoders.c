#include "encoders.h"

#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "quadrature.pio.h"

// A pin of each pair; B is the next GPIO.
static const uint8_t enc_pin[ENC_COUNT] = { 0, 2, 6, 10, 12, 14 };
// Clockwise = positive. The KY-040 modules (enc 1-5) put CLK/DT the other way
// round from the bare encoder on enc 6.
static const bool enc_invert[ENC_COUNT] = { true, true, true, true, true, false };

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

// The SM pushes its position every pass (dropping pushes while the FIFO is
// full), so queued entries can be stale. As in pico-examples'
// quadrature_encoder: read exactly level + 1 entries. The last one was pushed
// after this call started. Never drain "until empty": the SM refills faster
// than the CPU reads, so that loop can spin forever.
int32_t encoder_read(unsigned i) {
    PIO pio = enc_pio[i];
    uint sm = enc_sm[i];
    for (uint n = pio_sm_get_rx_fifo_level(pio, sm) + 1; n > 0; n--)
        last[i] = (int32_t)pio_sm_get_blocking(pio, sm);
    return enc_invert[i] ? -last[i] : last[i];
}
