// SSD1306 128x64 over I2C: probe, init, writes of dirty column ranges.
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
    uint32_t bytes;             // display-data bytes written (stats)
} ssd1306_t;

// True if a device ACKs at addr (address-only write, short timeout).
bool ssd1306_probe(i2c_inst_t *i2c, uint8_t addr);

// Sets up the struct and, if the device answers, sends the init sequence.
// dev->present says whether it worked.
void ssd1306_init(ssd1306_t *dev, i2c_inst_t *i2c, uint8_t addr);

// Writes columns lo..hi of one page from row[] (a full 128-byte page buffer).
bool ssd1306_write_range(ssd1306_t *dev, unsigned page, unsigned lo, unsigned hi,
                         const uint8_t *row);

// Harmless command (0xE3 NOP): checks the device still ACKs, e.g. after a bus speed change.
bool ssd1306_nop(ssd1306_t *dev);

// Writes up to max_pages dirty pages of fb, only their dirty column ranges
// (clearing their dirty bits). Single-core use only (boot); core 1 does the rest.
// Returns the number written. A failed write counts an error and leaves the
// page dirty to be retried.
unsigned ssd1306_flush(ssd1306_t *dev, fb_t *fb, unsigned max_pages);

// Contrast 0..255 (command 0x81). No-op if the device is not present.
void ssd1306_set_contrast(ssd1306_t *dev, uint8_t contrast);
