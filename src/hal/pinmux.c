/* FMUX (pad function select). Offsets/values are board constants. */
#include "pinmux.h"
#include "board.h"

#define FMUX(o) (*(volatile uint32_t *)(uintptr_t)(BOARD_FMUX_BASE + (o)))

void pinmux_set(uint32_t fmux_off, uint32_t func)
{
    FMUX(fmux_off) = func & 7u;
}

uint32_t pinmux_get(uint32_t fmux_off)
{
    return FMUX(fmux_off);
}
