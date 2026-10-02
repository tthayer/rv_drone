#ifndef PLIC_H
#define PLIC_H
#include <stdint.h>

typedef void (*plic_fn)(void);

void plic_init(void);                           /* S-ctx of hart 0: thr=0, all off */
void plic_register(unsigned irq, plic_fn fn);   /* set handler, priority 1, enable */
void plic_dispatch(void);                       /* claim/call/complete until empty */

#endif
