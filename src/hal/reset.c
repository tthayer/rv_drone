/* Whole-chip warm reset. Nano: RTC-domain request, as in vendor U-Boot
 * cv_system_reset(). QEMU: SBI SRST cold reboot. */
#include <stdint.h>
#include "reset.h"
#include "board.h"

#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))

void board_reset(void)
{
#ifdef BOARD_NANO
    R32(0x05026000u + 0xcc) = 1;                    /* RTC_EN_WARM_RST_REQ */
    while (R32(0x05026000u + 0xcc) != 1)
        ;
    R32(0x05025000u + 0x4) = 0xab18;                /* RTC_CTRL0_UNLOCKKEY */
    R32(0x05025000u + 0x8) |= 0xffff0800u | 0x10u;  /* RTC_CTRL0: warm reset */
#else
    register long a0 __asm__("a0") = 1;             /* cold reboot */
    register long a1 __asm__("a1") = 0;
    register long a6 __asm__("a6") = 0;
    register long a7 __asm__("a7") = 0x53525354;    /* SRST */
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
#endif
    for (;;)
        __asm__ volatile("wfi");
}
