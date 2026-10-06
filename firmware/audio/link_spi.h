// rvlink PIO SPI slave (pio1) + DRQ (Pico A side of the Nano audio link).
#ifndef LINK_SPI_H
#define LINK_SPI_H

#include <stdint.h>
#include "link_validate.h"

// underruns_src: counter reported to the Nano as "underruns" (audio late count).
void link_spi_init(const volatile uint32_t *underruns_src);

// Call from the I2S block-completion IRQ (750 Hz). Raises DRQ if armed.
// Must run at a higher IRQ priority than the link's own IRQs.
void link_spi_on_block(void);

// Copies a consistent-enough snapshot of the counters.
void link_spi_get_stats(link_stats_t *out);

#endif
