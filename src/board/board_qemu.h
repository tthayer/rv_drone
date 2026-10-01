#ifndef BOARD_QEMU_H
#define BOARD_QEMU_H
#define BOARD_NAME          "qemu"
#define BOARD_UART_BASE     0x10000000UL   /* virt NS16550A */
#define BOARD_UART_SHIFT    0              /* 1-byte stride */
#define BOARD_TIMEBASE_HZ   10000000UL     /* virt timebase-frequency */
#endif
