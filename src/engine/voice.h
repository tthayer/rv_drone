/* Voice-path kernels: TPT state-variable filter + tanh-style drive + envelope
 * for many voice channels ("lanes") at once. Each active voice contributes two
 * lanes (L, R). The filter is recursive in time, so vectorisation runs across
 * lanes: per sample, all lanes advance together. The scalar and RVV
 * (XTheadVector) versions do the same math in the same order. */
#ifndef VOICE_H
#define VOICE_H

#define VL_MAX   32                 /* lanes: 16 voices x 2 channels */
#define VL_FRAMES 64                /* frames per call, max */

typedef struct {
    int lanes;                      /* in use, <= VL_MAX */
    /* per lane, read/written by the kernel */
    float ic1[VL_MAX], ic2[VL_MAX]; /* SVF state */
    float env[VL_MAX];              /* envelope level */
    /* per lane, read only */
    float a1[VL_MAX], a2[VL_MAX], a3[VL_MAX];   /* SVF coefficients */
    float env_tgt[VL_MAX], env_coef[VL_MAX];
    /* global, read only */
    float k;                        /* SVF damping (2 - 2*reso) */
    int bp;                         /* 1 = band-pass output (v1 * k), else low-pass (v2) */
    float drive, drive_out;         /* tanh(x * drive) * env * drive_out */
    /* x[i][lane] in, y[i][lane] out (row stride VL_MAX) */
    float x[VL_FRAMES][VL_MAX];
    float y[VL_FRAMES][VL_MAX];
} voice_lanes_t;

void voice_lanes_scalar(voice_lanes_t *s, int frames);
#ifdef ENGINE_RVV
void voice_lanes_rvv(voice_lanes_t *s, int frames);
#endif
#endif
