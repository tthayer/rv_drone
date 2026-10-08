#ifndef BOARD_NANO_H
#define BOARD_NANO_H
#define BOARD_NAME          "nano"
#define BOARD_UART_BASE     0x04140000UL   /* UART0, DW 16550-compatible */
#define BOARD_UART_SHIFT    2              /* 4-byte register stride */
#define BOARD_UART_DW       1              /* DesignWare: USR at reg 31 */
/* Timebase 25 MHz: measured (time +25,000,000/s). The TRM documents the 25 MHz
 * xtal but not the C906 time CSR's clock. */
#define BOARD_TIMEBASE_HZ   25000000UL
/* PLIC at 0x70000000: TRM memory map (PLIC, 64 MiB). Register layout and the
 * S-mode access gate (a T-Head control bit OpenSBI sets) are standard RISC-V /
 * T-Head C900, not in the TRM. S-ctx of hart 0 = 1. */
#define BOARD_PLIC_BASE     0x70000000UL
#define BOARD_PLIC_SCTX     1
/* UART0 = PLIC source 44: TRM interrupt table, "Master RISCV C906" (the A53 and
 * little-C906 numbers differ). Verified on hardware. */
#define BOARD_UART_IRQ      44
/* LED1 on pad SD0_PWR_EN (GPIOA14): FMUX 0x03001038, func 3 = XGPIOA_14. */
#define BOARD_LED_FMUX      0x03001038UL
#define BOARD_LED_FMUX_GPIO 3
#define BOARD_LED_GPIO_BASE 0x03020000UL   /* GPIO0 (DW APB): DR 0x00, DDR 0x04 */
#define BOARD_LED_BIT       14
/* M4: SPI2 audio link (Pico A). FMUX/CLKGEN/RSTGEN bases and the clock/reset bits
 * are in the TRM (memory map, clock and reset chapters). Pad FMUX offsets and
 * function numbers are not: the TRM defers them to SG2002_PINOUT.xlsx, so they
 * come from the vendor SDK pinlist headers. All verified on hardware. */
#define BOARD_HAS_SPI_LINK  1
#define BOARD_FMUX_BASE     0x03001000UL
#define BOARD_CLKGEN_BASE   0x03002000UL   /* CLK_EN_1 +0x04, CLK_EN_3 +0x0C, BYP_0 +0x30, DIV_SPI +0x100, FPLL_CSR +0x910 */
#define BOARD_RSTGEN_BASE   0x03003000UL   /* SOFT_RSTN_1 +0x04: bit 10 = SPI2 (active low) */
#define BOARD_SPI_BASE      0x041A0000UL   /* SPI2: snps,dw-apb-ssi */
#define BOARD_SPI_TARGET_HZ 8000000UL
#define BOARD_GPIO_BASE     0x03020000UL   /* GPIO0 = GPIOA */
/* GPIO0 = PLIC source 60: TRM interrupt table, Master RISCV C906. Verified on hardware (DRQ). */
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
/* M5: sysDMA (DW_axi_dmac, 8 ch) for SPI2. SDK cv181x_base_riscv.dtsi:141 (IRQ 29), TRM
 * system-control (remap 0x154, int_mux 0x298), dma-mapping.h (CVI_SPI2_RX 20, _TX 21).
 * IRQ 29, remap, int_mux bits and request numbers 20/21 are all in the TRM
 * (interrupt table, system-control registers, system_dma_channel_mapping) and
 * verified on hardware. BOARD_SPI_DMA 0 (-DBOARD_SPI_DMA=0) keeps the polled path only. */
#define BOARD_HAS_DMA       1
#define BOARD_SYSCTL_BASE   0x03000000UL
#define BOARD_DMAC_BASE     0x04330000UL
#define BOARD_DMAC_IRQ      29
#define BOARD_SPI_DMA_RX_REQ 20
#define BOARD_SPI_DMA_TX_REQ 21
#ifndef BOARD_SPI_DMA
#define BOARD_SPI_DMA       1
#endif
/* M6: UART2 panel link (Pico B); base, PLIC 46, clock gates and reset bit are in
 * the TRM (pads' FMUX values are SDK-only). SDK cv181x_base.dtsi serial@04160000 (25 MHz,
 * reg-shift 2), cv181x_base_riscv.dtsi:190 (PLIC 46), clk-cv181x.c (CLK_EN_1 bit 18
 * clk_uart2, bit 19 clk_apb_uart2), cv181x-resets.h (RST_UART2 = SOFT_RSTN_0 bit 25).
 * Pads: A28 = IIC0_SCL (FMUX 0x70) func 2 UART2_TX, A29 = IIC0_SDA (0x74) func 2 RX. */
#define BOARD_HAS_PANEL_LINK 1
#define BOARD_UART2_BASE    0x04160000UL
#define BOARD_UART2_IRQ     46
#define BOARD_UART2_IRQ_OR_0 BOARD_UART2_IRQ
#define BOARD_UART2_CLK_HZ  25000000UL
#define BOARD_FMUX_UART2_TX 0x70
#define BOARD_FMUX_UART2_RX 0x74
#define BOARD_FMUX_UART2_FN 2
/* M8: SD0 slot (microSD), DWC MSHC = standard SDHCI + vendor regs at +0x200 (TRM
 * sdmmc chapter; base in the TRM memory map). BOARD_SD_BASE_HZ is the SDK DTS
 * src-frequency (375 MHz); the TRM preset for clk_sd0 is 100 MHz, so sd.c decodes
 * the real value and never divides from less than this. Pads/pulls come from the
 * vendor sdhci-cv181x.c (not in the TRM); PWRSW and CLK_EN_0 bits 18-20 are in it. */
#define BOARD_HAS_SD        1
#define BOARD_SD_BASE       0x04310000UL
#define BOARD_SD_BASE_HZ    375000000UL
#endif
