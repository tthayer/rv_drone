#include "osc.h"

#define PH_SCALE (1.0f / 4294967296.0f)

/* One sample: t in [0,1), dt/idt for polyBLEP. Shared definition of the math. */
static inline float osc_sample(float t, float dt, float idt, float shape)
{
    /* sine: sin(2 pi t) = -sin(2 pi u), u = t - 0.5; parabola + one refinement */
    float z = 2.0f * (t - 0.5f);
    float az = z < 0.0f ? -z : z;
    float y = 4.0f * z * (1.0f - az);
    float ay = y < 0.0f ? -y : y;
    float sn = -(y + 0.225f * (y * ay - y));
    /* polyBLEP saw */
    float saw = 2.0f * t - 1.0f;
    if (t < dt) {
        float x = t * idt;
        saw -= x * (2.0f - x) - 1.0f;
    } else if (t > 1.0f - dt) {
        float x = (t - 1.0f) * idt;
        saw -= x * (x + 2.0f) + 1.0f;
    }
    return sn + shape * (saw - sn);
}

void osc_bank_scalar(osc_bank_t *b, float shape, int frames, float *L, float *R)
{
    for (int k = 0; k < b->n; k++) {
        uint32_t ph = b->ph[k], inc = b->inc[k];
        float dt = b->dt[k], idt = b->idt[k], gl = b->gl[k], gr = b->gr[k];
        for (int i = 0; i < frames; i++) {
            float s = osc_sample((float)(ph + (uint32_t)i * inc) * PH_SCALE, dt, idt, shape);
            L[i] += s * gl;
            R[i] += s * gr;
        }
        b->ph[k] = ph + (uint32_t)frames * inc;
    }
}

#ifdef ENGINE_RVV
#include <riscv_vector.h>

void osc_bank_rvv(osc_bank_t *b, float shape, int frames, float *L, float *R)
{
    for (int k = 0; k < b->n; k++) {
        uint32_t ph = b->ph[k], inc = b->inc[k];
        float dt = b->dt[k], idt = b->idt[k], gl = b->gl[k], gr = b->gr[k];
        for (int i0 = 0; i0 < frames;) {
            size_t vl = __riscv_vsetvl_e32m4((size_t)(frames - i0));
            /* phase = ph + (i0 + lane) * inc, exact uint32 wrap */
            vuint32m4_t idx = __riscv_vadd_vx_u32m4(__riscv_vid_v_u32m4(vl), (uint32_t)i0, vl);
            vuint32m4_t pu = __riscv_vadd_vx_u32m4(__riscv_vmul_vx_u32m4(idx, inc, vl), ph, vl);
            vfloat32m4_t t = __riscv_vfmul_vf_f32m4(__riscv_vfcvt_f_xu_v_f32m4(pu, vl), PH_SCALE, vl);
            /* sine */
            vfloat32m4_t z = __riscv_vfmul_vf_f32m4(__riscv_vfsub_vf_f32m4(t, 0.5f, vl), 2.0f, vl);
            vfloat32m4_t az = __riscv_vfsgnjx_vv_f32m4(z, z, vl);
            vfloat32m4_t y = __riscv_vfmul_vv_f32m4(__riscv_vfmul_vf_f32m4(z, 4.0f, vl),
                                                    __riscv_vfrsub_vf_f32m4(az, 1.0f, vl), vl);
            vfloat32m4_t ay = __riscv_vfsgnjx_vv_f32m4(y, y, vl);
            vfloat32m4_t yy = __riscv_vfsub_vv_f32m4(__riscv_vfmul_vv_f32m4(y, ay, vl), y, vl);
            vfloat32m4_t sn = __riscv_vfneg_v_f32m4(__riscv_vfmacc_vf_f32m4(y, 0.225f, yy, vl), vl);
            /* polyBLEP saw */
            vfloat32m4_t saw = __riscv_vfsub_vf_f32m4(__riscv_vfmul_vf_f32m4(t, 2.0f, vl), 1.0f, vl);
            vbool8_t lo = __riscv_vmflt_vf_f32m4_b8(t, dt, vl);
            vbool8_t hi = __riscv_vmfgt_vf_f32m4_b8(t, 1.0f - dt, vl);
            vfloat32m4_t x1 = __riscv_vfmul_vf_f32m4(t, idt, vl);
            vfloat32m4_t b1 = __riscv_vfsub_vf_f32m4(
                __riscv_vfmul_vv_f32m4(x1, __riscv_vfrsub_vf_f32m4(x1, 2.0f, vl), vl), 1.0f, vl);
            vfloat32m4_t x2 = __riscv_vfmul_vf_f32m4(__riscv_vfsub_vf_f32m4(t, 1.0f, vl), idt, vl);
            vfloat32m4_t b2 = __riscv_vfadd_vf_f32m4(
                __riscv_vfmul_vv_f32m4(x2, __riscv_vfadd_vf_f32m4(x2, 2.0f, vl), vl), 1.0f, vl);
            vfloat32m4_t blep = __riscv_vfmv_v_f_f32m4(0.0f, vl);
            blep = __riscv_vmerge_vvm_f32m4(blep, b1, lo, vl);
            blep = __riscv_vmerge_vvm_f32m4(blep, b2, hi, vl);
            saw = __riscv_vfsub_vv_f32m4(saw, blep, vl);
            /* morph, pan, accumulate */
            vfloat32m4_t s = __riscv_vfmacc_vf_f32m4(sn, shape, __riscv_vfsub_vv_f32m4(saw, sn, vl), vl);
            vfloat32m4_t vl_l = __riscv_vle32_v_f32m4(L + i0, vl);
            vfloat32m4_t vl_r = __riscv_vle32_v_f32m4(R + i0, vl);
            __riscv_vse32_v_f32m4(L + i0, __riscv_vfmacc_vf_f32m4(vl_l, gl, s, vl), vl);
            __riscv_vse32_v_f32m4(R + i0, __riscv_vfmacc_vf_f32m4(vl_r, gr, s, vl), vl);
            i0 += (int)vl;
        }
        b->ph[k] = ph + (uint32_t)frames * inc;
    }
}
#endif
