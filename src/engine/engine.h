/* Drone engine: 16 voices of 3-7 detuned oscillators (sine -> saw morph, drift
 * LFOs, sub), a stereo SVF + saturator per voice, then chorus, feedback delay
 * and an 8-line FDN reverb. Float, no libm, no hardware: runs on the Nano
 * (main-loop context only; the trap entry does not save FP registers) and on
 * the host (emu/). Single instance. */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdint.h>
#include "params.h"

#ifndef ENGINE_VOICES
#define ENGINE_VOICES 16
#endif

void  engine_init(float sample_rate);         /* defaults, all voices silent */
void  engine_set_param(int id, float value);  /* value in the parameter's unit */
float engine_param(int id);
void  engine_note_on(int note, int velocity); /* velocity 0 = note off */
void  engine_note_off(int note);
void  engine_all_off(void);
int   engine_voices_active(void);
void  engine_voice_info(int i, int *note, float *env);   /* read-only, for tests and the UI */
/* MIDI clock follower (24 PPQN). t_us is the sender's timestamp of the event
 * (Pico B's microsecond timer): tempo comes from tick spacing, so link jitter
 * does not reach it. Start re-zeroes the beat position. */
enum { ENGINE_CLK_TICK, ENGINE_CLK_START, ENGINE_CLK_CONTINUE, ENGINE_CLK_STOP };
void  engine_clock(int kind, uint32_t t_us);
float engine_bpm(void);                       /* 0 = no clock in the last 0.5 s */
int   engine_clock_running(void);             /* between Start/Continue and Stop */

/* Per-stage render time, in the timer's ticks, accumulated since the last take.
 * osc = oscillator banks; voice = sub + filter + drive + envelope; then the
 * effect passes; total = whole engine_render(), frames = frames rendered. */
typedef struct {
    uint64_t osc, voice, chorus, delay, reverb, total;
    uint32_t frames;
} engine_profile_t;
void engine_set_timer(uint64_t (*now)(void));      /* NULL = no profiling */
void engine_profile_take(engine_profile_t *out);  /* copy and reset */

/* Vector kernels (RVV / XTheadVector, Nano build only). Returns the new state:
 * stays 0 when the engine was built without ENGINE_RVV. Main-loop context only:
 * the trap entry saves neither FP nor vector registers. */
int   engine_set_simd(int on);
int   engine_simd(void);

/* n frames of stereo float, roughly +-1. Any n; internally 32-frame control blocks. */
void  engine_render(float *left, float *right, int n);
#endif
