# rv_drone controls

How to play rv_drone from the panel and from MIDI. For how each control works
inside the engine, see `docs/ENGINE.md`.

## Power-up

1. The three displays show a short test pattern, then the boot splash: "RV
   DRONE", a "booting" line, the seconds elapsed, and a waveform scrolling
   across all three displays.
2. The Nano boots from the SD card in a few seconds and takes over the
   displays. It loads the preset named in `/presets/LAST.TXT`, the last slot
   you loaded or saved.
3. It stays **silent** until a note arrives over MIDI.

The splash comes back whenever the Nano stops talking to the panel for 2.5 s.
Turning an encoder or pressing a switch during the splash replaces it with a
panel test screen (raw encoder counts and switch states).

## The panel

Six encoders with push switches, under three displays. Each display shows two
encoders, left half and right half:

```
   Display 1             Display 2             Display 3
+---------+---------+ +---------+---------+ +---------+---------+
| page name    x/6  | | MIDI  N57      []  | | V3  120>BPM    10% |  header
|  enc 1  |  enc 2  | |  enc 3  |  enc 4  | |  enc 5  |  enc 6  |
+---------+---------+ +---------+---------+ +---------+---------+
```

**Headers:**

- **Display 1:** the page name and its number, for example `OSC 1/6`.
- **Display 2:** `MIDI`, the last message received, and an activity box that
  lights for each message. Messages show as `N57` (note on, note 57), `n57`
  (note off), `C74` (controller 74), `P5` (program change) or `B` (pitch bend).
- **Display 3:** the voices sounding (`V3`), the MIDI clock tempo when a clock
  is coming in (`120>BPM` while it runs, `120 BPM` while it is stopped), and
  the Nano's CPU load.

**Each half** shows the parameter's name, its value and a bar for its position.
A frame flashes around a half when its value changes, and the half inverts
while its switch is held.

## Encoders and switches

- **Turn** an encoder to change its parameter.
  - Fast turns (3 or more clicks at once) move 4× as far.
  - Whole-number parameters (OSCS, TRANSPOS) move one step per click, two when
    turned fast.
  - Choice parameters (MODE, LATCH, STACK, REV MODE…) step through their
    options and wrap around at the ends.
- **Push encoder 1** for the next page: OSC → FILTER → SPACE → AMP → MODES →
  PRESET → back to OSC. Encoder 1 still turns the first parameter on each page.
- **Push encoders 2–6** to reset their parameter to its default. The first
  parameter on each page can only be reset over MIDI or by loading a preset.

Changes take effect at once. They are not saved until you save a preset.

## Pages

Defaults in brackets. CC is the MIDI controller that also sets the parameter.

### 1. OSC: the oscillators

| Enc | Name | Range | What it does | CC |
|---|---|---|---|---|
| 1 | DETUNE | 0–50 cents [12 c] | spread of the oscillators around the note | 20 |
| 2 | DRIFT | 0–100 % [30 %] | slow random pitch wander of each oscillator, up to ±10 cents | 21 |
| 3 | SHAPE | 0–100 % [60 %] | waveform, from sine (0 %) to saw (100 %) | 22 |
| 4 | OSCS | 3–16 [5] | oscillators per voice | 23 |
| 5 | SUB | 0–100 % [30 %] | sine one octave below the note | 24 |
| 6 | SPREAD | 0–100 % [70 %] | stereo width of the oscillators | 25 |

### 2. FILTER

| Enc | Name | Range | What it does | CC |
|---|---|---|---|---|
| 1 | CUTOFF | 40 Hz–12 kHz [900 Hz] | filter frequency | 26, 74 |
| 2 | RESO | 0–95 % [30 %] | resonance | 27, 71 |
| 3 | MODRATE | 0.01–2 Hz [0.07 Hz] | speed of the filter LFO (when not synced) | 28 |
| 4 | MODDEPTH | 0–100 % [35 %] | how far the LFO sweeps the cutoff, up to ±3 octaves | 29, 1 |
| 5 | DRIVE | 0–100 % [25 %] | saturation after the filter | 30 |
| 6 | MODE | LP, BP [LP] | lowpass or bandpass | 31 |

Each voice has its own LFO, running at a slightly different rate, so the voices
of a chord move apart.

### 3. SPACE: the effects

| Enc | Name | Range | What it does | CC |
|---|---|---|---|---|
| 1 | CHORUS | 0–100 % [40 %] | chorus amount | 32, 93 |
| 2 | DELAY | 20 ms–2 s [850 ms] | delay time (when not synced) | 33 |
| 3 | FEEDBACK | 0–95 % [55 %] | delay repeats; each repeat gets darker | 34 |
| 4 | DLY MIX | 0–100 % [25 %] | delay level | 35 |
| 5 | SIZE | 0–100 % [75 %] | reverb size and decay time (1.5–13.5 s) | 36 |
| 6 | REVERB | 0–100 % [45 %] | reverb level | 37, 91 |

Changing DELAY glides the time, so it bends the pitch of the repeats like tape.

### 4. AMP: envelope and output

| Enc | Name | Range | What it does | CC |
|---|---|---|---|---|
| 1 | ATTACK | 10 ms–20 s [2 s] | fade-in time of each note | 38, 73 |
| 2 | RELEASE | 50 ms–30 s [4 s] | fade-out time after release | 39, 72 |
| 3 | LATCH | OFF, ON [ON] | hold chords without keeping keys down (below) | 40 |
| 4 | TRANSPOS | −24…+24 semitones [0] | shifts everything you play | 41 |
| 5 | DAMP | 0–100 % [50 %] | darkens the reverb tail | 42 |
| 6 | VOLUME | 0–100 % [85 %] | output level | 43, 7 |

**LATCH ON:** releasing keys does nothing, so a chord keeps sounding. The first
note after you have let go of every key starts a new chord, and the old chord
fades out over RELEASE. **LATCH OFF:** notes stop when you release them, for
playing normally or from a sequencer.

### 5. MODES: clock sync, stacking, reverb mode

| Enc | Name | Range | What it does | CC |
|---|---|---|---|---|
| 1 | SYNC | OFF, MIDI [MIDI] | follow an incoming MIDI clock | 44 |
| 2 | LFO DIV | FREE, 1/4, 1/2, 1 BT, 2 BT, 1 BAR, 2 BAR, 4 BAR, 8 BAR [FREE] | one filter-LFO cycle per division | 45 |
| 3 | DLY DIV | FREE, 1/16, 1/8, 1/8., 1/4, 1/4., 1/2 [FREE] | delay time as a note value | 46 |
| 4 | STACK | UNISON, OCTAVES, FIFTHS, ORGAN [UNISON] | intervals the oscillators are tuned to | 47 |
| 5 | REV MODE | HALL, SHIM OCT, SHIM 5TH, SUB OCT, FREEZE [HALL] | reverb character (below) | 48 |
| 6 | SHIMMER | 0–100 % [50 %] | strength of the shimmer modes | 49 |

**Clock:** with SYNC on MIDI and a clock coming in, LFO DIV and DLY DIV lock
to the tempo. FREE, SYNC OFF, or no clock for half a second falls back to
MODRATE and DELAY. A MIDI Start restarts the beat count.

**STACK** tunes the oscillators of each voice to a chord of intervals: OCTAVES
(0, +12, −12, +24), FIFTHS (0, +7, +12, +19, −12, +24) or ORGAN (drawbar-style
footages). UNISON keeps them all on the note. It works best with more OSCS.

**REV MODE:**

| Mode | Sound |
|---|---|
| HALL | the plain reverb |
| SHIM OCT | the reverb tail is shifted up an octave and fed back, so octaves bloom above the chord |
| SHIM 5TH | the same, a fifth up |
| SUB OCT | the same, an octave down: a darker, lower bloom |
| FREEZE | the reverb stops taking new sound and holds its current tail for minutes; whatever you play next sounds dry on top |

SHIMMER only acts in the three shimmer modes. Larger SIZE makes the shimmer
longer. To use FREEZE, play the chord with REV MODE on HALL, then switch to
FREEZE while it rings. Switch back to HALL to let the frozen tail decay.

### 6. PRESET

| Control | Action |
|---|---|
| Turn encoder 1 | pick a slot, 01–16 |
| Push encoder 2 | load the slot |
| Push encoder 3 | save the current settings to the slot (overwrites it) |
| Push encoder 1 | back to the OSC page |

Display 3 previews the selected slot before you load it: cutoff and
resonance, shape and detune, oscillators and sub, delay and feedback, reverb
and size, attack and release. A message confirms each action: `LOADED 03`,
`SAVED 03`, `EMPTY 05` (nothing saved there), `BAD FILE` or `NO CARD`.

Loading or saving a slot also makes it the one loaded at the next power-up.
Presets are text files on the SD card, `/presets/P01.TXT` to `P16.TXT`.

## MIDI

The MIDI input is the DIN socket on the Unit MIDI.

- **Channel:** all channels are accepted.
- **Notes** play voices, up to 16 at once. Velocity is ignored: every note
  plays at the same level. When all 16 voices are busy, the quietest one fades
  out over 30 ms and takes the new note.
- **Controllers:**
  - CC 20–49 set the parameters in page order, as listed in the tables above.
  - CC 1, 7, 71, 72, 73, 74, 91 and 93 set the usual synth controls: mod
    depth, volume, resonance, release, attack, cutoff, reverb and chorus.
  - CC 123 (All Notes Off) releases every voice, in either LATCH mode.
- **Clock:** Start, Stop, Continue and clock ticks drive the MODES page sync.
- **Ignored:** pitch bend and program change (they still show in the MIDI
  header). Aftertouch and other messages are not passed on by the panel.

## Quick starts

- **A slow pad:** hold a chord with LATCH on. Raise ATTACK and RELEASE, open
  CUTOFF a little, and turn up REVERB and SIZE.
- **Shimmer:** on MODES, set REV MODE to SHIM OCT and SHIMMER to about 60 %,
  with SIZE above 60 % and REVERB around 50 %.
- **Rhythmic movement:** send a MIDI clock, then set LFO DIV to 1 BAR and
  DLY DIV to 1/8 or the dotted 1/8., with some FEEDBACK.
- **Big organ chord:** set STACK to ORGAN, OSCS to 8 or more, SHAPE low.
