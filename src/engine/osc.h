/* Oscillator bank kernels: N oscillators (sine -> polyBLEP saw morph) rendered
 * for a whole control block and mixed into stereo buffers. Phases are 32-bit
 * fixed point (wrap is exact and free; t = phase / 2^32). The scalar and RVV
 * (XTheadVector, C906) versions do the same math in the same order. */
#ifndef OSC_H
#define OSC_H
#include <stdint.h>

#define OSC_MAX 16

/* Band-limited single-cycle wavetables (issue #3). Each table has WT_LEVELS
 * MIP levels, one per octave, with harmonics above that level's Nyquist
 * removed; the oscillator picks the level from its pitch. The table lookup is
 * scalar only: XTheadVector exposes no indexed load (GCC gates the RVV gather
 * on the 'v' extension, which this target does not have), so a wavetable bank
 * is rendered by the scalar path even when the RVV kernels are selected. */
#define WT_N      1024            /* samples per level (power of two) */
#define WT_LEVELS 10              /* harmonic caps 512 >> level = 512..1 */
#define WT_TABS   4               /* SAW, SQUARE, ORGAN, CUSTOM */

void osc_wavetable_init(void);                 /* build the built-in tables */
/* Replace the CUSTOM table (WT_TABS-1) from one single cycle of `n` samples
 * (resampled to WT_N). Used as-is: supply a band-limited cycle if high notes
 * matter. */
void osc_wavetable_set_custom(const float *cycle, int n);
/* Base of the MIP level for wavetable `tab` (0..WT_TABS-1) at `inc` (2^32 =
 * one cycle), chosen so the table's harmonics stay under Nyquist. */
const float *osc_wavetable_level(int tab, uint32_t inc);

typedef struct {
    uint32_t ph[OSC_MAX];       /* phase at the start of the block */
    uint32_t inc[OSC_MAX];      /* per-sample increment, 2^32 = one cycle */
    float dt[OSC_MAX], idt[OSC_MAX];   /* inc as a fraction of a cycle, and 1/dt */
    float gl[OSC_MAX], gr[OSC_MAX];    /* pan gains */
    const float *wt[OSC_MAX];   /* non-NULL: read this MIP level instead of osc_sample */
    int n;                      /* oscillators in use */
} osc_bank_t;

/* L[i] += sum_k out_k[i] * gl[k], R likewise, for i < frames (<= 64); advances ph. */
void osc_bank_scalar(osc_bank_t *b, float shape, int frames, float *L, float *R);
#ifdef ENGINE_RVV
void osc_bank_rvv(osc_bank_t *b, float shape, int frames, float *L, float *R);
#endif
#endif
