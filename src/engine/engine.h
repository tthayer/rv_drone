/* Drone engine: 4 voices of 3-7 detuned oscillators (sine -> saw morph, drift
 * LFOs, sub), a stereo SVF + saturator per voice, then chorus, feedback delay
 * and an 8-line FDN reverb. Float, no libm, no hardware: runs on the Nano
 * (main-loop context only; the trap entry does not save FP registers) and on
 * the host (emu/). Single instance. */
#ifndef ENGINE_H
#define ENGINE_H
#include <stdint.h>
#include "params.h"

#define ENGINE_VOICES 4

void  engine_init(float sample_rate);         /* defaults; latches a D2+A2 drone */
void  engine_set_param(int id, float value);  /* value in the parameter's unit */
float engine_param(int id);
void  engine_note_on(int note, int velocity); /* velocity 0 = note off */
void  engine_note_off(int note);
void  engine_all_off(void);
int   engine_voices_active(void);
/* n frames of stereo float, roughly +-1. Any n; internally 32-frame control blocks. */
void  engine_render(float *left, float *right, int n);
#endif
