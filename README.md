# rv_drone

A bare-metal drone synthesizer on three boards:

| Board | Role |
|---|---|
| Sipeed LicheeRV Nano (SG2002, C906 RV64GC) | DSP engine and UI state. Runs in S-mode under OpenSBI. |
| Pico 2 W "A" (RP2350) | Audio clock master; PIO I2S to a PCM5102A. Linked to the Nano by SPI2. |
| Pico 2 W "B" (RP2350) | Front panel: 6 encoders, 3 SSD1306 OLEDs, MIDI in. Linked to the Nano by UART. |

The design, pin maps, link protocols and milestones are in
`docs/ARCHITECTURE.md`.

**Current state:** M0–M3 are done. The Nano boots over USB, Pico A plays a 440 Hz tone over I2S, and the Pico B panel (encoders, switches, OLEDs, MIDI) works. The Nano boots from SD and runs a 1 kHz timer
interrupt and an interrupt-driven UART0, and `make usbboot` boots it over USB. The Pico firmware is in `firmware/`; M2 (Pico A audio) is verified on hardware; M3 (Pico B panel) is written but not yet run on hardware.

## Toolchain

    brew install riscv64-elf-gcc qemu dtc

See "Pico firmware" below for the Pico toolchain.

## Pico firmware

Toolchain (needs arm-none-eabi-gcc **with newlib**):

    brew install cmake ninja picotool
    brew install --cask gcc-arm-embedded      # needs sudo for the pkg installer

The Homebrew `arm-none-eabi-gcc` formula has no newlib. If you can't use the
cask, expand the Arm GNU Toolchain pkg into `~/opt/arm-gnu-toolchain-*`
(`pkgutil --expand-full <pkg> dir`, move `dir/Payload` there); `make pico`
picks that up through `PICO_TOOLCHAIN_PATH` (a bin dir; override as needed).

Get the SDK (pinned 2.3.1, with submodules, into `third_party/pico-sdk`):

    tools/get-pico-sdk.sh

Build (`make pico` runs the cmake and ninja steps; output in `build/firmware/`):

    make pico           # -> build/firmware/audio/audio.{elf,uf2} and build/firmware/panel/panel.{elf,uf2}

Flash Pico A: either `make pico-flash-audio` (runs
`picotool load -fx build/firmware/audio/audio.uf2`; it reboots a running Pico
into BOOTSEL through the USB reset interface), or hold BOOTSEL while plugging
it in and copy `audio.uf2` to the RPI-RP2 drive. The console is USB CDC: it
prints the sysclk (153.6 MHz), the PIO divider (25) and, once a second, the DMA
block count (expect 750/s) and the late-refill count.

PCM5102A wiring for M2 (Pico A):

| Pico A | PCM5102A |
|---|---|
| GP10 (pin 14) | BCK |
| GP11 (pin 15) | LCK / LRCK |
| GP12 (pin 16) | DIN |
| 3V3(OUT) (pin 36) | VIN |
| GND | GND |

Check your module, as boards differ. On the common purple modules:

- SCK must be tied to GND (no MCLK is supplied); often a solder bridge.
- FMT low = I2S.
- XSMT high = unmuted; often a solder bridge or jumper to 3.3 V.
- FLT low and DEMP low.

### Pico B (panel), M3

`make pico-flash-panel` loads `build/firmware/panel/panel.uf2` the same way.
With both Picos on USB, pick one by its USB serial number: `PICO_A_SER` and
`PICO_B_SER` make variables, which add `--ser <serial>` to `picotool load`:

    make pico-flash-panel PICO_B_SER=E66...

picotool 2.3.1 has no `list` command. `picotool info -a` shows what it finds
(without arguments it lists BOOTSEL devices only; add `-f` to include running
ones, which reboots them), and `system_profiler SPUSBDataType` shows each
board's serial number on macOS. Which `picotool info` field prints the
serial is unchecked.

Wiring is in `docs/ARCHITECTURE.md` ("Pico B (panel)" pin table): 6 encoders
(A/B on adjacent GPIOs), 6 switches, I2C0 on GP4/GP5 (OLED 0 at 0x3C, OLED 1
at 0x3D), I2C1 on GP26/GP27 (OLED 2 at 0x3C), MIDI in on GP21. Nothing is
required to be present: encoders and switches idle on pull-ups, and the boot
banner lists which I2C addresses answered ("I2C0: 0x3C" etc.); only displays
that ACK are driven.

**OLED 1 must be at 0x3D.** On most SSD1306 modules, move the 0 Ω address
resistor on the back from the 0x78 pad to the 0x7A pad. Until then it shares
0x3C with OLED 0, so the firmware reports `I2C0: 0x3C` and `OLED1 ... not
found`, and drives OLED 0 only.

M5Stack Unit MIDI (Grove cable): white to GP21 (pin 27), red to VBUS (pin 40),
black to GND, mode switch on **Bypass**. See the architecture doc for the
reasoning and what is still unverified about the 3.5 mm input.

The console (USB CDC) prints a banner (re-printed when a terminal connects),
then `enc N delta D total T`, `sw N down|up` and `midi note_on ch C note N vel
V` (also `note_off`, `cc`, `pitch_bend`, `program`; channels print as 1 to 16).
Encoder totals are raw quadrature counts, usually 4 per detent; swap an
encoder's A/B wires if it counts backwards. Each OLED shows its two encoders
(total, position bar, last delta; a half inverts while its switch is held),
with a MIDI activity box in the header. The display code is `render.c`; M6
replaces it with pages from the Nano.

Host tests of the MIDI parser, switch debounce and the OLED renderer (which
also writes the three layouts as PBM images to `build/panel-test/`):

    make test-panel

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

## Dev loop: USB boot (no SD card)

With no bootable SD card in it, the Nano's mask ROM waits in USB download
mode (CVITEK "USB Com Port", 3346:1000). `tools/usbboot.py` boots
`fip.bin` straight into RAM. It writes nothing to flash or SD.

One-time setup:

    python3 -m venv .venv && .venv/bin/pip install pyserial
    export USB_DL_MAGIC=~/src/duo-buildroot-sdk/build/tools/cv181x/usb_dl/rom_usb_dl/cv_dl_magic.bin
    export RESET_PORT=/dev/cu.usbserial-XXXX     # UART0 adapter (optional)

To boot each build:

    make usbboot

1. With RESET_PORT set, Ctrl-R is sent to the running image, which
   warm-resets the chip into USB download mode. Otherwise, power-cycle the
   board yourself.
2. The image is pushed over USB and runs. Watch UART0 for its output.

The connection needs USB-C from the Nano to the Mac (data plus power) and
no SD card, or one without `fip.bin`. A boot takes about 20–40 s.
