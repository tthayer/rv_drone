// Classic 5x7 ASCII font (0x20..0x7E), column-major, bit 0 = top row.
// Glyphs sit in a 6x8 cell: 5 columns plus 1 blank, rows 0..7 (descenders of
// , g j p q y use row 7). Unknown characters draw as '?'.
#pragma once

#include <stdint.h>

#define FONT_W    5
#define FONT_H    8
#define FONT_ADV  6     // cell width

const uint8_t *font5x7_glyph(char c);
