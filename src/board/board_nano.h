#ifndef BOARD_NANO_H
#define BOARD_NANO_H
#define BOARD_NAME          "nano"
#define BOARD_UART_BASE     0x04140000UL   /* UART0, DW 16550-compatible */
#define BOARD_UART_SHIFT    2              /* 4-byte register stride */
/* UNVERIFIED: 25 MHz timebase (SG2002 oscillator); confirm on hardware. */
#define BOARD_TIMEBASE_HZ   25000000UL
#endif
