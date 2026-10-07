# rv_drone drone engine

The engine runs on the LicheeRV Nano's C906 core in `src/engine/`. It is plain
C11 float code with no libm and no hardware access, so the Mac emulator (`emu/`)
runs exactly the same code and produces the same audio. This document
describes what it does, how it is driven, and how it runs on the Nano.
`docs/ARCHITECTURE.md` covers the board-level system.

| File | Contents |
|---|---|
| `src/engine/engine.c`, `engine.h` | voices, filter, effects, MIDI clock follower, `engine_render()` |
| `src/engine/params.c`, `params.h` | the parameter table: ranges, curves, units, formatting, CC map, clock divisions |
| `src/engine/dsp.h` | libm-free math: `dsp_exp2`, `dsp_mtof`, `dsp_sin1`, `dsp_tanh`, `dsp_noise`, `dsp_flush` |
| `src/ui/ui.c` | encoder pages, MIDI routing, presets, display drawing; calls into the engine |
| `src/app/main.c` (`render_block`) | Nano glue: renders one rvlink block, converts it to 24-bit, measures the time it takes |

## Signal flow

```
 per voice (x4) ------------------------------------------------------------
  osc 1..N (N = OSCS, 3-7)                          per-voice filter LFO
  sine -> polyBLEP saw (SHAPE)                      (free Hz, or synced)
  detune spread + drift LFO ---> pan (SPREAD) --+           |
  sub: sine one octave down (SUB, centre) ------+--> stereo SVF (LP/BP, RESO)
                                                    --> tanh drive (DRIVE)
                                                    --> envelope (ATTACK/RELEASE)
 ---------------------------------------------------------------------------
 sum of voices x 0.45
  --> chorus (2 modulated taps per side, CHORUS)
  --> feedback delay (stereo, cross-fed, damped, DELAY/FEEDBACK/DLY MIX)
  --> FDN reverb (8 lines, SIZE/DAMP/REVERB)
  --> volume (VOLUME^2, smoothed) --> soft clip --> L/R float, about +-1
```

The engine works in **32-frame control blocks**. Pitches, pans, envelope
coefficients, filter coefficients, LFO phases and effect settings are worked
out once per control block. Oscillators, filters, the envelope and the
effects run per sample. `engine_render(l, r, n)` accepts any `n` and splits it
into control blocks internally.

## Voices

- **Four voices.** Each voice holds one MIDI note (plus TRANSPOSE). At boot
  `engine_init()` latches **D2 + A2** (notes 38 and 45), so the hardware makes
  sound with no MIDI attached.
- **Voice allocation for a note-on:**
  1. the voice already playing that note (it retriggers);
  2. otherwise a silent voice;
  3. otherwise a releasing voice;
  4. otherwise the oldest voice.

  A voice that starts from silence gets random oscillator phases and drift
  rates, so no two notes start identically.
- **LATCH ON (the default):** note-offs are ignored. The first note-on after
  all keys have been released starts a new chord, releasing the previous one.
  This holds a drone chord with no sustain pedal.
- **LATCH OFF:** note-offs release their voice, so an external sequencer can
  gate notes. CC 123 (All Notes Off) releases everything in either mode.

### Oscillator bank

- **Oscillator count:** OSCS, 3 to 7 per voice.
- **Detune:** the oscillators are spread linearly across ±DETUNE cents. Each
  one also has its own **drift LFO**: a sine of ±(DRIFT × 10) cents at a
  random rate of 0.05–0.17 Hz. This slow beating is what makes the drone move.
- **Waveform:** a crossfade from sine to **polyBLEP saw**, set by SHAPE
  (0 = pure sine, 1 = band-limited saw).
- **Stereo:** neighbouring oscillators are panned to opposite sides with an
  equal-power law; SPREAD sets the width.
- **Level:** the bank is normalised by roughly 1/√N, so changing OSCS doesn't
  jump the level.
- **Sub:** a sine one octave below the root, level SUB × 1.2, panned to the
  centre and added before the filter.

### Filter, drive, envelope

- **Filter:** a stereo **TPT state-variable filter** (Zavalishin topology:
  stable when modulated, no delay-free-loop problems).
  - MODE selects LP, or BP with its gain normalised to the peak.
  - The damping is k = 2 − 2·RESO, with RESO from 0 to 95 %.
  - The coefficient g = tan(π·fc/fs) comes from the `dsp_sin1` ratio.
- **Cutoff:** CUTOFF, smoothed per control block so encoder steps don't
  zipper, times 2^(MODDEPTH × 3 × lfo), so modulation reaches up to
  ±3 octaves. The result is clamped to 20 Hz … 0.42·fs.
- **Filter LFO:** one sine per voice.
  - Free-running, its rate is MODRATE × (1 + 0.13·voice index), so the voices
    drift apart.
  - Synced to MIDI clock, see below. Voices are then offset by ¼ cycle.
- **Drive:** `tanh_approx(x × (1 + 3·DRIVE)) / (1 + DRIVE)` on each channel
  after the filter.
- **Envelope:** a one-pole glide towards 1 (gate on) or 0 (gate off). The time
  constant is ATTACK/3 or RELEASE/3, so the level is about 95 % of the way
  there after ATTACK or RELEASE seconds. A voice is freed when its level falls
  below 1e-4 with the gate off.

## Effects (shared)

The summed voices are scaled by 0.45 into the effects chain.

- **Chorus:**
  - Two linearly interpolated taps per channel, at 12 ms ± 4 ms.
  - Modulated by a 0.23 Hz LFO in quadrature (L and R a quarter-cycle apart).
  - Cross taps at 1.37× the opposite channel's delay.
  - Output: `out += CHORUS × 0.6 × (tap + cross tap)`.
- **Delay:**
  - Stereo with cross-fed feedback (ping-pong-like) and a one-pole lowpass in
    the loop, so repeats darken.
  - Up to 2 s; DELAY sets the time in free mode.
  - The time glides (0.0005 per sample), so changing it bends the pitch like
    tape instead of clicking.
  - FEEDBACK goes up to 95 %; DLY MIX sets the wet level.
- **Reverb:** an 8-line **feedback delay network**.
  - Line lengths: primes from 1031 to 2969 samples, scaled by
    (0.5 + 1.3·SIZE).
  - Orthonormal Hadamard mixing (fast in-place butterflies, ×1/√8).
  - Each line has its own gain for an RT60 of 1.5 + 12·SIZE² seconds, and a
    one-pole damping lowpass set by DAMP.
  - Input goes to the lines with alternating signs. Even lines feed L, odd
    lines feed R. REVERB sets the wet level.
- **Output:** volume is VOLUME², smoothed per sample, then `tanh_approx(x × 1.5)`
  soft-clips the output into ±1.

## MIDI clock follower

The MicroFreak (or any MIDI clock master) sets the tempo. Pico B timestamps
every clock byte in its UART interrupt and forwards it to the Nano as an
rvpanel `CLOCK` packet. See `docs/ARCHITECTURE.md`, "Panel link".

- **Tempo:** the mean of the last 24 tick intervals (one beat at 24 PPQN),
  from Pico B's timestamps, so link jitter doesn't reach it. An interval over
  250 ms restarts the average. The tempo counts as valid once there are 4
  intervals and the last tick was under 0.5 s ago.
- **Beat position:** advances every control block at the measured tempo. On
  each tick it is pulled 25 % of the way onto the tick grid, or snapped if it
  is more than a beat off. **Start** resets it so the next tick is beat 0.
  Stop and Continue only switch the "running" indicator.
- **What the clock drives** (CLOCK page, SYNC = MIDI, with a valid tempo):
  - LFO DIV sets one filter-LFO cycle per ¼ beat, ½ beat, 1 beat, 2 beats,
    1 bar, 2 bars, 4 bars or 8 bars. The LFO phase comes straight from the beat
    position.
  - DLY DIV sets the delay to 1/16, 1/8, dotted 1/8, 1/4, dotted 1/4 or 1/2,
    capped at 2 s.
  - FREE in either setting keeps the Hz or millisecond parameter.
- **Without a clock** (no tick for 0.5 s), everything falls back to the free
  values. The header shows `120>BPM` while the clock is running and `120 BPM`
  while it is stopped.
- **Checked in emu:** at 120 BPM with 1 BAR, the filter-LFO period measures
  2.02 s. At 90 BPM with 2 beats, it measures 1.33 s.

## Parameters

Each parameter has a **normalised position** (0–1, which is what an encoder
moves and what presets store) and a **value** in its own unit (what the engine
uses). EXP parameters are spaced logarithmically. INT and ENUM parameters move
exactly one step per encoder click, and ENUMs wrap around.

| Page | Param | Range | Default | Curve | CC |
|---|---|---|---|---|---|
| OSC | DETUNE | 0–50 cents | 12 c | lin | 20 |
| OSC | DRIFT | 0–100 % (±10 c) | 30 % | lin | 21 |
| OSC | SHAPE | sine → saw | 60 % | lin | 22 |
| OSC | OSCS | 3–7 | 5 | int | 23 |
| OSC | SUB | 0–100 % | 30 % | lin | 24 |
| OSC | SPREAD | 0–100 % | 70 % | lin | 25 |
| FILTER | CUTOFF | 40 Hz–12 kHz | 900 Hz | exp | 26, **74** |
| FILTER | RESO | 0–95 % | 30 % | lin | 27, **71** |
| FILTER | MODRATE | 0.01–2 Hz | 0.07 Hz | exp | 28 |
| FILTER | MODDEPTH | 0–100 % (±3 oct) | 35 % | lin | 29, **1** |
| FILTER | DRIVE | 0–100 % | 25 % | lin | 30 |
| FILTER | MODE | LP / BP | LP | enum | 31 |
| SPACE | CHORUS | 0–100 % | 40 % | lin | 32, **93** |
| SPACE | DELAY | 20 ms–2 s | 850 ms | exp | 33 |
| SPACE | FEEDBACK | 0–95 % | 55 % | lin | 34 |
| SPACE | DLY MIX | 0–100 % | 25 % | lin | 35 |
| SPACE | SIZE | 0–100 % | 75 % | lin | 36 |
| SPACE | REVERB | 0–100 % | 45 % | lin | 37, **91** |
| AMP | ATTACK | 10 ms–20 s | 2 s | exp | 38, **73** |
| AMP | RELEASE | 50 ms–30 s | 4 s | exp | 39, **72** |
| AMP | LATCH | OFF / ON | ON | enum | 40 |
| AMP | TRANSPOS | −24…+24 st | 0 | int | 41 |
| AMP | DAMP | 0–100 % | 50 % | lin | 42 |
| AMP | VOLUME | 0–100 % (squared) | 70 % | lin | 43, **7** |
| CLOCK | SYNC | OFF / MIDI | MIDI | enum | 44 |
| CLOCK | LFO DIV | FREE, 1/4 … 8 BAR | FREE | enum | 45 |
| CLOCK | DLY DIV | FREE, 1/16 … 1/2 | FREE | enum | 46 |

CC values 0–127 map to a position of 0–1. CC 123 is All Notes Off.

**UI:** the six encoders show the six parameters of the current page; each
display shows two. Pushing encoder 1 steps through OSC → FILTER → SPACE → AMP
→ CLOCK → PRESET. Pushing any other encoder resets its parameter to the
default. The PRESET page is described in the README ("SD card and presets").
Presets store every parameter's position by name, so adding a parameter later
doesn't break older files.

## Running on the Nano

- **Where it runs:** `render_block()` in `src/app/main.c` is the rvlink render
  callback. The audio link calls it **from the main loop** right after each
  SPI frame, to have the next 64-frame block ready before Pico A asks for it.
  It renders, clamps, converts to 24-bit left-aligned `int32` and times the
  work with `rdtime`.
- **Float stays out of interrupts.** The trap entry doesn't save FP
  registers, so the engine, `ui_*` and the parameter code must only run in
  main-loop context. Check with
  `riscv64-elf-objdump -d build/nano/rv_drone.elf`: FP instructions should
  appear only in engine, UI, params and render functions. `main()` sets
  `sstatus.FS`.
- **Concurrency:** UI events (encoders, MIDI, clock) also reach the engine
  from the main loop, so engine state never needs locking on the Nano. The
  emulator locks the SDL audio device around UI calls instead.
- **SD stalls:** long SD waits call `audio_link_poll()` as an idle hook, so
  rendering continues while a preset saves.
- **Latency:** Pico A's 4-block buffer plus one block in flight is about
  6.7 ms from render to DAC. MIDI note and parameter changes take effect at
  the next render, so they're quantised to block boundaries of 1.33 ms.
- **CPU cost** (C906, `-O2`, no vector instructions yet), per 64-frame block
  out of a 1333 µs budget:

  | Load | Time | CPU |
  |---|---|---|
  | 1 voice × 5 osc | ~240 µs | 18 % |
  | 2 voices × 5 osc (boot drone) | ~290 µs | 21 % |
  | 4 voices × 7 osc (worst case, console `w`) | ~444 µs | 33 % |

  The effects cost about 140 µs per block whatever the voice count. That figure
  is inferred from the measurements above, not measured on its own. The
  console prints the render average, maximum and load each second, and the
  right display's header shows the load.
- **Memory** (static `.bss`, about 1.3 MiB):

  | Buffer | Size |
  |---|---|
  | delay | 2 × 131072 floats = 1 MiB |
  | reverb | 8 × 8192 floats = 256 KiB |
  | chorus | 2 × 2048 floats = 16 KiB |

  The delay, chorus and reverb read positions use integer index arithmetic, so
  the 32-bit write counters never lose precision over long runs.
- **Numerics:**

  | Function | Accuracy |
  |---|---|
  | `dsp_exp2` | relative error ~1e-4 (0.14 cent) |
  | `dsp_sin1` | absolute error 0.0011 |
  | `dsp_tanh` | a rational soft clipper, deliberately not exact tanh (max difference 0.024) |

  Feedback paths flush values below 1e-15 to zero (`dsp_flush`), so
  denormals can't build up.

## Working on the engine

- **Listen on the Mac:** `make emu`, then `build/emu/rv_drone_emu`. The mouse
  wheel turns the encoders, the keyboard plays notes, and `k` toggles a test
  clock.
- **Check without listening:** `build/emu/rv_drone_emu --wav out.wav --set NAME=value --clock BPM`
  prints peak, RMS, DC and a NaN count. `.venv` has numpy for spectrum or
  modulation checks.
- **Adding a parameter:**
  1. Append it to the enum in `params.h`, so its position is also its CC
     offset (CC 20 + id).
  2. Add its row in `params.c`. The last page may have fewer than six
     parameters.
  3. Use `E.p[...]` in `engine.c`.
  4. Update this document's table and the CC list in `docs/ARCHITECTURE.md`.

  Presets pick it up automatically.
- **Tests:** `make test-presets` runs a preset round trip through the UI and
  engine.
- **Measure on hardware:** console `w` sets up the worst-case load. The
  console's `engine:` line shows the render time.
