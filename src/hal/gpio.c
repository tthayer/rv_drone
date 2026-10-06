/* DW APB GPIO, port A (GPIO0). Edge interrupts only; one handler per bit. */
#include "gpio.h"
#include "board.h"
#include "plic.h"

#define R32(o) (*(volatile uint32_t *)(uintptr_t)(BOARD_GPIO_BASE + (o)))
#define DDR        0x04
#define INTEN      0x30
#define INTMASK    0x34
#define INTTYPE    0x38   /* 1 = edge */
#define INT_POL    0x3C   /* 1 = rising / high */
#define INTSTATUS  0x40
#define EOI        0x4C
#define EXT_PORTA  0x50

static gpio_fn handlers[32];
volatile uint32_t gpio_irq_count;

static void gpio_isr(void)
{
    uint32_t st = R32(INTSTATUS);
    R32(EOI) = st;                       /* edge: clear before running handlers */
    gpio_irq_count++;
    for (unsigned b = 0; st; b++, st >>= 1)
        if ((st & 1) && handlers[b])
            handlers[b]();
}

void gpio_init(void)
{
    R32(INTMASK) = 0xffffffffu;
    R32(INTEN) = 0;
    R32(EOI) = 0xffffffffu;
    plic_register(BOARD_GPIO_IRQ, gpio_isr);
}

void gpio_set_input(unsigned bit)
{
    R32(DDR) &= ~(1u << bit);
}

int gpio_read(unsigned bit)
{
    return (R32(EXT_PORTA) >> bit) & 1;
}

void gpio_irq_edge(unsigned bit, int rising, gpio_fn fn)
{
    uint32_t m = 1u << bit;
    handlers[bit] = fn;
    R32(INTMASK) |= m;
    R32(INTTYPE) |= m;
    if (rising)
        R32(INT_POL) |= m;
    else
        R32(INT_POL) &= ~m;
    R32(INTEN) |= m;
    R32(EOI) = m;                        /* drop edges raised by the reconfig */
    R32(INTMASK) &= ~m;
}
