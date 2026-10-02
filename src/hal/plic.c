/* Minimal PLIC driver, S-mode context of hart 0.
 * Nano: "thead,c900-plic" at 0x70000000 (UNVERIFIED); OpenSBI sets the T-Head
 * S-mode access-control bit, we do not touch it. */
#include "plic.h"
#include "board.h"

#define MAX_IRQ 128
#define PRIO(i)  (BOARD_PLIC_BASE + 4u * (i))
#define ENABLE(w) (BOARD_PLIC_BASE + 0x2000u + BOARD_PLIC_SCTX * 0x80u + 4u * (w))
#define THRESH   (BOARD_PLIC_BASE + 0x200000u + BOARD_PLIC_SCTX * 0x1000u)
#define CLAIM    (THRESH + 4)

#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static plic_fn handlers[MAX_IRQ];

void plic_init(void)
{
    for (unsigned w = 0; w < MAX_IRQ / 32; w++)
        R32(ENABLE(w)) = 0;
    R32(THRESH) = 0;
}

void plic_register(unsigned irq, plic_fn fn)
{
    if (irq == 0 || irq >= MAX_IRQ)
        return;
    handlers[irq] = fn;
    R32(PRIO(irq)) = 1;
    R32(ENABLE(irq / 32)) |= 1u << (irq % 32);
}

void plic_dispatch(void)
{
    uint32_t irq;
    while ((irq = R32(CLAIM)) != 0) {
        if (irq < MAX_IRQ && handlers[irq])
            handlers[irq]();
        R32(CLAIM) = irq;
    }
}
