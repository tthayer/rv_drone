#ifndef CPUCLK_H
#define CPUCLK_H
#include <stdint.h>

/* Main C906 clock (SG2002 TRM ch. 8). Reset default is fpll/2 = 750 MHz through
 * div_clk_c906_0_1 (clk_sel_0[23] = 0). The vendor FSBL may or may not raise it.
 * 1 GHz = MPLL (default 1000 MHz) / 1 through div_clk_c906_0_0 (clk_sel_0[23] = 1).
 *
 * cpuclk_measure_mhz(): rdcycle against the 25 MHz rdtime over 2.5 ms; 0 if the
 * cycle counter is not accessible from S-mode (the trap is caught) or not running.
 * cpuclk_set_mhz(1000): switches only if MPLL is powered, locked and computes to
 * 900-1100 MHz from its registers, then re-measures and switches back if the
 * result is more than 10 % off. Returns the measured MHz after the attempt.
 * Main-loop context, interrupts may be on (they are masked while measuring). */
struct trap_frame;
uint32_t cpuclk_measure_mhz(void);
uint32_t cpuclk_mpll_mhz(void);          /* MPLL output computed from its registers, 0 if off/unknown */
uint32_t cpuclk_set_mhz(uint32_t mhz);
const char *cpuclk_note(void);           /* what the last set attempt did, for the console */
void cpuclk_dump(void);                  /* clock registers to the console */
int  cpuclk_probe_trap(struct trap_frame *f);
#endif
