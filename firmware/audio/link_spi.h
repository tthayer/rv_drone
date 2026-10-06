// rvlink PIO SPI slave (pio1) + DRQ (Pico A side of the Nano audio link).
#ifndef LINK_SPI_H
#define LINK_SPI_H

#include <stdint.h>
#include "link_validate.h"
#include "audio_ring.h"

// Good non-test frames are pushed into ring; its fill and underruns go in the reply.
void link_spi_init(audio_ring_t *ring);

// Call from the I2S block-completion IRQ (750 Hz), after the ring pop. Raises DRQ
// if armed and the ring is below AUDIO_RING_TARGET.
// Must run at a higher IRQ priority than the link's own IRQs.
void link_spi_on_block(void);

// Copies a consistent-enough snapshot of the counters.
void link_spi_get_stats(link_stats_t *out);

// DRQ requests raised right after a frame because the ring was still short.
uint32_t link_spi_catchups(void);

#endif
