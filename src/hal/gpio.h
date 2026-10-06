#ifndef GPIO_H
#define GPIO_H
#include <stdint.h>

typedef void (*gpio_fn)(void);

void gpio_init(void);                                 /* register GPIO0 IRQ with the PLIC */
void gpio_set_input(unsigned bit);
int  gpio_read(unsigned bit);                         /* EXT_PORTA */
/* Edge interrupt on bit; fn runs from the PLIC dispatch (keep it short). */
void gpio_irq_edge(unsigned bit, int rising, gpio_fn fn);

extern volatile uint32_t gpio_irq_count;
#endif
