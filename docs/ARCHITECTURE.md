# rv_drone — Architecture

A bare-metal drone synthesizer on three boards:

- **Sipeed LicheeRV Nano (SG2002)**: the DSP engine, and the owner of all
  synth and UI state. No Linux; our binary owns the big C906 core after the
  vendor boot chain brings up DDR.
- **Pico A, a Raspberry Pi Pico 2 W (RP2350)**: audio only. It is the
  audio clock master and drives PIO I2S to the PCM5102A.
- **Pico B, a Raspberry Pi Pico 2 W**: the front panel. It handles the
  encoders, switches, OLEDs and MIDI in.

The Nano header has no usable I2S (see "Rejected audio paths"), which is
why audio goes through a Pico. The panel is on its own Pico so that UI
work can never disturb audio timing.

## Hardware

```
                          SPI2 + DRQ (audio)
   LicheeRV Nano  ◀──────────────────────────────▶  Pico A (audio)
   DSP + UI state                                    PIO I2S ─▶ PCM5102A
   UART0 ─▶ FTDI (debug)
        ▲
        │ UART2, 1.5625 Mbaud (panel link)
        ▼
   Pico B (panel)
     6 encoders: A/B on PIO, switches on GPIO
     I2C0: OLED 0 (0x3C) + OLED 1 (0x3D)    I2C1: OLED 2 (0x3C)
     UART1 RX ◀─ MIDI opto
   ───────────────────── common GND across all four boards and the FTDI ─────
```

| Function | Part | Where | Interface |
|---|---|---|---|
| DSP | LicheeRV Nano B | Nano | C906 @ 1 GHz, RV64GC + RVV 0.7.1, 256 MB DDR |
| Audio link | — | Nano ⇄ Pico A | SPI2, Nano master, mode 3, 8 MHz to start; DRQ from Pico A |
| Audio out | PCM5102A | Pico A | I2S (PIO), Pico master, 64 fs, no MCLK (SCK tied low) |
| Panel link | — | Nano ⇄ Pico B | UART, 1.5625 Mbaud 8N1, COBS-framed packets |
| Displays | 3 × SSD1306 128x64 (as in pseudopod) | Pico B | I2C0: 0x3C + 0x3D; I2C1: 0x3C |
| Controls | 6 rotary encoders with push | Pico B | PIO quadrature + GPIO |
| MIDI in | M5Stack Unit MIDI (opto-isolated 3.5 mm TRS + DIN-5 in) | Pico B | UART1 RX, 31250 8N1 |
| Storage | 32 GB microSD, FAT32 | Nano | SD0 (on-board slot) |
| Debug | FTDI adapter (5 V; divider on its TX) / USB CDC | Nano UART0 / Picos USB | 115200 |

⚠ **OLED 1 must be moved to 0x3D.** On most SSD1306 modules that means
moving the 0 Ω "IIC ADDRESS SELECT" resistor on the back from the 0x78
position to the 0x7A position. That is SMD work. Bridging the 0x7A pads
with a solder blob, after lifting the original resistor, is often enough.
If it isn't practical, OLED 1 goes on a third bus run by PIO-based I2C
using the spare GP9 plus one switch pin. That switch then moves to an
ADC-ladder input.

### Nano pins

Header naming: L1–L14 is the left column and R1–R14 the right, top to
bottom (schematic sheet 1/4), with USB-C at the bottom (the L14/R14 end;
L13/L14 are 5 V). On a breadboard: L sits in column c and R in column i,
leaving a/b (left) and j (right) free. Every header GPIO is 3.3 V.

| Pin | Pad | Use |
|---|---|---|
| L1 | A17 | UART0 RX ← FTDI TX via 1 kΩ / 1.8 kΩ divider |
| L2 | A16 | UART0 TX → FTDI RX |
| L3 | GND | common GND |
| L9 | A27 | DRQ (GPIO in) ← Pico A GP20 |
| R4 | A29 | UART2 RX ← Pico B GP16 |
| R6 | A28 | UART2 TX → Pico B GP17 |
| R7 | P18 | SPI2 CS → Pico A GP17 |
| R9 | P21 | SPI2 MISO (SDI) ← Pico A GP19 |
| R10 | P22 | SPI2 MOSI (SDO) → Pico A GP16 |
| R11 | P23 | SPI2 SCK → Pico A GP18 |
| R13 | A14 | onboard LED1 (status) |

- **L5/L6 are the speaker amp outputs (VOP/VON).** Never connect anything to
  them.
- **A28/A29 double as JTAG TDI/TDO.**
**Pinmux, checked against the SDK** (`cv181x_pinlist_swconfig.h`,
`cv181x_reg_fmux_gpio.h`). FMUX base is 0x03001000:

| Pad (pin) | FMUX offset | Function | Value |
|---|---|---|---|
| SD1_D3 (P18) | 0xD0 | SPI2_CS_X | 1 |
| SD1_D0 (P21) | 0xDC | SPI2_SDI | 1 |
| SD1_CMD (P22) | 0xE0 | SPI2_SDO | 1 |
| SD1_CLK (P23) | 0xE4 | SPI2_SCK | 1 |
| EMMC_DAT3 (A27) | 0x58 | XGPIOA_27 | 3 |
| IIC0_SCL (A28) | 0x70 | UART2_TX | 2 |
| IIC0_SDA (A29) | 0x74 | UART2_RX | 2 |
| SD0_PWR_EN (A14) | 0x38 | XGPIOA_14 (LED) | 3 |

- **Peripherals:**
  - SPI2 is `snps,dw-apb-ssi` at 0x041A0000, with clock `CV181X_CLK_SPI`.
  - UART2 is `snps,dw-apb-uart` at 0x04160000, with a 25 MHz clock and
    reg-shift 2.
  - Both come from the SDK `cv181x_base.dtsi`.
  - **SPI2 clock (M4 research):** `clk_spi` = FPLL 1500 MHz / 8 = **187.5 MHz**
    (TRM `clock/clksource_preset_freq_div_param.table.rst:272`, `spi.rst`
    "Clock"; FSBL `fsbl/plat/cv181x/platform.c:223`). SCK = 187.5 MHz / BAUDR
    (even), so 8 MHz is not exact: BAUDR 24 gives 7.8125 MHz.
  - **Gate and reset bits:** clk_apb_spi2 = CLKGEN (0x03002000) `CLK_EN_1`
    (+0x04) bit 11; clk_spi = `CLK_EN_3` (+0x0C) bit 6 (`clk-cv181x.c:749-757,
    1280-1288`); both reset to 1. Clock bypass-to-xtal is `CLK_BYP_0` (+0x30)
    bit 30; the FSBL clears it (`platform.c:254-258`). Divider reg +0x100: bit 3
    = use reg factor, else 8. SPI2 soft reset = `SOFT_RSTN_1` (0x03003004) bit
    10, active low (TRM `reset_registers_describe.table.rst:132`).
  - **DW SSI layout:** compatible `snps,dw-apb-ssi` (not DWC_ssi), so CTRLR0 is
    DFS[3:0], FRF[5:4], SCPH[6], SCPOL[7], TMOD[9:8] (SDK `spi-dw-core.c:270-322`,
    `spi-dw.h:43-60`). Mode 3 8-bit TR = 0xC7. BAUDR must be even.
  - **GPIO0 IRQ:** TRM C906 map has GPIO0 = A53 76 - 16 = PLIC 60 (same offset as
    UART0, verified 44). Still unverified on hardware.
  - **No SD1 pad-power enable found** in the SDK or TRM; VDDIO_SD1 is a board
    supply (measure P22).
- **No SPI on A22–A25.** Those pads are the eMMC pads, and their only SPI
  functions are SPINOR/SPINAND, the flash controllers. The only SPI
  exposed on the header is SPI2, on the P pads.
- **The P pads are in the SD1 I/O domain.** Measure P22 to confirm 3.3 V
  before connecting Pico A.
- **Freed:** A22–A26 are free again, as GPIO only.

### Pico pins (both are Pico 2 W)

GP23/24/25/29 belong to the CYW43 wireless chip, and the onboard LED is on
the CYW43, so none of those are available on either board.

**Pico A (audio):**

| GP | Pico pin | Use |
|---|---|---|
| 10 | 14 | I2S BCK → PCM5102A |
| 11 | 15 | I2S LRCK → PCM5102A |
| 12 | 16 | I2S DIN → PCM5102A |
| 16 | 21 | SPI0 RX ← Nano MOSI (P22) |
| 17 | 22 | SPI0 CSn ← Nano CS (P18) |
| 18 | 24 | SPI0 SCK ← Nano SCK (P23) |
| 19 | 25 | SPI0 TX → Nano MISO (P21) |
| 20 | 26 | DRQ → Nano A27 |

Everything else on Pico A is spare (debug LEDs, scope triggers).

**Pico B (panel).** 25 of 26 GPIOs are used:

| GP | Pico pin | Use | GP | Pico pin | Use |
|---|---|---|---|---|---|
| 0 | 1 | Enc 1 A | 14 | 19 | Enc 6 A |
| 1 | 2 | Enc 1 B | 15 | 20 | Enc 6 B |
| 2 | 4 | Enc 2 A | 16 | 21 | UART0 TX → Nano A29 (UART2 RX) |
| 3 | 5 | Enc 2 B | 17 | 22 | UART0 RX ← Nano A28 (UART2 TX) |
| 4 | 6 | I2C0 SDA (OLED 0, 1) | 18 | 24 | Enc 1 SW |
| 5 | 7 | I2C0 SCL (OLED 0, 1) | 19 | 25 | Enc 2 SW |
| 6 | 9 | Enc 3 A | 20 | 26 | Enc 3 SW |
| 7 | 10 | Enc 3 B | 21 | 27 | UART1 RX ← MIDI opto |
| 8 | 11 | Enc 4 SW | 22 | 29 | Enc 5 SW |
| 9 | 12 | spare | 26 | 31 | I2C1 SDA (OLED 2) |
| 10 | 14 | Enc 4 A | 27 | 32 | I2C1 SCL (OLED 2) |
| 11 | 15 | Enc 4 B | 28 | 34 | Enc 6 SW |
| 12 | 16 | Enc 5 A | | | |
| 13 | 17 | Enc 5 B | | | |

- **Encoders:** A/B are adjacent GPIOs, as the PIO quadrature program
  needs. Switches use internal pull-ups and pull to GND. Debouncing is in
  software.
- **I2C pull-ups:** 4.7 kΩ to 3.3 V on each bus, unless the OLED modules
  already have them; most do.
- **Pico consoles:** both use USB CDC.
- **Power:** during development each board runs from its own USB, with
  every GND tied together. For the finished build, one 5 V supply feeds
  Nano L13 (VSYS) and Pico pin 39 (VSYS) on both Picos.

**MIDI input: M5Stack Unit MIDI** (https://docs.m5stack.com/en/unit/Unit_MIDI).
Its schematic is `SCH_UnitMIDI_B04`, dated 2024-07-08.

- **Input stage:** 3.5 mm (J2) and DIN-5 inputs feed a TLP2361 opto. The
  opto is powered from the unit's MCU_VDD, a 3.3 V LDO (SE8533) running
  from the Grove 5 V. Its output (UART_MIDI_IN) is a 3.3 V logic signal,
  safe for the Pico.
- **Grove HY2.0-4P wiring:**

  | Grove pin | Wire | Net | Connect to |
  |---|---|---|---|
  | 1 | black | GND | GND |
  | 2 | red | 5 V | Pico B VBUS (pin 40) |
  | 3 | yellow | UART_MIDI_OUT, host → onboard SAM2695 synth | leave unconnected |
  | 4 | white | UART_MIDI_IN, from the opto | Pico B GP21 (UART1 RX) |

- **Mode switch:** set it to **Bypass**. Per the docs, Bypass routes MIDI
  IN to the Grove TX (white). In Separate mode that pin is described as
  floating.
- **Unverified:** whether the 3.5 mm input is wired as TRS Type A (the
  MicroFreak's type). If no MIDI arrives, try a Type A↔B adapter or use
  the DIN input.
- **Bonus:** the unit's SAM2695 General MIDI synth, with its own headphone
  out, could serve as a MIDI monitor while debugging.

### Rejected audio paths (why the Picos exist)

- **No edge pin has an I2S function.** A28/A18/A19 were assumed from the
  pinlist and turned out to have none.
- **I2S2 on the Ethernet pads (mux func 7)** is the only internal route.
  But the pads pass through C49–C52 (100 nF in series), then L5/L6 and the
  U7/U8 magnetics, before reaching RJ1. The only usable tap is the SoC side
  of tiny SMD caps, which is too small to hand-solder.
- **SPI1 + a 74HC4040 divider generating LRCK** was workable. Its
  drawbacks: a non-standard rate (~48.83 kHz), the risk of gaps in SCK,
  and fewer encoders.
- **The internal codec** feeds a bridged speaker amp (L5/L6). It's mono,
  fair quality, and undocumented.
- **A single Pico for audio and panel** ran out of pins. It needed a
  TCA9548A mux plus an MCP23017, and it mixed UI work into the
  audio-timing board.

## Links

### Audio link: Nano ⇄ Pico A (`common/rvlink.h`)

**Roles:**

- Pico A is the audio clock master. It keeps a ring of 4 blocks (64 stereo
  frames each) feeding PIO I2S at exactly 48 kHz.
- It raises **DRQ** whenever a slot is free and its status reply is
  preloaded.
- The Nano answers each DRQ with one fixed-length, full-duplex SPI
  transaction.

**Electrical:**

- SPI mode 3 (CPOL=1, CPHA=1). Pico A's slave is a PIO program on pio1
  (`firmware/audio/spi_slave.pio`); the PL022 slave returned bytes one bit
  early on hardware. Per-frame re-arm discards any partial byte.
- Start at 1 MHz and raise toward ~7.8 MHz (PIO half-period budget: 64 ns
  at 7.8 MHz vs about 26 ns of loop overhead at 153.6 MHz).
- The payload needs about 3.2 Mbit/s.

**Transaction (528 bytes each way; little-endian; CRC32 last):**

| Nano → Pico A | Pico A → Nano |
|---|---|
| magic `'RVL1'`, seq u16, flags u16 | magic `'RVA1'`, seq echo u16, ring fill u8, underruns u16 |
| audio: 64 × (L,R) int32, 24-bit left-aligned (512 B) | padding |
| CRC32 | CRC32 |

- **Errors:** a CRC failure drops the block (Pico A plays silence for it)
  and bumps a counter. A sequence gap counts as an underrun.
- **Latency:** at most 4 × 64 frames ≈ 5.3 ms. The Nano always has the next
  block rendered before DRQ arrives.
- **Pico A clock:** sysclk is 153.6 MHz (48 kHz × 64 × 50), so the PIO
  divider is an integer and adds no fractional-divider jitter.

### Panel link: Nano ⇄ Pico B (`common/rvpanel.h`)

- **Framing:** UART 1.5625 Mbaud 8N1 (25 MHz / 16, an exact divisor on the
  Nano). Each packet is COBS-encoded with a 0x00 delimiter and ends with a
  CRC16. Either side may send at any time; there are no acks.
- **Nano → Pico B:**
  - `PAGE {display u8, page u8, data[128]}`: the Nano sends only dirty
    pages.
  - `CONFIG {…}`: brightness, encoder acceleration, and similar settings.
- **Pico B → Nano:**
  - `ENC {id u8, delta i8}`
  - `SW {id u8, state u8}`
  - `MIDI {len u8, bytes[3]}`: whole messages, parsed on Pico B.
  - `STATUS {crc_errors u16, …}`
- **Budget:** about 150 KB/s. A full frame on all 3 displays is about
  3.1 KB, so the link supports about 48 full refreshes per second, far
  more than the I2C side.
- **I2C refresh:** I2C0 carries two displays at 400 kHz (1 MHz if the
  modules tolerate it). Pico B writes pages as they arrive.
- **MIDI latency:** MIDI goes through Pico B and the UART. That adds about
  0.1 ms, which is negligible.

## Boot chain

```
Mask ROM ─▶ fip.bin on SD FAT partition
           ├─ FSBL (vendor cv181x.bin, closed)   DDR init, PLLs
           ├─ ddr_param.bin
           ├─ monitor: OpenSBI fw_dynamic @ 0x80000000 (M-mode)
           └─ loader_2nd: rv_drone.bin @ 0x80200000 (S-mode)   ◀── us
```

We keep the vendor FSBL and `ddr_param.bin` and put our image in the U-Boot
slot, using [sophgo/fiptool](https://github.com/sophgo/fiptool):

```
fiptool --fsbl cv181x.bin --ddr_param ddr_param.bin \
        --opensbi fw_dynamic.bin --rtos cvirtos.bin \
        --uboot build/nano/rv_drone.bin build/nano/fip.bin
```

(`tools/mkfip.sh` wraps this; fiptool requires `--rtos` even though we don't
use the little core. The vendor blobs live in the fiptool repo's `data/`.)

`fw_dynamic.bin` is built by `make opensbi` (`tools/build-opensbi.sh`): mainline
OpenSBI v1.8.1, `PLATFORM=generic`, `FW_TEXT_START=0x80000000`, with
`boards/licheervnano/sg2002-licheervnano.dts` embedded via `FW_FDT_PATH`.
**FDT handoff:** the vendor FSBL does not load a DTB; it jumps to OpenSBI with
`a1 = 0x80080000` (monitor base + 512 KB) and a `fw_dynamic_info` in `a2`
(`next_addr` = our image, `next_mode` = S). The vendor SDK builds OpenSBI with
`FW_FDT_PATH`, which makes `fw_base.S` override `a1` with the embedded DTB and
relocate it to the address the FSBL passed. We do the same, so our S-mode entry
gets `a1` = 0x80080000 pointing at that DTB (hartid in `a0`).

**Why S-mode under OpenSBI rather than replacing the monitor:**

- It's the path the vendor tooling already supports.
- OpenSBI gives us a working console, a timer (`sbi_set_timer`), and a
  sane trap setup on day one.
- Moving to M-mode later is a linker-address change plus our own trap and
  timer code. Nothing above the HAL notices.

fiptool LZMA-wraps the payload, and the FSBL accepts it. This was verified
on hardware on 2026-10-01.

## Software structure

```
src/                 Nano image (bare metal, RV64)
  boot/              start.S, linker script, trap entry, SBI calls
  board/             per-board constants (nano, qemu)
  hal/               uart, plic, timer, gpio+pinmux, spi, dma, sdhci, cache
  drivers/           audio_link (rvlink master), panel_link (rvpanel), sd → FatFs
  engine/            platform-independent DSP: oscillators, filters, mod, fx, voices
  ui/                pages, parameter model, rendering 3 × 1 KB framebuffers
  app/               main loop, wiring, preset load/save
firmware/            Pico SDK tree, one CMake project, two targets
  audio/             Pico A: audio_i2s.pio, rvlink slave (SPI0 + DMA, DRQ)
  panel/             Pico B: quadrature.pio, switches, ssd1306, midi, rvpanel
  common/            shared Pico code (COBS, CRC, USB CDC logging)
common/              rvlink.h, rvpanel.h: protocols shared by all three builds
third_party/         FatFs (ChaN), OpenSBI clone (gitignored)
emu/                 host build: engine + ui on macOS (CoreAudio, terminal/SDL OLEDs)
```

`engine/` and `ui/` have **no hardware includes**. They take parameter
changes and MIDI events and return audio blocks and framebuffers, so they
also run on the host (`emu/`).

## Runtime model

**Nano (single big core):**

| Context | Trigger | Work |
|---|---|---|
| GPIO IRQ (DRQ rising) | Pico A needs a block | Start the SPI2 TX/RX DMA, sending the already-rendered block |
| SPI DMA-complete IRQ | transfer done | Check the status reply, then render the next block (`engine_render(64)`, 1.33 ms budget) |
| UART2 RX IRQ | panel bytes | COBS decode, then queue events (encoder, switch, MIDI) for the audio context |
| Main loop | best effort | UI events → param changes → redraw the framebuffers → queue dirty pages to UART2 TX; preset I/O on SD |

- **Cache coherency:** the C906 doesn't snoop DMA. Clean the TX buffer and
  invalidate the RX buffer with T-Head CMO (`th.dcache.cva` / `th.dcache.iva`),
  or place both in an uncached region. Decided in M4.
- **Little core (C906L):** unused. The FSBL starts the vendor `cvirtos.bin`
  on it (see Unverified), and it will later be replaced by a parking loop.

**Pico A:**

- Core 0 runs the link: SPI0 DMA and DRQ, into the ring.
- PIO DMA feeds I2S from the ring.
- An underrun plays silence and is reported.

**Pico B:**

- Core 0 runs the panel link (UART0 with DMA) and the I2C display writers.
- Core 1 runs the encoder PIO FIFOs, switch debounce and the MIDI parser.

**Audio format:** 48 kHz exactly, stereo, 24-bit in 32-bit slots.
Synthesis is `float` on the Nano. RVV 0.7.1 is a later optimisation.

## Drone engine (initial sketch)

- **Voices:** 4 drone voices. Each one is a bank of 3–7 detuned oscillators
  (saw / sine / wavetable morph) with a per-partial drift LFO.
- **Per voice:** an SVF (LP/BP) with slow cutoff modulation and a gentle
  saturator.
- **Global effects:** a chorus/ensemble, a long feedback delay and an FDN
  reverb. A drone lives or dies on these.
- **MIDI:**
  - A note-on sets or adds the drone root or chord tone; a note-off is
    optional (latch mode).
  - CCs are mapped to the same parameter IDs the encoders drive.
  - Clock sync of the LFOs comes later.
- **UI:** three displays, each split down the middle, so each encoder
  owns one 64-pixel-wide half. See "UI layout" below.
- **Presets:** stored as flat binary/INI files in `/presets` on the same FAT
  partition as `fip.bin`.

### UI layout

```
 OLED 0 (I2C0 0x3C)      OLED 1 (I2C0 0x3D)      OLED 2 (I2C1 0x3C)
+----------+----------+ +----------+----------+ +----------+----------+
| OSC  1/4 |          | | FILTER   |          | | SPACE    |          |  a
| DETUNE   | DRIFT    | | CUTOFF   | RESO     | | DELAY    | REVERB   |  b
|   12.5c  |   0.30   | |  1.2 kHz |   0.65   | |  850 ms  |   72 %   |  c
| [####  ] | [##    ] | | [#####  ]| [###   ] | | [##    ] | [##### ] |  d
+----------+----------+ +----------+----------+ +----------+----------+
   Enc 1      Enc 2        Enc 3      Enc 4        Enc 5      Enc 6
```

Rows:

- **a:** 8 px header, holding the page name and page x/N.
- **b:** parameter name, 6x8 font.
- **c:** value, large font.
- **d:** bar or arc showing the knob position.

Behaviour:

- **Pages:** each page maps up to 6 parameters onto the six encoders.
- **Encoder switches:**
  - Enc 1 push cycles the page.
  - Other pushes are per page: fine adjust, reset to default, or
    latch/hold.
- **Feedback:**
  - A half briefly inverts or brightens when its encoder moves.
  - The header row carries global state: MIDI activity, CPU load, page
    x/N.
  - A scope or spectrum view can take over one display temporarily, e.g.
    while a switch is held.

## Milestones

| # | Goal | Done when |
|---|---|---|
| M0 ✅ | Nano toolchain, linker, `start.S`, fip packaging, LED | LED1 toggles from an SD boot (2026-10-01) |
| M1 ✅ | Nano UART0 via FTDI; traps, PLIC, SBI timer IRQ | Boot log captured; 1 kHz tick, `time` +25,000,000/s; UART RX IRQ echo (2026-10-05) |
| M2 ✅ | Pico SDK tree; Pico A PIO I2S at 153.6 MHz sysclk | Clean 440 Hz sine from the PCM5102A; 10 min soak, 0 late refills, 750±1 blocks/s (2026-10-06) |
| M3 ✅ | Pico B: encoders, switches, 3 OLEDs, MIDI (Unit MIDI) | 6 encoders, 6 switches, 3 OLEDs (0x3C/0x3D on I2C0, 0x3C on I2C1) and MIDI in all working (2026-10-06) |
| M4 | Nano pinmux, GPIO IRQ, SPI2 master + DMA, cache handling | Logic-analyser check of SPI2 mode 3 at 8 MHz |
| M5 | rvlink end to end | The Nano's sine plays via Pico A; 0 CRC errors and 0 underruns over 10 min |
| M6 | Nano UART2 + rvpanel end to end | Encoders and MIDI reach the Nano; the Nano draws on all 3 OLEDs |
| M7 | Drone engine + `emu/` host build | Engine plays on the host, then on hardware via the panel and MIDI |
| M8 | SDHCI + FatFs presets | Save and load across power cycles |
| M9 | Perf (RVV), enclosure, single 5 V supply | CPU headroom ≥ 50 % at 4 voices |

### Status (2026-10-06)

**Done:**
- **M0–M3:** done on hardware.
  - Pico A plays a clean 440 Hz tone through the PCM5102A
    (`make pico-flash-audio`).
  - Pico B reads 6 encoders, 6 switches and MIDI, and drives 3 OLEDs
    (`make pico-flash-panel`).
- **USB boot dev loop:** `make usbboot RESET_PORT=…` takes about 20 s from
  Ctrl-R to the running image, with no SD card. The C906L is parked by
  `build/park.bin`.
- **Bring-up diagnostics:** M1's are still in `main.c` (init markers,
  per-second PLIC counters). Trim them once M4 is stable.

**Next:**
- **M4:** Nano SPI2 on P18/P21/P22/P23. Measure P22 for 3.3 V first.

**Optional:**
- **Faster USB boot:** slim OpenSBI (generic platform with only the 8250,
  ACLINT, PLIC and T-Head drivers) so the ROM stops re-requesting its
  276 KB window.

## References

- SG2002 TRM: https://github.com/sophgo/sophgo-doc/tree/main/SG200X/TRM
  (memory map: `system-architecture/memorymap_sg2002.table.rst`; IRQs:
  `interrupts.table.rst`)
- fiptool: https://github.com/sophgo/fiptool
- FSBL source (boot flow, `reset_c906l`):
  https://github.com/milkv-duo/duo-buildroot-sdk, under `fsbl/plat/cv181x/`
- Pinmux table: `u-boot-2021.10/board/cvitek/cv181x/cv181x_pinlist_swconfig.h`
  in the same SDK
- LicheeRV Nano wiki: https://wiki.sipeed.com/hardware/en/lichee/RV_Nano/1_intro.html
- Pico 2 W datasheet and pinout: https://datasheets.raspberrypi.com/picow/pico-2-w-datasheet.pdf
- RP2350 datasheet (PIO, PL022 SPI slave, UART):
  https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf
- The Milk-V Duo 256M uses the same SG2002, so its bare-metal material
  applies here.

### Verified on hardware (2026-10-01)

- Booting from SD works with fiptool's LZMA-wrapped `loader_2nd`. The
  vendor FSBL hands over to OpenSBI v1.8.1 (with its embedded DTB), and
  OpenSBI starts our S-mode payload at 0x80200000.
- LED1 toggles on A14 (FMUX 0x03001038 = 3, GPIO0 bit 14).
- The card that worked was a 32 GB SDHC with MBR and a single FAT32
  partition at a 4 MiB offset, holding `fip.bin` in its root.

### Verified on hardware (2026-10-05, M1)

- **UART0:** 0x04140000, DW 16550 with reg-shift 2, at 115200. The PLIC
  source is **44**.
- **PLIC:** 0x70000000, with the hart 0 S-mode context = 1. OpenSBI
  grants S-mode R/W on 0x70000000–0x73ffffff.
- **Timebase:** exactly 25 MHz. The OpenSBI banner shows
  `aclint-mtimer @ 25000000Hz`, and `time` advances 25,000,000 per 1000
  ticks.
- **SBI v3.0 extensions:** TIME, IPI, HSM, PMU, DBCN, FWFT, SSE.
- **DW UART busy-detect:** at handoff, an external interrupt is already
  pending (`sip=0x220`). Unless USR is read before the UART IRQ is
  enabled, it storms and main never runs. `uart_enable_rx_irq()` reads
  USR and IIR and drains RBR first, and the ISR also clears busy-detect.
- **Boot log:** the FSBL prints DDR3 at 1866 MT/s and BIST PASS, then the
  vendor `cvirtos` starts on the C906L ("RT: … CVIRTOS"). After that,
  "Jump to monitor at 0x80000000" and OpenSBI v1.8.1 run.

### Unverified (from research; confirm on hardware)

- That SPI2 really runs at 187.5 MHz as decoded, that a 528 B frame streams
  back to back in mode 3, and GPIO0's PLIC source (60). M4 checks these.
- That SD is SDHCI.
- **Little core:** since 2026-10-05 the fip carries `build/park.bin`
  instead of the vendor `cvirtos.bin`. The FSBL still releases the C906L,
  but it only runs a `wfi` loop at 0x83F40000. The top 2 MB (0x8FE00000+)
  stays reserved in the DTS.
