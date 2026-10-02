/* Minimal SBI helpers (OpenSBI runs in M-mode below us). */
#ifndef SBI_H
#define SBI_H
#include <stdint.h>

/* Legacy console putchar (extension 0x01, function id ignored). Debug fallback
 * for when the UART driver is suspect. Deprecated in SBI spec but OpenSBI
 * still implements it. */
static inline void sbi_console_putchar(int ch)
{
    register long a0 __asm__("a0") = ch;
    register long a7 __asm__("a7") = 0x01;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory", "a1");
}

/* TIME extension (EID "TIME", FID 0): arm the S-mode timer; also clears STIP.
 * Falls back to legacy set_timer (EID 0) if the extension errors. */
static inline void sbi_set_timer(uint64_t t)
{
    register long a0 __asm__("a0") = (long)t;
    register long a1 __asm__("a1") = 0;
    register long a6 __asm__("a6") = 0;
    register long a7 __asm__("a7") = 0x54494D45;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    if (a0 != 0) {                  /* SBI_ERR: try legacy */
        register long b0 __asm__("a0") = (long)t;
        register long b7 __asm__("a7") = 0;
        __asm__ volatile("ecall" : "+r"(b0) : "r"(b7) : "memory", "a1");
    }
}

#endif
