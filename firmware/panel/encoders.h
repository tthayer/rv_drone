// Six quadrature encoders on PIO: enc 0..3 on pio0 SM0..3, enc 4..5 on pio1
// SM0..1, one shared copy of quadrature.pio at offset 0 on each PIO.
#pragma once

#include <stdint.h>

#define ENC_COUNT 6

void encoders_init(void);

// Newest absolute position of encoder i (counts, wraps at 2^32). Counts are
// raw quadrature edges: usually 4 per detent. Reversing A and B flips the
// sign.
int32_t encoder_read(unsigned i);
