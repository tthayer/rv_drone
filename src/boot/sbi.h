/* Minimal SBI helpers (OpenSBI runs in M-mode below us). */
#ifndef SBI_H
#define SBI_H

/* Legacy console putchar (extension 0x01, function id ignored). Debug fallback
 * for when the UART driver is suspect. Deprecated in SBI spec but OpenSBI
 * still implements it. */
static inline void sbi_console_putchar(int ch)
{
    register long a0 __asm__("a0") = ch;
    register long a7 __asm__("a7") = 0x01;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory", "a1");
}

#endif
