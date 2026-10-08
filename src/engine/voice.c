#include "voice.h"

/* tanh-style soft clip, as dsp_tanh(): clamp to +-3, x(27+x^2)/(27+9x^2) */
static inline float sat(float x)
{
    x = x < -3.0f ? -3.0f : x > 3.0f ? 3.0f : x;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void voice_lanes_scalar(voice_lanes_t *s, int frames)
{
    for (int l = 0; l < s->lanes; l++) {
        float ic1 = s->ic1[l], ic2 = s->ic2[l], env = s->env[l];
        float a1 = s->a1[l], a2 = s->a2[l], a3 = s->a3[l];
        float tgt = s->env_tgt[l], coef = s->env_coef[l];
        for (int i = 0; i < frames; i++) {
            float v3 = s->x[i][l] - ic2;
            float v1 = a1 * ic1 + a2 * v3;
            float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            float o = s->bp ? v1 * s->k : v2;
            env += (tgt - env) * coef;
            s->y[i][l] = sat(o * s->drive) * (env * s->drive_out);
        }
        s->ic1[l] = ic1;
        s->ic2[l] = ic2;
        s->env[l] = env;
    }
}

#ifdef ENGINE_RVV
#include <riscv_vector.h>

/* Lanes in chunks of up to 8 (e32/m2 at VLEN 128); each chunk keeps its state
 * and coefficients in registers for the whole block. Rows are VL_MAX floats
 * apart, so a chunk of row i is contiguous. */
void voice_lanes_rvv(voice_lanes_t *s, int frames)
{
    for (int l0 = 0; l0 < s->lanes;) {
        size_t vl = __riscv_vsetvl_e32m2((size_t)(s->lanes - l0));
        vfloat32m2_t ic1 = __riscv_vle32_v_f32m2(s->ic1 + l0, vl);
        vfloat32m2_t ic2 = __riscv_vle32_v_f32m2(s->ic2 + l0, vl);
        vfloat32m2_t env = __riscv_vle32_v_f32m2(s->env + l0, vl);
        vfloat32m2_t a1 = __riscv_vle32_v_f32m2(s->a1 + l0, vl);
        vfloat32m2_t a2 = __riscv_vle32_v_f32m2(s->a2 + l0, vl);
        vfloat32m2_t a3 = __riscv_vle32_v_f32m2(s->a3 + l0, vl);
        vfloat32m2_t tgt = __riscv_vle32_v_f32m2(s->env_tgt + l0, vl);
        vfloat32m2_t coef = __riscv_vle32_v_f32m2(s->env_coef + l0, vl);
        for (int i = 0; i < frames; i++) {
            vfloat32m2_t x = __riscv_vle32_v_f32m2(&s->x[i][l0], vl);
            vfloat32m2_t v3 = __riscv_vfsub_vv_f32m2(x, ic2, vl);
            /* v1 = a1*ic1 + a2*v3 ; v2 = ic2 + a2*ic1 + a3*v3 (same order as scalar) */
            vfloat32m2_t v1 = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(a1, ic1, vl),
                                                     __riscv_vfmul_vv_f32m2(a2, v3, vl), vl);
            vfloat32m2_t v2 = __riscv_vfadd_vv_f32m2(
                __riscv_vfadd_vv_f32m2(ic2, __riscv_vfmul_vv_f32m2(a2, ic1, vl), vl),
                __riscv_vfmul_vv_f32m2(a3, v3, vl), vl);
            ic1 = __riscv_vfsub_vv_f32m2(__riscv_vfmul_vf_f32m2(v1, 2.0f, vl), ic1, vl);
            ic2 = __riscv_vfsub_vv_f32m2(__riscv_vfmul_vf_f32m2(v2, 2.0f, vl), ic2, vl);
            vfloat32m2_t o = s->bp ? __riscv_vfmul_vf_f32m2(v1, s->k, vl) : v2;
            env = __riscv_vfadd_vv_f32m2(env, __riscv_vfmul_vv_f32m2(
                      __riscv_vfsub_vv_f32m2(tgt, env, vl), coef, vl), vl);
            /* saturator */
            vfloat32m2_t d = __riscv_vfmul_vf_f32m2(o, s->drive, vl);
            d = __riscv_vfmax_vf_f32m2(__riscv_vfmin_vf_f32m2(d, 3.0f, vl), -3.0f, vl);
            vfloat32m2_t d2 = __riscv_vfmul_vv_f32m2(d, d, vl);
            vfloat32m2_t num = __riscv_vfmul_vv_f32m2(d, __riscv_vfadd_vf_f32m2(d2, 27.0f, vl), vl);
            vfloat32m2_t den = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(d2, 9.0f, vl), 27.0f, vl);
            vfloat32m2_t y = __riscv_vfmul_vv_f32m2(__riscv_vfdiv_vv_f32m2(num, den, vl),
                                                    __riscv_vfmul_vf_f32m2(env, s->drive_out, vl), vl);
            __riscv_vse32_v_f32m2(&s->y[i][l0], y, vl);
        }
        __riscv_vse32_v_f32m2(s->ic1 + l0, ic1, vl);
        __riscv_vse32_v_f32m2(s->ic2 + l0, ic2, vl);
        __riscv_vse32_v_f32m2(s->env + l0, env, vl);
        l0 += (int)vl;
    }
}
#endif
