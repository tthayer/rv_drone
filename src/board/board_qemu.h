#ifndef BOARD_QEMU_H
#define BOARD_QEMU_H
#define BOARD_NAME          "qemu"
#define BOARD_UART_BASE     0x10000000UL   /* virt NS16550A */
#define BOARD_UART_SHIFT    0              /* 1-byte stride */
#define BOARD_TIMEBASE_HZ   10000000UL     /* virt timebase-frequency */
#define BOARD_PLIC_BASE     0x0c000000UL   /* virt PLIC; hart0 S = ctx 1 */
#define BOARD_PLIC_SCTX     1
#define BOARD_UART_IRQ      10
#define BOARD_UART2_IRQ_OR_0 0
#endif
