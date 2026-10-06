#ifndef BOARD_NANO_H
#define BOARD_NANO_H
#define BOARD_NAME          "nano"
#define BOARD_UART_BASE     0x04140000UL   /* UART0, DW 16550-compatible */
#define BOARD_UART_SHIFT    2              /* 4-byte register stride */
#define BOARD_UART_DW       1              /* DesignWare: USR at reg 31 */
/* UNVERIFIED: 25 MHz timebase (SG2002 oscillator); confirm on hardware. */
#define BOARD_TIMEBASE_HZ   25000000UL
/* UNVERIFIED: PLIC base from vendor DTS ("thead,c900-plic"); S-mode access is
 * gated by a T-Head control bit that OpenSBI sets. S-ctx of hart 0 = 1. */
#define BOARD_PLIC_BASE     0x70000000UL
#define BOARD_PLIC_SCTX     1
/* UNVERIFIED: UART0 PLIC source 44 (SG2002 DTS). */
#define BOARD_UART_IRQ      44
/* LED1 on pad SD0_PWR_EN (GPIOA14): FMUX 0x03001038, func 3 = XGPIOA_14. */
#define BOARD_LED_FMUX      0x03001038UL
#define BOARD_LED_FMUX_GPIO 3
#define BOARD_LED_GPIO_BASE 0x03020000UL   /* GPIO0 (DW APB): DR 0x00, DDR 0x04 */
#define BOARD_LED_BIT       14
/* M4: SPI2 audio link (Pico A). FMUX/clock/reset bases per TRM + SDK clk-cv181x.c. */
#define BOARD_HAS_SPI_LINK  1
#define BOARD_FMUX_BASE     0x03001000UL
#define BOARD_CLKGEN_BASE   0x03002000UL   /* CLK_EN_1 +0x04, CLK_EN_3 +0x0C, BYP_0 +0x30, DIV_SPI +0x100, FPLL_CSR +0x910 */
#define BOARD_RSTGEN_BASE   0x03003000UL   /* SOFT_RSTN_1 +0x04: bit 10 = SPI2 (active low) */
#define BOARD_SPI_BASE      0x041A0000UL   /* SPI2: snps,dw-apb-ssi */
#define BOARD_SPI_TARGET_HZ 8000000UL
#define BOARD_GPIO_BASE     0x03020000UL   /* GPIO0 = GPIOA */
/* UNVERIFIED on hw: GPIO0 PLIC src 60 (TRM C906 map: A53 76 - 16, same offset as UART0 60->44). */
#define BOARD_GPIO_IRQ      60
#define BOARD_DRQ_GPIO_BIT  27             /* A27 = EMMC_DAT3, FMUX 0x58 func 3 */
#define BOARD_FMUX_DRQ      0x58           /* func 3 = XGPIOA_27 */
#define BOARD_FMUX_DRQ_FN   3
/* SPI2 on the P pads, all func 1: CS P18, MISO P21, MOSI P22, SCK P23 */
#define BOARD_FMUX_SPI_CS   0xD0
/* CS is driven as GPIO (func 3 = PWR_GPIO_18, RTC GPIO bank gpio4 at 0x05021000)
 * because the controller CS blips whenever the TX FIFO runs empty. */
#define BOARD_SPI_CS_FN     3
#define BOARD_SPI_CS_GPIO   0x05021000UL
#define BOARD_SPI_CS_BIT    18
#define BOARD_FMUX_SPI_MISO 0xDC
#define BOARD_FMUX_SPI_MOSI 0xE0
#define BOARD_FMUX_SPI_SCK  0xE4
#endif
