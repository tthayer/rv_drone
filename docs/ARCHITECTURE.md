# rv_drone — Architecture

A bare-metal drone synthesizer on the Sipeed LicheeRV Nano (Sophgo SG2002).
No Linux. Our binary owns the big C906 core after the vendor boot chain brings
up DDR.

## Hardware

Board: **LicheeRV-Nano-B** (no Ethernet, no Wi-Fi/BT; it does have the
onboard mic and speaker amp). Pin data comes from the 70405 (rev 1.2)
schematic,
https://cn.dl.sipeed.com/fileList/LICHEE/LicheeRV_Nano/02_Schematic/LicheeRV_Nano-70405_Schematic.pdf,
and the SDK pinlist `cv181x_pinlist_swconfig.h`.

| Function | Part | Interface | SG2002 block | Pins |
|---|---|---|---|---|
| Compute | LicheeRV Nano B | — | C906 @ 1 GHz, RV64GC + RVV 0.7.1, 256 MB DDR | — |
| Audio out | PCM5102A | I2S, SoC master, no MCLK (SCK tied low → internal PLL) | I2S2 @ 0x04120000 | **Ethernet footprint U7**, mux func 7: BCLK=ETH_TXM, LRCK=ETH_TXP, DOUT=ETH_RXP (ETH_RXM=IIS2_DI, unused) ⚠ |
| Display | SSD1306 128x64 (same as pseudopod) | I2C @ 400 kHz, addr 0x3C | I2C3 | SCL=P22 (R10), SDA=P23 (R11); external 4.7 kΩ pull-ups |
| MIDI in | 3.5 mm TRS Type A → H11L1/6N138 opto | UART RX, 31250 8N1 | UART3 | RX=P20 (R12) |
| Controls | 5 rotary encoders with push | GPIO | GPIO0..3 @ 0x03020000 | see allocation below |
| Storage | 32 GB microSD, FAT32 | SD | SD0 @ 0x04310000 | on-board slot |
| Debug | USB-UART | UART0 115200 | UART0 @ 0x04140000 | TX=A16 (L2), RX=A17 (L1) |
| Status LED | onboard LED1 | GPIO | — | A14 (R13) |

Header naming: L1–L14 is the left column and R1–R14 the right, top to
bottom, as on schematic sheet 1/4. Every header GPIO is 3.3 V per the
sheet 1/4 legend (Vio = 3.3 V for GPIOA/B/P).

**Encoder allocation** (A/B are the quadrature inputs, SW is the push switch;
internal pull-ups, switches to GND):

| Encoder | A | B | SW |
|---|---|---|---|
| 1 | A24 (L7) | A23 (L8) | B3 (R5) |
| 2 | A27 (L9) | A25 (L10) | P18 (R7) |
| 3 | A22 (L11) | A26 (L12) | P19 (R8) |
| 4 | A18 (R2) | A19 (R1) | P21 (R9) |
| 5 | A28 (R6) | A29 (R4) | A15 (L4) ⚠ |

L5/L6 are the speaker amp outputs (VOP/VON). They are unused, and nothing
may be connected to them.

⚠ **Before wiring:**

- **Audio pads.** The I2S signals exist only on the Ethernet footprint. No
  edge pin and no Wi-Fi footprint pad has an I2S function. Before we
  commit to it, check the following on the board itself (the designator
  drawing helps):
  - the U7 pad pitch and reachability;
  - that no magnetics, series capacitors or resistors sit between the pads
    and the SoC;
  - that the on-chip ETH PHY can be held off while the pads are muxed
    to function 7.

  Bring the three wires out to a small header, with short leads. BCLK is
  about 3 MHz at 48 kHz × 64.
- **Pad voltages.** Measure the P-pad rail (VDDIO_SD1) on P22 before
  connecting the OLED or the opto pull-up. The legend says 3.3 V, but it's
  per-domain.
- **A15.** This may be the speaker amp enable. Pressing encoder 5's switch
  would then just toggle an amp we don't use, which is harmless. If it's
  a problem, encoder 5's switch moves to A14 and we lose the LED.
- **JTAG.** A18/A19/A28/A29 are also the JTAG pins (TCK/TMS/TDI/TDO).
  During early bring-up, leave encoders 4–5 unconnected if we want JTAG
  debugging.
- **Spare pins.** None are left. A 6th encoder or extra buttons would need
  an I/O expander on the I2C bus (e.g. MCP23017).

**MIDI input circuit (TRS Type A):** tip = pin 4 (source), ring = pin 5
(sink). Tip goes through a 220 Ω resistor to the opto LED anode, and ring
goes to the cathode. Put a 1N4148 reverse-parallel across the LED. The opto
output is open-collector with a 470 Ω–1 kΩ pull-up to 3.3 V, into UART RX.
A footprint for a Type A/B swap jumper is cheap insurance.

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

Open risk: the `loader_2nd` header and compression handling. fiptool
LZMA-wraps the payload, and the BL2 source falls back to an uncompressed copy.
M0 confirms what actually boots.

## Software structure

```
src/
  boot/      start.S, linker script, trap entry, SBI calls
  hal/       uart, plic, timer, gpio+pinmux, i2c, i2s, dma, sdhci, cache
  drivers/   ssd1306, encoders, midi_uart, sd → FatFs diskio
  engine/    platform-independent DSP: oscillators, filters, mod, fx, voice mgmt
  ui/        pages, parameter model, rendering into a 1 KB framebuffer
  app/       main loop, wiring, preset load/save
third_party/ FatFs (ChaN), fiptool
emu/         host build: engine + ui on macOS (CoreAudio, terminal/SDL OLED)
```

`engine/` and `ui/` have **no hardware includes**. They take parameter
changes, MIDI events and render calls, and return audio blocks and a
framebuffer. That lets us run them on the host (`emu/`), which is the main
lever on iteration speed. Each hardware test otherwise means rebuilding
`fip.bin` and swapping the SD card.

## Runtime model (single big core)

| Context | Rate | Work |
|---|---|---|
| I2S DMA IRQ (highest) | every 64 frames @ 48 kHz (1.33 ms) | Renders the next half-buffer: drains the param/MIDI queues, then calls `engine_render(64)` into the inactive half |
| Timer IRQ | 1 kHz | Encoder quadrature decode (state-table, debounced) and button debounce; posts events to the UI queue |
| UART3 RX IRQ | per byte | MIDI byte goes into the SPSC ring; the parser runs in the audio context |
| Main loop | best effort | UI events → param changes (lock-free queue to audio) → redraw the dirty OLED pages over I2C; preset I/O on SD |

- **Audio format:** 48 kHz, 32-bit I2S slots with 24-bit data (the PCM5102A
  autodetects), stereo. Synthesis is in `float`, using the scalar FPU first.
  RVV 0.7.1 (T-Head toolchain) is an optimisation for later, if the profile
  calls for it.
- **DMA:** the system DMAC (0x04330000, IRQ 29, probably DW AXI DMAC)
  runs as a circular linked list over two halves.
- **Cache coherency:** the C906 doesn't snoop DMA. Before handing the DMA
  a buffer, we either clean the buffer with T-Head CMO instructions
  (`th.dcache.cva`), or place the audio buffers in a region we never cache.
  This is a known sharp edge, so we decide it in M3 with a scope on the
  output.
- **Display:** a full 1 KB frame takes about 25 ms at 400 kHz. So only
  the dirty 128-byte pages are sent, from the main loop, and never from IRQ
  context. The page-buffer logic ports from
  `../pseudopod/main/display/panel_ssd1306.c`.
- **Little core (C906L, 700 MHz):** unused at first. If UI or I2C jitter
  ever threatens audio, MIDI+UI can move there behind the mailbox
  (0x01900000). The FSBL already knows how to release it (`reset_c906l`).

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
| M0 | Toolchain, linker, `start.S`, polled UART0, fip packaging | "rv_drone hello" on UART0 from SD boot (and on QEMU `virt`) |
| M1 | Traps, PLIC, SBI timer IRQ | 1 kHz tick count printed each second |
| M2 | Pinmux + GPIO, encoders | Encoder deltas and pushes printed |
| M3 | Wire the U7 pads; I2S2 (ETH pads, mux func 7) + DMA, cache handling | Clean 440 Hz sine out of the PCM5102A, no clicks over 10 min |
| M4 | UART3 MIDI in (P20) | Note and CC events printed from a real keyboard |
| M5 | I2C + SSD1306 | Text and page UI on the OLED, with audio running glitch-free |
| M6 | Drone engine + `emu/` host build | Engine plays on the host, then on hardware via encoders and MIDI |
| M7 | SDHCI + FatFs presets | Save and load presets across power cycles |
| M8 | Perf (RVV / little core), enclosure polish | CPU headroom ≥ 50 % at 4 voices |

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
- The Milk-V Duo 256M uses the same SG2002, so its bare-metal material
  applies here.

### Unverified (from research; confirm on hardware)

- PLIC at 0x70000000 and CLINT at 0x74000000, recalled from the Linux DTS.
- That the UART registers are DW 16550-compatible with a 4-byte stride.
- That I2C is DesignWare and SD is SDHCI (inferred from the Linux drivers).
- The `loader_2nd` header format and whether LZMA is required.
- I2S2 on the Ethernet footprint pads (mux func 7): whether the pads are
  reachable and whether anything sits between them and the SoC. (Disproven
  earlier: I2S on A28/A18/A19. Those pads have no IIS function.)
- **The little core is not idle.** fiptool requires `--rtos`, so the FSBL
  loads the vendor `cvirtos.bin` to 0x83F40000 and releases the C906L. Until
  we replace that image with a tiny parking loop, two things hold:
  - Our heap and audio buffers must stay clear of that region and of the top
    2 MB (0x8FE00000+).
  - The RTOS may touch peripherals such as the mailbox or I2S.
