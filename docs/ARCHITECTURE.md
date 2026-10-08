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

Wiring diagrams (WireViz; source YAML next to them, `make wiring` to re-render):
`docs/wiring/system.svg` (single 5 V supply, board-to-board links, DAC with
its RC supply filter, debug UART) and `docs/wiring/panel.svg` (Pico B to the
encoders, OLEDs and Unit MIDI). The pin tables below remain the source of truth.

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
    reg-shift 2. FIFOs are 64 B each way (TRM §21.2.2).
  - **UART clock** (TRM Table 8.4 and vendor `clk-cv181x.c`): every UART's
    SCLK is **`clk_cam0_200`**, shared by UART0–4. It is the 25 MHz xtal when
    `clk_byp_0[16] = 1` (the reset default), otherwise `div_clk_cam0_200`
    (0x0A8; src 2 = DISPPLL, 1200 MHz by default).
    - The TRM's UART chapter (§21.2.4.1) instead describes `clk_sel_0` bits
      9–13 and 187.5 MHz. That contradicts the `clk_sel_0` register table
      (bits 22:1 reserved) and the vendor driver, so it is not used.
    - Consequence: from 25 MHz, the 115200 console is about 3 % off (the TRM
      says so itself). DISPPLL / 6 = 200 MHz would give the console 0.45 % and
      UART2 an exact 1.5625 Mbaud (divisor 8), but both UARTs must be
      re-divided at the same moment.
    - **Measured (2026-10-07):** `div_clk_cam0_200` = 0x00010009 (src 0 =
      osc, ÷1) gives 25 MHz. UART0 divisor 14 → **111,607 baud, −3.1 %**
      against 115200; the console works at that error. Boot prints
      `uart: clk_cam0_200 … UART0 divisor N -> B baud (E %)`, read-only. The
      clock is not changed yet.
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
  every GND tied together (on battery: the laptop charger caused a ground
  loop). Finished build: USB-C PD trigger (9 V; not every charger offers
  12 V) → buck set to 5.0–5.1 V (≥ 1.5 A, bulk cap) → star point feeding
  Nano L13, each Pico's VSYS (pin 39) through a Schottky diode (so a Pico's
  own USB can stay plugged in), and the PCM5102A VIN through an RC filter
  (10 Ω, 220 µF + 100 nF). Unit MIDI 5 V moves to Pico B VSYS. Never plug the
  Nano's USB-C in while the buck feeds L13. Budget ≈ 0.4–0.7 A at 5 V.

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
  | 2 | red | 5 V | Pico B VBUS (pin 40) now; VSYS (pin 39) in the single-supply build |
  | 3 | yellow | UART_MIDI_OUT, host → onboard SAM2695 synth | leave unconnected |
  | 4 | white | UART_MIDI_IN, from the opto | Pico B GP21 (UART1 RX) |

- **Mode switch:** set it to **Bypass**. Per the docs, Bypass routes MIDI
  IN to the Grove TX (white). In Separate mode that pin is described as
  floating.
- **DIN input:** verified on hardware (2026-10-07), notes and clock.
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

- Pico A is the audio clock master. It keeps a ring of up to 8 blocks (64
  stereo frames each), primed to 3, feeding PIO I2S at exactly 48 kHz.
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

- **Known issue (2026-10-07, largely resolved by grounding): link noise in the analog output.**
  - **Symptom:** with the engine silent (the Nano's `out: peak` reads 0/0), a
    high-pitched tone is audible on the **right** channel.
  - **Diagnosis:** it stops when the Nano holds the link idle, and persists
    with Pico B and the OLEDs unpowered. So it is electrical coupling of the
    7.8 MHz SPI bursts and DRQ edges (750/s) into the DAC output through the
    ground wiring, not data. It was masked by the old always-on boot drone.
  - **Confirmed:** fitting the Pico B ↔ Nano ground (wire 3 of the panel cable
    in `docs/wiring/system.svg`) cut the tone a lot.
  - **Resolved (mostly):** adding more ground wires between the boards almost
    entirely eliminated it. The remaining options below are for any residue.
  - **Rules:** every board-to-board ground in the diagram must be fitted,
    short and solid, ideally starred to one point. The Nano ↔ Pico A link
    ground (L3 → pin 23) carries the SPI return current, so it matters most.
  - **Further fixes:**
    - a dedicated PCM5102A ground to Pico A pin 13;
    - SPI/DRQ wires twisted with ground and routed away from the DAC and jack;
    - the planned RC filter on the DAC's VIN.

    Software mitigations not yet tried: lower SPI pad drive strength, slower
    SCK.
- **Known issue (2026-10-07, resolved): noise while holding a key on an
  external MIDI keyboard.**
  - **Symptom:** with the MicroFreak connected, the audio distorted or buzzed
    only while a key was held, and got worse when a ground wire was touched.
  - **Diagnosis:** with LATCH on the engine state doesn't change between held
    and released, and the Nano's `out:` meter stayed around −16 dBFS. It was
    a **ground loop**: the MicroFreak and rv_drone ran from different power
    sources, joined through the MIDI cable and the audio, and the player's
    hand on the touch keyboard closed the loop.
  - **Fix:** power the MicroFreak from **the same USB power source** as
    rv_drone. Generally: give connected gear one ground reference (same
    supply or power strip), or use a ground-loop isolator on the audio
    output.
- **Errors:** a CRC failure drops the block (Pico A plays silence for it)
  and bumps a counter. A sequence gap counts as an underrun.
- **CRC on Pico A, in hardware:** the RP2350 DMA sniffer runs on the RX
  channel. It is set to CRC-32 on bit-reversed data, with output reversed and
  inverted and a seed of ~0, which equals zlib's CRC-32. So each frame is
  checked as it arrives, with no software pass.
  - Over the whole 528 bytes, including the stored CRC, a good frame leaves
    the residue **0x2144DF1C**.
  - Between frames, the same sniffer seals the reply, through a mem-to-null
    DMA channel.
  - A boot self-test compares the sniffer with the software `rvlink_crc32`
    (including the "123456789" check value). If they differ, the link stays
    on software CRC (console: `rvlink CRC: hardware|software`).
  - Once a second, one live frame is also checked in software. Disagreements
    show as `hw/sw-mismatch`, which should be 0.
- **Latency:** the ring is primed to 3 × 64 frames = 4.0 ms (it was 4
  blocks, 5.3 ms, until 2026-10-07). The Nano always has the next block
  rendered before DRQ arrives.
- **Pico A clock:** sysclk is 153.6 MHz (48 kHz × 64 × 50), so the PIO
  divider is an integer and adds no fractional-divider jitter.

### Panel link: Nano ⇄ Pico B (`common/rvpanel.h`)

- **Framing:** UART 1.5625 Mbaud 8N1 (25 MHz / 16, an exact divisor on the
  Nano). Packet = type u8, payload, CRC-16/CCITT-FALSE (LE) over type+payload;
  COBS-encoded, 0x00 delimiter. Either side may send at any time; no acks.
  Bad CRC/COBS packets are dropped and counted. Max wire size 137 B (PAGE).
- **Nano → Pico B:**
  - `PAGE 0x01 {display u8, page u8, data[128]}`: changed pages at up to
    30 Hz, plus every page once a second (lost packets, Pico B reboot; also
    the keepalive).
  - `CONFIG 0x02 {contrast u8}`: all displays.
- **Pico B → Nano:**
  - `ENC 0x81 {id u8, delta i8}` (larger deltas are split)
  - `SW 0x82 {id u8, down u8}`
  - `MIDI 0x83 {len u8, bytes[3]}`: whole channel messages (status first),
    parsed on Pico B; realtime/SysEx are not forwarded yet.
  - `STATUS 0x84 {rx_ok u32, crc_err u16, cobs_err u16, dropped u16}`: once a
    second; the Nano prints it as `peer(...)`.
  - `CLOCK 0x85 {kind u8, t_us u32}`: MIDI clock (kind 0 tick 0xF8, 1 start
    0xFA, 2 continue 0xFB, 3 stop 0xFC), sent **from Pico B's UART1 IRQ** and
    stamped with its µs timer, so I2C flushes in its main loop add no jitter.
    All Pico B sends run with IRQs off per packet so packets never interleave.
- **Ownership:** until the first PAGE arrives, Pico B animates the boot
  splash (`render_splash`: title, elapsed seconds, a three-sine drone wave
  scrolling across the displays as one 384 px strip, about 30 fps). An
  encoder turn or switch press swaps it for the local stand-in UI. Once
  pages arrive it shows only the Nano's pages. After 2.5 s without a packet
  from the Nano it returns to the splash (or the stand-in UI if an input was
  used). Verified on hardware (2026-10-07): the splash runs through a Nano
  `make usbboot` (about 20 s) and hands over cleanly, with 0 OLED write errors.
- **Nano side:** `src/drivers/panel_link.c`. The UART2 RX IRQ decodes and
  queues events. The TX ring is drained by the UART2 **THR-empty interrupt** in
  programmable-threshold mode (`IER[7]`, `FCR[5:4]` = ¼ full), so the 64-byte FIFO
  is refilled whenever 16 bytes or fewer are left. Bursts therefore stream at
  the full 1.5625 Mbaud. Until 2026-10-07 it was drained from the 1 kHz tick,
  about 64 B/ms, roughly 40 % of the line rate. `txirq` in the panel console
  line counts these interrupts. Then
  `src/app/ui.c` (state + drawing with the shared `common/fb.c`; inverted
  `NANO n` header marks Nano-drawn screens).
- **Budget:** about 150 KB/s. A full frame on all 3 displays is about
  3.1 KB, so the link supports about 48 full refreshes per second, far
  more than the I2C side.
- **I2C refresh (Pico B):**
  - Both buses probe and initialise at 400 kHz, then switch to **1 MHz**
    (Fast-mode Plus) if every display ACKs a NOP at that speed, else they stay
    at 400 kHz. The banner prints the speed. Verified on hardware
    (2026-10-07): `I2C 1000 kHz, display writer on core 1`, all 3 OLEDs ok,
    0 write errors.
  - 1 MHz relies on the OLED modules' own pull-ups (typically 4.7–10 kΩ).
  - Only each page's **changed column range** is sent (`fb_t.lo/hi`,
    `ssd1306_write_range`), so turning a knob rewrites a few dozen bytes
    instead of whole 128-byte pages.
  - Core 1 does all I2C writes, so core 0 never blocks.
  - The once-a-second `oled: bytes …` console line shows the data volume per
    display.
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
common/              rvlink.h, rvpanel.h: protocols shared by all three builds;
                     fb.c, font5x7.c: framebuffer drawing (Nano UI + Pico B)
third_party/         FatFs (ChaN), OpenSBI clone (gitignored)
emu/                 host build: engine + ui on macOS (CoreAudio, terminal/SDL OLEDs)
```

`engine/` and `ui/` have **no hardware includes**. They take parameter
changes and MIDI events and return audio blocks and framebuffers, so they
also run on the host (`emu/`).

## Runtime model

**Nano (single big core):** the vendor FSBL runs the C906 at **1050 MHz**
(MPLL / 1; measured at boot by `src/hal/cpuclk.c`, see `docs/ENGINE.md`).
`CPU_MHZ=1000` (the default) only steps in if the clock is more than 10 % off
target.


| Context | Trigger | Work |
|---|---|---|
| GPIO IRQ (DRQ rising) | Pico A needs a block | Start the SPI2 TX/RX DMA, sending the already-rendered block |
| SPI DMA-complete IRQ | transfer done | Check the status reply, then render the next block (`engine_render(64)`, 1.33 ms budget) |
| UART2 RX IRQ | panel bytes | COBS decode, then queue events (encoder, switch, MIDI) for the audio context |
| Main loop | best effort | UI events → param changes → redraw the framebuffers → queue dirty pages to UART2 TX; preset I/O on SD |

- **Cache coherency:** the C906 doesn't snoop DMA. Clean the TX buffer and
  invalidate the RX buffer with T-Head CMO (`th.dcache.cva` / `th.dcache.iva`),
  or place both in an uncached region. Decided: T-Head CMO from S-mode
  (`src/hal/cache.c`; the vendor FSBL sets mxstatus.THEADISAEE, OpenSBI v1.8.1
  does not touch it; a boot-time probe falls back to polled SPI if it traps).
- **SPI2 DMA (M5, `src/hal/dma.c`, `spi.c`):** sysDMA = DW_axi_dmac at
  0x04330000, PLIC 29, IRQ routed to the big C906 via `sdma_dma_int_mux`
  (0x03000298 [18:10]); handshake slots 0/1 remapped to SPI2 RX/TX (req 20/21)
  in `sdma_dma_ch_remap0` (0x03000154). `BOARD_SPI_DMA=0` (EXTRA_CFLAGS) or
  the `d` console key selects the polled path; 3 consecutive DMA faults fall
  back to polled automatically. Verified on hardware (10 min soak, 0 errors).
- **Little core (C906L):** unused. The FSBL starts the vendor `cvirtos.bin`
  on it (see Unverified), and it will later be replaced by a parking loop.

**Pico A:**

- Core 0 runs the link: PIO SPI slave + DMA and DRQ, into the ring
  (`firmware/audio/audio_ring.h`: 8 blocks, primed to 3 = 4.0 ms). The CRC is
  checked by the DMA sniffer (see "Audio link").
- PIO DMA feeds I2S from the ring. Good frames without `RVLINK_F_TEST` are
  pushed; test frames are only validated.
- Flow control: DRQ is raised at each I2S block tick while the ring holds
  fewer than 3 blocks (`AUDIO_RING_TARGET`), plus a catch-up request 30 µs after a frame that left
  it short (priming, a lost frame). So the Nano renders exactly at the DAC
  rate, and no rate matching is needed.
- An underrun plays silence, is reported (`underruns` in the reply) and
  re-primes the ring.
- **Nano side:** the drone engine (`docs/ENGINE.md`) renders each block in
  main-loop context. The 330 Hz test tone of M5 was replaced by the engine in
  M7. The trap entry does not save FP registers, so float code must stay out
  of IRQ handlers. The `p` console key switches to the M4 test pattern (the
  ring then starves and catch-up requests run at about 1180 frames/s).

**Pico B:**

- **Core 0:**
  - the inputs: encoder PIO counts, the 1 kHz switch debounce timer, and the
    MIDI UART1 IRQ, which also forwards clock;
  - the panel link (UART0 RX IRQ ring, rvpanel decode and sends);
  - the local fallback UI.
- **Core 1:** the display writer. It owns both I2C buses after boot and loops
  over the displays, writing one dirty column range per display per pass.
  Contrast changes reach it as a request.
- **Sharing:** the framebuffers are shared under a hardware spin lock held
  only for a page copy, so neither core waits on the other's I2C traffic.

**Audio format:** 48 kHz exactly, stereo, 24-bit in 32-bit slots.
Synthesis is `float` on the Nano. RVV 0.7.1 (XTheadVector): an oscillator-bank
kernel plus a boot-time probe and self-test (`src/hal/vec.c`); see `docs/ENGINE.md`,
"SIMD (RVV) and profiling".

## Drone engine

Implemented in M7 (`src/engine/`). **Full engine documentation: `docs/ENGINE.md`**
(signal flow, parameters, clock follower, Nano integration, CPU/memory). Summary,
with these specifics: oscillators morph sine → polyBLEP saw (SHAPE), detune spread
across the bank plus ±10 cents of per-partial drift, a sine sub one octave
down, a stereo TPT SVF per voice with a per-voice sine LFO on cutoff (up to
±3 octaves), a tanh-style saturator, then chorus (2 taps per side) → cross-fed
damped delay (≤ 2 s) → 8-line Hadamard FDN reverb → volume + soft clip.
Control rate is 32 frames.
**MIDI clock follower** (external master, e.g. the MicroFreak): tempo = mean
of the last 24 tick intervals (Pico B timestamps; gaps > 250 ms restart the
average); a beat position advances at that tempo and is pulled onto the tick
grid each tick (snap if > 1 beat off); Start re-zeroes it. CLOCK page: SYNC
(OFF/MIDI), LFO DIV (FREE, 1/4 beat … 8 bars per filter-LFO cycle; voices
offset by ¼ cycle), DLY DIV (FREE, 1/16, 1/8, 1/8., 1/4, 1/4., 1/2; ≤ 2 s).
With no tick for 0.5 s everything returns to the free-running values. The
right display's header shows the tempo (`120>BPM` while running).
**Verified on hardware (2026-10-07)** with the MicroFreak as master (its
sequencer playing): about 70 ticks/s reach Pico B, the Nano tracks 176 BPM
with the transport running, and LFO DIV audibly locks the sweep.
Verified in emu: 120 BPM + 1 BAR → filter-LFO period 2.02 s; 90 BPM +
2 beats → 1.33 s. Latch: a note-on with no keys held starts a new
chord. The engine starts silent; notes come from MIDI (it latched a D2 + A2
drone at boot until 2026-10-07).
CCs: 1 mod depth, 7 volume, 71 reso, 72 release, 73 attack, 74 cutoff,
91 reverb, 93 chorus, 20–47 = parameters 0–27 in page order (44 SYNC,
45 LFO DIV, 46 DLY DIV, 47 STACK); 123 = all off.

### Initial sketch

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
| [####  ] | [##    ] | | [##### ] | [###   ] | | [##    ] | [##### ] |  d
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
| M4 ✅ | Nano pinmux, GPIO IRQ, SPI2 master (polled); Pico A PIO SPI slave | rvlink test pattern at 7.8 MHz: 10 min soak, 433k frames, 4 CRC errors, 0 pattern errors (2026-10-06). DMA + cache moved to M5 |
| M5 ✅ | rvlink end to end | Nano 330 Hz sine plays via Pico A's ring: 10 min soak, 457k frames, 0 CRC errors, 0 underruns, 0 late refills (2026-10-06) |
| M6 ✅ | Nano UART2 + rvpanel end to end | 6 encoders, 6 switches and MIDI reach the Nano; the Nano draws all 3 OLEDs; 0 link errors in steady state (2026-10-06) |
| M7 ✅ | Drone engine + `emu/` host build | Engine plays on the host, then on hardware via the panel and MIDI |
| M8 ✅ | SDHCI + FatFs presets | Saved slot 1, power-cycled the Nano, USB-booted: `LAST.TXT` restored slot 1 and all 28 params matched (2026-10-07) |
| M9 | Perf (RVV), enclosure, single 5 V supply | CPU headroom ≥ 50 % at full polyphony: **met** (worst case 16 voices × 16 osc = 48 % with RVV, 2026-10-07); enclosure and the single 5 V supply are still to do |

### Status (2026-10-06)

**Done:**
- **M0–M7:** done on hardware. The Nano renders audio and streams it over
  rvlink (SPI2 DMA, 7.8 MHz) into Pico A's ring and out of the PCM5102A.
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
- **M5 ✅ (2026-10-06):** real audio over rvlink.
  - Nano renders a 330 Hz sine (`src/app/tone.c`); Pico A plays it from an
    8-block ring with DRQ flow control (see Runtime model). 10 min soak:
    457k frames at 750/s; 0 CRC/magic/DMA/SPI errors on the Nano; Pico A 0 CRC,
    short, underrun, overflow or late; ring steady at 3–4 blocks.
    2 spurious DRQ edges were filtered (crosstalk).
  - Earlier prerequisites: non-blocking Nano console; DRQ hold-off for 20 µs
    after each frame; Pico A short detection moved from the CS edge IRQ
    (it fired mid-frame) to a stall check at the block tick.
  - 10 min soak: 450k frames at 750/s, 0 CRC, magic, pattern, short or
    gap errors on either side.
  - Nano SPI DMA works (DW AXI DMAC, SPI2 handshakes 20/21, T-Head CMO).
    10 min soak: 450k DMA frames, 0 errors on either side, 0 DMA errors,
    and IRQs stay enabled during frames.
- **M6 ✅ (2026-10-06):** rvpanel end to end. All 6 encoders, 6 switches and
  MIDI notes (140 of 140 messages in the capture window) reach the Nano; the
  Nano draws all 3 OLEDs. 0 CRC/COBS errors both ways in steady state.
  - OLED 1's 0x3D address resistor had fallen off (it answered at 0x3C and
    mirrored OLED 0); re-soldered.
  - Follow-ups: pull-up on the Nano's UART2 RX pad (A29): with Pico B
    unpowered the line floats and the decoder counts noise as COBS/CRC
    errors. MIDI realtime (clock) is not forwarded yet.
- **M7 ✅ (2026-10-06):** drone engine + `emu/`.
  - `src/engine/` (engine.c, params.c, dsp.h: no libm, identical on host and
    Nano), `src/ui/ui.c` (4 pages × 6 params, MIDI routing),
    `src/app/panel_ui.c` (panel glue), `emu/main.c` (SDL2 window + audio,
    `--wav` offline render with level stats; `make emu`, `make emu-wav`).
  - Host: default drone renders clean (peak −6 dBFS, no NaN/DC; spectrum
    shows D2/A2 partials and the sub octave; `emu --wav` defaults to those notes).
  - Hardware: encoders/pages and MIDI chords drive the engine. Render cost
    per 64-frame block: 290 µs (2 voices × 5 osc), 444 µs worst case
    (4 voices × 7 osc, console key `w`) = 33 % of the 1333 µs budget.
  - Encoders: 4 quadrature counts per click on all six (`UI_COUNTS_PER_DETENT`).
  - Audio noise/dropouts were a ground loop through the laptop charger
    (cleared on battery); not the PCM5102A module.
- **M8 (done, 2026-10-07):** SD + FatFs presets.
  - `src/hal/sd.c`: polled SDHCI on SD0 (0x04310000, DWC MSHC). Setup per the
    vendor Linux driver: CLK_EN_0 bits 18–20, pads func 0 + pulls (FMUX
    0x900/0xA00–0xA14), SD_PWRSW_CTRL (0x030001F4) = 3.3 V, MSHC_CTRL /
    PHY_TX_RX_DLY / PHY_CONFIG defaults after each reset. Base clock 375 MHz
    assumed (DTS); identify at 400 kHz, then 4-bit at 23.4 MHz. SD0_PWR_EN
    stays the LED GPIO; the card works with it (card VDD is not gated by it).
    Long waits call an idle hook = `audio_link_poll`, so saves don't starve
    Pico A (0 underruns across a save).
  - FatFs R0.15a (`tools/get-vendor.sh`, checksummed; config `src/fs/ffconf.h`:
    mkfs on, 8.3 names, no RTC), `src/fs/diskio.c`, `src/app/preset_fs.c`.
  - Presets: `/presets/P01.TXT`..`P16.TXT`, text `NAME=position` (0..10000)
    per line, robust to parameter additions; `LAST.TXT` = slot loaded at
    boot. UI page 6 PRESET (after OSC, FILTER, SPACE, AMP, CLOCK): enc 1 turn
    = slot, enc 2 push = load, enc 3 push = save. Console: `i` card info, `F` twice = format, `S`/`L` = slot 1.
  - Verified: 32 GB SDHC detected and formatted (FAT32, one MBR partition),
    save + load OK. Host: `make test-presets` round trip.
  - Power cycle (2026-10-07): saved slot 1 (`S`), cut the Nano's power, USB-booted;
    the boot loaded slot 1 via `LAST.TXT` and the `P` dump matched all 28 params.

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
- **SPI2 DMA:** DMAC register layout and single-LLI transfers with 8-bit beats,
  the handshake remap, the DMA IRQ route (int_mux), S-mode T-Head CMO and
  CS-low-to-first-SCK latency (a few us with DMA vs ~0 polled).
- **Little core:** since 2026-10-05 the fip carries `build/park.bin`
  instead of the vendor `cvirtos.bin`. The FSBL still releases the C906L,
  but it only runs a `wfi` loop at 0x83F40000. The top 2 MB (0x8FE00000+)
  stays reserved in the DTS.
