# rv_drone

A bare-metal drone synthesizer on three boards:

| Board | Role |
|---|---|
| Sipeed LicheeRV Nano (SG2002, C906 RV64GC) | DSP engine and UI state. Runs in S-mode under OpenSBI. |
| Pico 2 W "A" (RP2350) | Audio clock master; PIO I2S to a PCM5102A. Linked to the Nano by SPI. |
| Pico 2 W "B" (RP2350) | Front panel: 6 encoders, 3 SSD1306 OLEDs, MIDI in. Linked to the Nano by UART. |

The design, pin maps, link protocols and milestones are in
`docs/ARCHITECTURE.md`.

**Current state:** M0 is done. The Nano boots from SD and blinks LED1, with
a UART0 hello and heartbeat. The Pico firmware (`firmware/`) is not
started yet.

## Toolchain

    brew install riscv64-elf-gcc qemu dtc

The Pico SDK toolchain (arm-none-eabi-gcc, cmake, picotool) gets added at
M2.

## Build (Nano)

    make BOARD=nano     # the default; outputs build/nano/rv_drone.{elf,bin,map,lst}
    make BOARD=qemu

## Run on QEMU

    make run-qemu       # Ctrl-A X to quit

## Boot on the Nano

You need these vendor files, which are not in this repo:

- a sophgo/fiptool clone;
- its `data/fsbl/cv181x.bin`, `data/ddr_param.bin` and `data/cvirtos.bin`
  (the SG2002 versions);
- an OpenSBI `fw_dynamic.bin`.

`make opensbi` builds the OpenSBI file for you:

- It clones mainline OpenSBI v1.8.1 into `third_party/`.
- It embeds `boards/licheervnano/sg2002-licheervnano.dts`.
- The output is `build/opensbi/fw_dynamic.bin`.

Then set the paths and build `fip.bin`:

    export FIPTOOL=~/src/fiptool
    export FSBL_BIN=$FIPTOOL/data/fsbl/cv181x.bin
    export DDR_PARAM_BIN=$FIPTOOL/data/ddr_param.bin
    export RTOS_BIN=$FIPTOOL/data/cvirtos.bin
    make fip            # builds OpenSBI if needed -> build/nano/fip.bin
    # optional: export OPENSBI_BIN=/path/to/fw_dynamic.bin to use your own

To boot:

1. Copy `fip.bin` to the root of the SD card's first FAT32 partition (MBR
   partition table).
2. Insert the card and power the Nano over USB-C.
3. LED1 toggles every 500 ms.
4. UART0 (A16 TX / A17 RX, 115200 8N1) prints the boot log. A 5 V FTDI
   adapter needs a 1 kΩ / 1.8 kΩ divider on its TX line.

If the Nano finds nothing bootable on the card, it shows up on USB as
CVITEK "USB Com Port" (3346:1000). That is the SG2002 mask ROM's download
mode.
