// SSD1306 128x64 over I2C: probe, init, page-wise flush of an fb_t.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

#include "fb.h"

typedef struct {
    i2c_inst_t *i2c;
    uint8_t addr;
    bool present;
    uint8_t next_page;          // flush cursor, so a partial flush is fair
    uint32_t errors;
} ssd1306_t;

// True if a device ACKs at addr (address-only write, short timeout).
bool ssd1306_probe(i2c_inst_t *i2c, uint8_t addr);

// Sets up the struct and, if the device answers, sends the init sequence.
// dev->present says whether it worked.
void ssd1306_init(ssd1306_t *dev, i2c_inst_t *i2c, uint8_t addr);

// Writes up to max_pages dirty pages of fb (clearing their dirty bits).
// Returns the number written. A failed write counts an error and leaves the
// page dirty to be retried.
unsigned ssd1306_flush(ssd1306_t *dev, fb_t *fb, unsigned max_pages);
