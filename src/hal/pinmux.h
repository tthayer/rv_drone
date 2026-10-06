#ifndef PINMUX_H
#define PINMUX_H
#include <stdint.h>

void pinmux_set(uint32_t fmux_off, uint32_t func);   /* FMUX reg, function select [2:0] */
uint32_t pinmux_get(uint32_t fmux_off);
#endif
