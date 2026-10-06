/* Small float DSP helpers with no libm, so the Nano (freestanding) and the host
 * (emu/) compute identical audio. Accuracy is audio-grade, not IEEE-exact. */
#ifndef DSP_H
#define DSP_H
#include <stdint.h>

#define DSP_PI     3.14159265358979323846f
#define DSP_TWO_PI 6.28318530717958647692f

static inline float dsp_clamp(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* 2^x, |x| < 30: integer part via the exponent field, fraction via a 5th-order
 * polynomial (max rel. error ~1e-4 = 0.14 cent). */
static inline float dsp_exp2(float x)
{
    float fl = (float)(int32_t)x;
    if (fl > x) fl -= 1.0f;
    float f = x - fl;
    float p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f +
              f * (0.00961812911f + f * 0.00133335581f))));
    union { float f; int32_t i; } u = { p };
    u.i += (int32_t)fl << 23;
    return u.f;
}

/* Note number (fractional) -> Hz, A4 = 69 = 440 Hz. */
static inline float dsp_mtof(float note)
{
    return 440.0f * dsp_exp2((note - 69.0f) * (1.0f / 12.0f));
}

/* Soft saturator: tanh-like rational, exact 0 slope 1, clamps to +-1 at |x| >= 3. */
static inline float dsp_tanh(float x)
{
    x = dsp_clamp(x, -3.0f, 3.0f);
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* sin(2 pi p) for any p (period 1): parabola plus one refinement step
 * (max abs error ~0.001). */
static inline float dsp_sin1(float p)
{
    float z = 2.0f * (p - (float)(int32_t)p);     /* (-2, 2) */
    if (z >= 1.0f) z -= 2.0f;
    else if (z < -1.0f) z += 2.0f;                /* [-1, 1): angle = z * pi */
    float az = z < 0.0f ? -z : z;
    float y = 4.0f * z * (1.0f - az);
    float ay = y < 0.0f ? -y : y;
    return y + 0.225f * (y * ay - y);
}

/* xorshift32 noise in [-1, 1). */
static inline float dsp_noise(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* Flush tiny values (denormal guard for feedback paths). */
static inline float dsp_flush(float x)
{
    return (x > -1e-15f && x < 1e-15f) ? 0.0f : x;
}
#endif
