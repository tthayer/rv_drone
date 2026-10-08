/* Oscillator bank kernels: N oscillators (sine -> polyBLEP saw morph) rendered
 * for a whole control block and mixed into stereo buffers. Phases are 32-bit
 * fixed point (wrap is exact and free; t = phase / 2^32). The scalar and RVV
 * (XTheadVector, C906) versions do the same math in the same order. */
#ifndef OSC_H
#define OSC_H
#include <stdint.h>

#define OSC_MAX 16

typedef struct {
    uint32_t ph[OSC_MAX];       /* phase at the start of the block */
    uint32_t inc[OSC_MAX];      /* per-sample increment, 2^32 = one cycle */
    float dt[OSC_MAX], idt[OSC_MAX];   /* inc as a fraction of a cycle, and 1/dt */
    float gl[OSC_MAX], gr[OSC_MAX];    /* pan gains */
    int n;                      /* oscillators in use */
} osc_bank_t;

/* L[i] += sum_k out_k[i] * gl[k], R likewise, for i < frames (<= 64); advances ph. */
void osc_bank_scalar(osc_bank_t *b, float shape, int frames, float *L, float *R);
#ifdef ENGINE_RVV
void osc_bank_rvv(osc_bank_t *b, float shape, int frames, float *L, float *R);
#endif
#endif
