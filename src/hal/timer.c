/* Periodic tick via SBI TIME; deadline advances by a fixed period (no drift). */
#include "timer.h"
#include "board.h"
#include "sbi.h"

#define PERIOD (BOARD_TIMEBASE_HZ / BOARD_TICK_HZ)

volatile uint64_t timer_ticks;
static uint64_t deadline;

void timer_init(void)
{
    deadline = rdtime() + PERIOD;
    sbi_set_timer(deadline);
}

void timer_handler(void)
{
    timer_ticks++;
    deadline += PERIOD;
    sbi_set_timer(deadline);
}
