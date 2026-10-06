#include "tone.h"
#include "board.h"

/* Rotating phasor (c, s) by (cw, sw) each sample; renormalised once per block
 * so float rounding can't make the amplitude drift. No libm needed. */
static float c, s, cw, sw, gain;

static float series_sin(float x)       /* x small: |x| < 0.1 for audio tones */
{
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}

static float series_cos(float x)
{
    float x2 = x * x;
    return 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f));
}

void tone_init(float hz, float amp)
{
    float w = 6.28318530717958647692f * hz / 48000.0f;
    cw = series_cos(w);
    sw = series_sin(w);
    c = 1.0f;
    s = 0.0f;
    gain = amp * 8388607.0f;
}

void tone_render(int32_t *lr, unsigned frames)
{
    for (unsigned i = 0; i < frames; i++) {
        int32_t v = (int32_t)(s * gain) * 256;       /* 24-bit, left-aligned */
        lr[2 * i] = v;
        lr[2 * i + 1] = v;
        float nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    /* |phasor| -> 1 with one Newton step on 1/sqrt (error is ~1e-7 per block). */
    float k = 1.5f - 0.5f * (c * c + s * s);
    c *= k;
    s *= k;
}
