# rv_drone — Architecture

A bare-metal drone synthesizer on the Sipeed LicheeRV Nano (Sophgo SG2002).
No Linux. Our binary owns the big C906 core after the vendor boot chain brings
up DDR.

## Hardware

| Function | Part | Interface | SG2002 block | Pins (Nano) |
|---|---|---|---|---|
| Compute | LicheeRV Nano | — | C906 @ 1 GHz, RV64GC + RVV 0.7.1, 256 MB DDR | — |
| Audio out | PCM5102A | I2S, SoC master, no MCLK (SCK tied low → internal PLL) | I2S2 @ 0x04120000 | BCLK=A28, LRCK=A18, DOUT=A19 ⚠ |
| Display | SSD1306 128x64 (same as pseudopod) | I2C @ 400 kHz, addr 0x3C | I2C1 or I2C3 (header) | TBD from schematic |
| MIDI in | 3.5 mm TRS Type A → H11L1/6N138 opto | UART RX, 31250 8N1 | UART1 (A29) or UART3 (P20) ⚠ | TBD |
| Controls | 4–6 rotary encoders w/ push | GPIO, 3 pins each | GPIO0..3 @ 0x03020000 | TBD |
| Storage | 32 GB microSD, FAT32 | SD | SD0 @ 0x04310000 | on-board slot |
| Debug | USB-UART | UART0 115200 | UART0 @ 0x04140000 | A16 TX / A17 RX |

⚠ **Verify before wiring.** The I2S2 pad mapping comes from the SDK pinlist
(UART2 pads, mux function 6), not from the board schematic. A28 and A29 are
also UART1's pads. Each pad has its own mux, so A29 *should* stay UART1_RX
while A28 is IIS2_BCLK. If that doesn't hold, MIDI moves to UART3 on P20.
Header I/O is 3.3 V per the Sipeed wiki. Confirm that per pin, since the
PCM5102A, SSD1306 and opto pull-up all assume 3.3 V.

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
| UART1 RX IRQ | per byte | MIDI byte goes into the SPSC ring; the parser runs in the audio context |
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
| M3 | I2S2 + DMA, cache handling | Clean 440 Hz sine out of the PCM5102A, no clicks over 10 min |
| M4 | UART1 MIDI in | Note and CC events printed from a real keyboard |
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
- The I2S2 header pinout, and whether A28/A29 can be split between I2S and
  UART.
- **The little core is not idle.** fiptool requires `--rtos`, so the FSBL
  loads the vendor `cvirtos.bin` to 0x83F40000 and releases the C906L. Until
  we replace that image with a tiny parking loop, two things hold:
  - Our heap and audio buffers must stay clear of that region and of the top
    2 MB (0x8FE00000+).
  - The RTOS may touch peripherals such as the mailbox or I2S.
