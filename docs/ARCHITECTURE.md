# rv_drone — Architecture

A bare-metal drone synthesizer on two boards:

- **Sipeed LicheeRV Nano (SG2002)**: runs the DSP engine. No Linux; our
  binary owns the big C906 core after the vendor boot chain brings up DDR.
- **Raspberry Pi Pico 2 W (RP2350)**: handles all I/O and owns the audio
  clock. It drives I2S to the PCM5102A and reads the encoders, MIDI and
  OLED.

The two boards are linked by SPI. The Nano header has no I2S pins
(see "Rejected audio paths"), which is why the I/O lives on the RP2350.

## Hardware

```
      LicheeRV Nano (DSP engine)                   Pico 2 W (I/O + audio clock)
 ┌───────────────────────────────┐   SPI1     ┌───────────────────────────────┐
 │ engine, fx, 256 MB DDR        │  mode 3    │ PIO I2S   ───▶ PCM5102A       │
 │ SPI master + DMA              │◀──────────▶│ PIO quad  ◀─── 5 encoders     │
 │                               │◀── DRQ ────│ UART1 RX  ◀─── MIDI opto      │
 │ UART0 debug ─▶ FTDI           │            │ I2C0      ───▶ SSD1306        │
 └───────────────────────────────┘            └───────────────────────────────┘
                 └──────────────── common GND ────────────────┘
```

| Function | Part | Where | Interface |
|---|---|---|---|
| DSP | LicheeRV Nano B | Nano | C906 @ 1 GHz, RV64GC + RVV 0.7.1, 256 MB DDR |
| Link | — | Nano ⇄ Pico | SPI, Nano master, mode 3, 8 MHz to start; DRQ from Pico |
| Audio out | PCM5102A | Pico | I2S (PIO), Pico master, 64 fs, no MCLK (SCK tied low) |
| Displays | SSD1306 128x64 (as in pseudopod), up to 8 | Pico | I2C0 → TCA9548A mux (0x70) → one OLED per channel (0x3C each) |
| MIDI in | 3.5 mm TRS Type A → H11L1/6N138 opto | Pico | UART1 RX, 31250 8N1 |
| Controls | 5 rotary encoders with push | Pico | PIO quadrature + GPIO |
| Storage | 32 GB microSD, FAT32 | Nano | SD0 (on-board slot) |
| Debug | FTDI adapter (5 V; divider on its TX) / USB CDC | Nano UART0 / Pico USB | 115200 |

The OLEDs are on the Pico, behind a TCA9548A I2C mux. That allows up to 8
displays on GP4/GP5 with no extra pins, since each SSD1306 only offers
0x3C or 0x3D. The Nano renders a 1 KB framebuffer per display and streams
dirty pages across the link, one 128-byte page per block.

Budget: 750 pages/s ≈ 94 full frames/s across all displays, e.g. 4
displays at about 23 fps. On the I2C side, 400 kHz gives about 25 ms per
full frame. SSD1306s usually tolerate 1 MHz, so we try that. Moving the OLED back
to the Nano's I2C3 (P22/P23) is a local change if it's ever wanted.

### Nano pins

Header naming: L1–L14 is the left column and R1–R14 the right, top to
bottom (schematic sheet 1/4), with USB-C at the bottom (the L14/R14 end;
L13/L14 are 5 V). On a breadboard: L sits in column c and R in column i,
leaving a/b (left) and j (right) free. Every header GPIO is 3.3 V.

| Pin | Pad | Use |
|---|---|---|
| L1 | A17 | UART0 RX ← FTDI TX via 1 kΩ / 1.8 kΩ divider |
| L2 | A16 | UART0 TX → FTDI RX |
| L3 | GND | common GND (FTDI, Pico) |
| L7 | A24 | SPI1 CS → Pico GP17 |
| L8 | A23 | SPI1 MISO ← Pico GP19 |
| L9 | A27 | DRQ (GPIO in) ← Pico GP20 |
| L10 | A25 | SPI1 MOSI → Pico GP16 |
| L11 | A22 | SPI1 SCK → Pico GP18 |
| R13 | A14 | onboard LED1 (status) |

- **L5/L6 are the speaker amp outputs (VOP/VON).** Never connect anything to
  them.
- Everything else is spare.
- UNVERIFIED: the SPI1 mux function number for each of A22–A25. Check the
  SDK pinlist before M4.

### Pico 2 W pins

GP23/24/25/29 belong to the CYW43 wireless chip, and the onboard LED is on
the CYW43, so none of those are available. That leaves 26 GPIOs, and all
of them are used:

| GP | Pico pin | Use | GP | Pico pin | Use |
|---|---|---|---|---|---|
| 0 | 1 | Enc 4 SW | 14 | 19 | Enc 3 B |
| 1 | 2 | Enc 5 SW | 15 | 20 | Enc 2 SW |
| 2 | 4 | Enc 1 A | 16 | 21 | SPI0 RX ← Nano MOSI |
| 3 | 5 | Enc 1 B | 17 | 22 | SPI0 CSn ← Nano CS |
| 4 | 6 | I2C0 SDA (TCA9548A) | 18 | 24 | SPI0 SCK ← Nano SCK |
| 5 | 7 | I2C0 SCL (TCA9548A) | 19 | 25 | SPI0 TX → Nano MISO |
| 6 | 9 | Enc 2 A | 20 | 26 | DRQ → Nano A27 |
| 7 | 10 | Enc 2 B | 21 | 27 | Enc 4 A |
| 8 | 11 | Enc 1 SW | 22 | 29 | Enc 4 B |
| 9 | 12 | UART1 RX ← MIDI opto | 26 | 31 | Enc 5 A |
| 10 | 14 | I2S BCK → PCM5102A | 27 | 32 | Enc 5 B |
| 11 | 15 | I2S LRCK → PCM5102A | 28 | 34 | Enc 3 SW |
| 12 | 16 | I2S DIN → PCM5102A | | | |
| 13 | 17 | Enc 3 A | | | |

- **Encoders:** A/B are adjacent GPIOs, as the PIO quadrature program
  needs. Switches use internal pull-ups and pull to GND.
- **Pico console:** uses USB CDC, which frees GP0/GP1 from UART0.
- **More encoders:** a 6th would need an MCP23017 on I2C0.
- **Power:** during development each board runs from its own USB, with
  GND tied between them. For the finished build, feed 5 V to Nano L13
  (VSYS) and Pico pin 39 (VSYS) from one supply.

**MIDI input circuit (TRS Type A):**

- Tip is pin 4 (source) and ring is pin 5 (sink).
- Tip goes through 220 Ω to the opto LED anode, and ring goes to the
  cathode. Put a 1N4148 across the LED, in reverse.
- The opto output is open-collector, with a 470 Ω–1 kΩ pull-up to 3.3 V,
  into GP9.
- A Type A/B swap jumper footprint is cheap insurance.

### Rejected audio paths (why the Pico exists)

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

## Nano ⇄ Pico link

**Roles:**

- The Pico is the clock master for audio. It keeps a ring of 4 output
  blocks (64 stereo frames each) feeding PIO I2S at exactly 48 kHz.
- It raises **DRQ** whenever a slot is free and its reply is preloaded in
  the SPI TX FIFO/DMA.
- The Nano answers each DRQ with one fixed-length, full-duplex transaction.

**Electrical:**

- SPI mode 3 (CPOL=1, CPHA=1). The RP2350's PL022 in slave mode with
  CPHA=0 needs CS to toggle between frames, and mode 3 avoids that.
- Start at 8 MHz. The RP2350 slave limit is clk_peri/12, about 12.8 MHz
  at a 153.6 MHz sysclk.
- The payload needs about 4.3 Mbit/s, so 8 MHz gives roughly 2× headroom.

**Transaction (1024 bytes each way; little-endian; CRC32 last):**

| Nano → Pico | Pico → Nano |
|---|---|
| magic `'RVL1'`, seq u16, flags u16 | magic `'RVP1'`, seq echo u16, ring fill u8, underruns u16 |
| audio: 64 × (L,R) int32, 24-bit left-aligned (512 B) | event count u8 |
| OLED: display id u8 + page index u8 + 128 B page data | events: up to 64 × {type u8, id u8, value i16} (encoder delta, switch, MIDI message, status) |
| reserved / padding | padding |
| CRC32 | CRC32 |

- **The Pico parses MIDI** into whole messages, so the Nano never sees raw
  bytes.
- **Errors:** a CRC failure on either side drops that block (the Pico
  plays silence for it) and bumps a counter. A sequence gap counts as an
  underrun.
- **Latency:** up to 4 × 64 frames ≈ 5.3 ms worst case. The Nano always
  has the next block already rendered when DRQ arrives.
- **Shared definitions:** both builds include the struct layouts and
  constants from `common/rvlink.h`.

**Pico audio clock:** sysclk is set to 153.6 MHz (48 kHz × 64 × 50), so the
PIO divider is an integer and adds no fractional-divider jitter.

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
  drivers/           link (rvlink master), sd → FatFs diskio
  engine/            platform-independent DSP: oscillators, filters, mod, fx, voices
  ui/                pages, parameter model, rendering into a 1 KB framebuffer
  app/               main loop, wiring, preset load/save
                     (ui/ renders one framebuffer per display)
firmware/rp2350/     Pico 2 W firmware (Pico SDK, C, CMake)
  audio_i2s.pio      I2S output, 64 fs
  quadrature.pio     encoder decode
  link.c             rvlink slave (SPI0 + DMA, DRQ)
  midi.c, oled.c     UART1 MIDI parser, SSD1306 page writer
common/rvlink.h      link protocol, shared by both builds
third_party/         FatFs (ChaN), OpenSBI clone (gitignored)
emu/                 host build: engine + ui on macOS (CoreAudio, terminal/SDL OLED)
```

`engine/` and `ui/` have **no hardware includes**. They take parameter
changes and MIDI events and return audio blocks and a framebuffer, so they
also run on the host (`emu/`).

## Runtime model

**Nano (single big core):**

| Context | Trigger | Work |
|---|---|---|
| GPIO IRQ (DRQ rising) | Pico needs a block | Start the SPI1 TX/RX DMA, sending the already-rendered block |
| DMA-complete IRQ | transfer done | CRC-check the reply, push its events to queues, then render the next block (`engine_render(64)`, 1.33 ms budget) |
| Main loop | best effort | UI events → param changes → UI redraw into the framebuffer; preset I/O on SD |

- **Cache coherency:** the C906 doesn't snoop DMA. Clean the TX buffer and
  invalidate the RX buffer with T-Head CMO (`th.dcache.cva` / `th.dcache.iva`),
  or place both in an uncached region. Decided in M4.
- **Little core (C906L):** unused. The FSBL starts the vendor `cvirtos.bin`
  on it (see Unverified), and it will later be replaced by a parking loop.

**Pico:**

- Core 0 handles the link: SPI0 DMA and DRQ, then the block ring.
- PIO DMA feeds I2S from the ring.
- Core 1 runs MIDI, the encoders (polled from the PIO FIFOs) and OLED
  page writes.
- An underrun repeats silence and is reported in the next reply.

- **Audio format:** 48 kHz exactly, stereo, 24-bit in 32-bit slots.
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
- **UI:** encoders are mapped onto pages of 4–6 parameters, and a push
  cycles the page. The OLED shows the page name, the values and a small
  scope.
- **Presets:** stored as flat binary/INI files in `/presets` on the same FAT
  partition as `fip.bin`.

## Milestones

| # | Goal | Done when |
|---|---|---|
| M0 ✅ | Nano toolchain, linker, `start.S`, fip packaging, LED | LED1 toggles from an SD boot (2026-10-01) |
| M1 | Nano UART0 via FTDI; traps, PLIC, SBI timer IRQ | Boot log captured; 1 kHz tick count printed each second |
| M2 | Pico scaffold (Pico SDK, CMake) + PIO I2S at 153.6 MHz sysclk | Clean 440 Hz sine from the PCM5102A, no clicks over 10 min |
| M3 | Pico encoders, MIDI (UART1), OLED | Events printed over USB CDC; text on the OLED |
| M4 | Nano pinmux, GPIO IRQ, SPI1 master + DMA, cache handling | Loopback/scope check of SPI1 mode 3 at 8 MHz |
| M5 | rvlink end to end | The Nano's sine plays via the Pico; Pico events arrive at the Nano; 0 CRC errors and 0 underruns over 10 min |
| M6 | Drone engine + `emu/` host build | Engine plays on the host, then on hardware via encoders and MIDI |
| M7 | SDHCI + FatFs presets | Save and load across power cycles |
| M8 | Perf (RVV), enclosure, single 5 V supply | CPU headroom ≥ 50 % at 4 voices |

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
- RP2350 datasheet (PIO, PL022 SPI slave):
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

### Unverified (from research; confirm on hardware)

- PLIC at 0x70000000 and CLINT at 0x74000000 (from the DTS).
- That UART0 is DW 16550-compatible with a 4-byte stride, and the 25 MHz
  timebase. M1 checks both.
- That SPI1 is DesignWare SSI, its clock source and mux function numbers,
  and that it streams back to back in mode 3. M4 checks this.
- That SD is SDHCI.
- **The little core is not idle.** fiptool requires `--rtos`, so the FSBL
  loads the vendor `cvirtos.bin` to 0x83F40000 and releases the C906L.
  Until that image is replaced with a parking loop:
  - keep our heap and buffers clear of that region and of the top 2 MB
    (0x8FE00000+);
  - expect the RTOS may touch peripherals such as the mailbox.
