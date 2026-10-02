#ifndef BOARD_NANO_H
#define BOARD_NANO_H
#define BOARD_NAME          "nano"
#define BOARD_UART_BASE     0x04140000UL   /* UART0, DW 16550-compatible */
#define BOARD_UART_SHIFT    2              /* 4-byte register stride */
/* UNVERIFIED: 25 MHz timebase (SG2002 oscillator); confirm on hardware. */
#define BOARD_TIMEBASE_HZ   25000000UL
/* LED1 on pad SD0_PWR_EN (GPIOA14): FMUX 0x03001038, func 3 = XGPIOA_14. */
#define BOARD_LED_FMUX      0x03001038UL
#define BOARD_LED_FMUX_GPIO 3
#define BOARD_LED_GPIO_BASE 0x03020000UL   /* GPIO0 (DW APB): DR 0x00, DDR 0x04 */
#define BOARD_LED_BIT       14
#endif
