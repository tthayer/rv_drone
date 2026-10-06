#include "ssd1306.h"

#include <string.h>

#define I2C_TIMEOUT_US  20000

#define CTRL_CMD    0x00
#define CTRL_DATA   0x40

// Segment remap + COM scan direction: flip both (0xA0 / 0xC0) if a module is
// mounted upside down.
static const uint8_t init_seq[] = {
    0xAE,           // display off
    0xD5, 0x80,     // clock divide / oscillator
    0xA8, 0x3F,     // multiplex 1/64
    0xD3, 0x00,     // display offset
    0x40,           // start line 0
    0x8D, 0x14,     // charge pump on
    0x20, 0x02,     // page addressing mode
    0xA1,           // segment remap (column 127 -> SEG0)
    0xC8,           // COM scan reversed
    0xDA, 0x12,     // COM pins: alternative, no remap
    0x81, 0xCF,     // contrast
    0xD9, 0xF1,     // pre-charge
    0xDB, 0x40,     // VCOMH deselect
    0xA4,           // follow RAM
    0xA6,           // normal (not inverted)
    0xAF,           // display on
};

bool ssd1306_probe(i2c_inst_t *i2c, uint8_t addr) {
    // The RP2xxx I2C block can't do a zero-length write (it reports an ACK
    // for any address), so probe with a 1-byte read, as pico-examples' bus_scan does.
    uint8_t dummy;
    return i2c_read_timeout_us(i2c, addr, &dummy, 1, false, I2C_TIMEOUT_US) == 1;
}

static bool write_cmds(ssd1306_t *dev, const uint8_t *cmds, size_t n) {
    uint8_t buf[1 + sizeof init_seq];
    if (n > sizeof init_seq) return false;
    buf[0] = CTRL_CMD;
    memcpy(buf + 1, cmds, n);
    return i2c_write_timeout_us(dev->i2c, dev->addr, buf, n + 1, false, I2C_TIMEOUT_US)
           == (int)(n + 1);
}

void ssd1306_init(ssd1306_t *dev, i2c_inst_t *i2c, uint8_t addr) {
    memset(dev, 0, sizeof *dev);
    dev->i2c = i2c;
    dev->addr = addr;
    if (!ssd1306_probe(i2c, addr)) return;
    dev->present = write_cmds(dev, init_seq, sizeof init_seq);
}

static bool write_page(ssd1306_t *dev, unsigned page, const uint8_t *data) {
    const uint8_t setpos[] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };   // page, col 0
    if (!write_cmds(dev, setpos, sizeof setpos)) return false;
    uint8_t buf[1 + FB_W];
    buf[0] = CTRL_DATA;
    memcpy(buf + 1, data, FB_W);
    return i2c_write_timeout_us(dev->i2c, dev->addr, buf, sizeof buf, false, I2C_TIMEOUT_US)
           == (int)sizeof buf;
}

unsigned ssd1306_flush(ssd1306_t *dev, fb_t *fb, unsigned max_pages) {
    unsigned done = 0;
    if (!dev->present) return 0;
    for (unsigned i = 0; i < FB_PAGES && done < max_pages; i++) {
        unsigned p = (dev->next_page + i) % FB_PAGES;
        if (!(fb->dirty & (1u << p))) continue;
        if (write_page(dev, p, fb->buf[p])) {
            fb->dirty &= (uint8_t)~(1u << p);
            done++;
        } else {
            dev->errors++;
            break;      // bus trouble: try again next refresh
        }
        dev->next_page = (uint8_t)((p + 1) % FB_PAGES);
    }
    return done;
}
