# rv_drone

A bare-metal drone synthesizer on three boards:

| Board | Role |
|---|---|
| Sipeed LicheeRV Nano (SG2002, C906 RV64GC) | DSP engine and UI state. Runs in S-mode under OpenSBI. |
| Pico 2 W "A" (RP2350) | Audio clock master; PIO I2S to a PCM5102A. Linked to the Nano by SPI2. |
| Pico 2 W "B" (RP2350) | Front panel: 6 encoders, 3 SSD1306 OLEDs, MIDI in. Linked to the Nano by UART. |

The design, pin maps, link protocols and milestones are in
`docs/ARCHITECTURE.md`; the drone engine (sound, parameters, MIDI/clock,
CPU budget) is documented in `docs/ENGINE.md`.

**Current state (2026-10-07):** M0–M8 are done on hardware (M8: presets on
SD survive a power cycle).

- **Nano:** renders the drone engine (16 voices × 3–16 detuned or stacked oscillators,
  filter, chorus, delay, FDN reverb) at about 33 % CPU worst case, streams it
  to Pico A over SPI2 DMA, owns the UI on all three OLEDs, follows MIDI clock,
  and saves/loads presets on the SD card.
- **Pico A:** plays the Nano's audio from an 8-block ring (primed to 3 blocks,
  frames checked by the DMA sniffer's hardware CRC) through the PCM5102A.
- **Pico B:** forwards encoders, switches, MIDI and MIDI clock; draws the
  Nano's display pages.
- **Mac:** the same engine and UI run in an SDL2 emulator (`make emu`).

Next: M9 (single 5 V supply, enclosure). Wiring diagrams are
in `docs/wiring/` (`system.svg`, `panel.svg`).

## Toolchain

    brew install riscv64-elf-gcc qemu dtc
    brew install sdl2 graphviz                     # emulator; wiring diagrams
    python3 -m venv .venv && .venv/bin/pip install pyserial wireviz numpy

See "Pico firmware" below for the Pico toolchain, and `tools/get-vendor.sh`
for the vendor files (fiptool, FSBL, USB-boot magic, FatFs).

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
prints the sysclk (153.6 MHz), the PIO divider (25) and, once a second, the
block count (expect 750/s), late refills, the audio ring (fill, playing or
priming, underruns, overflows, catch-up requests) and the rvlink receive
counters. `link: hwcrc` means the frames are checked by the DMA sniffer's
hardware CRC (boot self-test passed); `hw/sw-mismatch` should stay 0. Pico A plays only what the Nano sends: silence until the ring has
primed.

PCM5102A wiring for M2 (Pico A):

| Pico A | PCM5102A |
|---|---|
| GP10 (pin 14) | BCK |
| GP11 (pin 15) | LCK / LRCK |
| GP12 (pin 16) | DIN |
| 3V3(OUT) (pin 36) | VIN (development; single-supply build: 5 V via an RC filter, see `docs/wiring/system.svg`) |
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

    make pico-flash-panel PICO_B_SER=53ADB4FD5CB7055B   # this bench's Pico B
    make pico-flash-audio PICO_A_SER=0608CFFAD05BCF10   # this bench's Pico A

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

M5Stack Unit MIDI (Grove cable): white to GP21 (pin 27), red to VBUS (pin 40)
(VSYS, pin 39, in the single-supply build), black to GND, mode switch on
**Bypass**. See the architecture doc for the
reasoning.

Pico B uses both cores. Core 1 writes the displays at 1 MHz I2C, falling
back to 400 kHz if a display doesn't ACK at 1 MHz, and sends only each page's
changed columns. Core 0 handles inputs and the Nano link. The banner shows the
I2C speed, and an `oled: bytes …` line once a second shows the data written per
display.

The console (USB CDC) prints a banner (re-printed when a terminal connects),
then `enc N delta D total T`, `sw N down|up` and `midi note_on ch C note N vel
V` (also `note_off`, `cc`, `pitch_bend`, `program`; channels print as 1 to 16).
Encoder totals are raw quadrature counts, usually 4 per detent; swap an
encoder's A/B wires if it counts backwards. Each OLED shows its two encoders
(total, position bar, last delta; a half inverts while its switch is held),
with a MIDI activity box in the header. That local UI (`render.c`) is only a
fallback now: once the Nano sends display pages (M6) Pico B shows those, and it
returns to the local UI if the Nano is silent for 2.5 s. Pico B forwards
encoder, switch, MIDI and MIDI-clock events to the Nano.

Host tests of the MIDI parser, switch debounce and the OLED renderer (which
also writes the three layouts as PBM images to `build/panel-test/`):

    make test-panel

## Emulator (Mac)

The engine (`src/engine`) and UI (`src/ui`) have no hardware dependencies, so
the same code runs on the Mac with SDL2 for audio and a window showing the
three OLEDs.

    make emu
    build/emu/rv_drone_emu

| Control | Action |
|---|---|
| Mouse wheel over a display half | turn that encoder |
| Click a display half | press that encoder's switch (enc 1 = next page; others reset the parameter, or load/save on the PRESET page) |
| `z s x d c v g b h n j m ,` | play notes C..C (piano layout); ↑ / ↓ = octave |
| `k` | toggle a 120 BPM test MIDI clock (for the CLOCK page) |
| space | all notes off |
| Esc | quit |

Presets are saved to `build/emu/presets/` in the same format as the SD card.

Offline render, for checking the sound without listening:

    make emu-wav                                  # 20 s default drone -> build/emu/drone.wav
    build/emu/rv_drone_emu --wav out.wav --seconds 30 --notes 38,45,50 \
        --set CUTOFF=400 --set "LFO DIV=5" --clock 120

It prints peak/RMS/DC per channel, a NaN check and the host render time.
`--set` takes a parameter name (as shown on the displays) and a value in its
unit; `--clock BPM` sends MIDI clock with a Start at t = 0.

## Host tests

    make test-link        # rvlink validator + Pico A audio ring
    make test-panel       # MIDI parser, debounce, OLED rendering, rvpanel COBS/CRC
    make test-presets     # preset save/load round trip through the UI
    make test-engine      # exp2 accuracy, envelope timing, voice-steal fade

## Build (Nano)

    make BOARD=nano     # the default; outputs build/nano/rv_drone.{elf,bin,map,lst}
    make C906_OPT=0     # generic rv64gc instead of the C906-tuned build (A/B timing)
    make CPU_MHZ=0      # leave the C906 clock as the FSBL set it (default 1000: MPLL/1, verified at boot)
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

## SD card and presets

Use a FAT32 microSD **without `fip.bin`** so the ROM still falls through to USB
boot. The card can be formatted by the Nano itself (console `F` twice), which
erases it. Presets live in `/presets/P01.TXT`..`P16.TXT` (one `NAME=position`
line per parameter, position 0..10000) and `/presets/LAST.TXT` names the slot
loaded at boot. On the panel: press encoder 1 until the header reads
`PRESET`, turn encoder 1 to pick a slot, push encoder 2 to load, push
encoder 3 to save. The right display previews the selected slot's stored
settings, so you can tell slots apart before loading:
- cutoff and resonance;
- shape and detune;
- oscillator count and sub;
- delay time (or its sync division) and feedback;
- reverb mix and size;
- attack and release.

It shows `EMPTY` or `NO CARD` when there's nothing to show. Insert or remove the card only with the power off.

## Nano console commands

UART0, 115200 8N1. Once a second the Nano prints link, panel, engine and DMA
statistics, plus a `prof` line with the render time per stage (oscillators,
voice, chorus, delay, reverb), and an `out: peak L … R …` line with the largest
sample sent to Pico A on each channel. Both are 0 when silent, which separates
data problems from electrical noise. At boot it reports the vector unit and the
result of the kernel self-test (`simd: ...`), the C906 clock (`cpuclk: ...`)
and the console's real baud rate and UART clock source (`uart: ...`).

| Key | Action |
|---|---|
| Ctrl-R | reset (used by `make usbboot`) |
| `d` | toggle SPI DMA ↔ polled for the audio link |
| `t` | force one link transfer (scope trigger) |
| `p` | toggle the M4 test pattern ↔ engine audio |
| `w` | worst-case CPU load: OSCS at maximum (16), sixteen voices latched (loud: `a` stops it) |
| `a` | all notes off |
| `v` | engine kernels RVV ↔ scalar (A/B timing; RVV only if the boot self-test passed; scalar caps OSCS at 7 to stay real-time) |
| `x` | rerun the scalar-vs-RVV kernel self-test |
| `P` | print every engine parameter's current value (six per line) |
| `c` | measure the C906 clock (rdcycle vs the 25 MHz timer) and print the clock registers |
| `i` | SD card and volume info |
| `F` `F` (within 3 s) | format the SD card (erases it) |
| `S` / `L` | save / load preset slot 1 |

## Repo hooks

`git config core.hooksPath .githooks` enables the pre-commit hook, which
re-renders the WireViz diagrams (`docs/wiring/`) whenever their YAML is committed.
It needs `.venv/bin/pip install wireviz` and `brew install graphviz`.
