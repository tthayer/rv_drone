#ifndef TIMER_H
#define TIMER_H
#include <stdint.h>

extern volatile uint64_t timer_ticks;

static inline uint64_t rdtime(void)
{
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

void timer_init(void);      /* start periodic tick at BOARD_TICK_HZ */
void timer_handler(void);   /* from trap dispatcher */

#endif
