#include "params.h"
#include "dsp.h"

const char *const page_names[PAGE_COUNT] = { "OSC", "FILTER", "SPACE", "AMP", "MODES" };

static const char *const fmode_labels[] = { "LP", "BP" };
static const char *const onoff_labels[] = { "OFF", "ON" };
static const char *const sync_labels[] = { "OFF", "MIDI" };
static const char *const stack_labels[] = { "UNISON", "OCTAVES", "FIFTHS", "ORGAN" };
static const char *const rev_mode_labels[] = { "HALL", "SHIM OCT", "SHIM 5TH", "SUB OCT", "FREEZE" };
/* filter-LFO cycle length */
static const char *const lfo_div_labels[] = { "FREE", "1/4", "1/2", "1 BT", "2 BT", "1 BAR", "2 BAR", "4 BAR", "8 BAR" };
static const float lfo_div_beats[] = { 0, 0.25f, 0.5f, 1, 2, 4, 8, 16, 32 };
/* delay time */
static const char *const dly_div_labels[] = { "FREE", "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2" };
static const float dly_div_beats[] = { 0, 0.25f, 0.5f, 0.75f, 1, 1.5f, 2 };

float param_lfo_div_beats(int i) { return (i >= 0 && i < 9) ? lfo_div_beats[i] : 0.0f; }
float param_dly_div_beats(int i) { return (i >= 0 && i < 7) ? dly_div_beats[i] : 0.0f; }

static const param_desc_t table[P_COUNT] = {
    [P_DETUNE]     = { "DETUNE",  0.0f,   50.0f,   12.0f, CURVE_LIN,  UNIT_CENTS, 0, 0.01f },
    [P_DRIFT]      = { "DRIFT",   0.0f,   1.0f,    0.3f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_SHAPE]      = { "SHAPE",   0.0f,   1.0f,    0.6f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_OSCS]       = { "OSCS",    3.0f,   16.0f,   5.0f,  CURVE_INT,  UNIT_NONE,  0, 0.077f },
    [P_SUB]        = { "SUB",     0.0f,   1.0f,    0.3f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_SPREAD]     = { "SPREAD",  0.0f,   1.0f,    0.7f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_CUTOFF]     = { "CUTOFF",  40.0f,  12000.0f, 900.0f, CURVE_EXP, UNIT_HZ,   0, 0.005f },
    [P_RESO]       = { "RESO",    0.0f,   0.95f,   0.3f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_FMOD_RATE]  = { "MODRATE", 0.01f,  2.0f,    0.07f, CURVE_EXP,  UNIT_HZ,    0, 0.01f },
    [P_FMOD_DEPTH] = { "MODDEPTH", 0.0f,  1.0f,    0.35f, CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_DRIVE]      = { "DRIVE",   0.0f,   1.0f,    0.25f, CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_FMODE]      = { "MODE",    0.0f,   1.0f,    0.0f,  CURVE_ENUM, UNIT_NONE,  fmode_labels, 0.5f },
    [P_CHORUS]     = { "CHORUS",  0.0f,   1.0f,    0.4f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_DLY_TIME]   = { "DELAY",   20.0f,  2000.0f, 850.0f, CURVE_EXP, UNIT_MS,    0, 0.005f },
    [P_DLY_FB]     = { "FEEDBACK", 0.0f,  0.95f,   0.55f, CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_DLY_MIX]    = { "DLY MIX", 0.0f,   1.0f,    0.25f, CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_REV_SIZE]   = { "SIZE",    0.0f,   1.0f,    0.75f, CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_REV_MIX]    = { "REVERB",  0.0f,   1.0f,    0.45f, CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_ATTACK]     = { "ATTACK",  0.01f,  20.0f,   2.0f,  CURVE_EXP,  UNIT_S,     0, 0.01f },
    [P_RELEASE]    = { "RELEASE", 0.05f,  30.0f,   4.0f,  CURVE_EXP,  UNIT_S,     0, 0.01f },
    [P_LATCH]      = { "LATCH",   0.0f,   1.0f,    1.0f,  CURVE_ENUM, UNIT_NONE,  onoff_labels, 0.5f },
    [P_TRANSPOSE]  = { "TRANSPOS", -24.0f, 24.0f,  0.0f,  CURVE_INT,  UNIT_ST,    0, 1.0f / 48.0f },
    [P_DAMP]       = { "DAMP",    0.0f,   1.0f,    0.5f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_VOLUME]     = { "VOLUME",  0.0f,   1.0f,    0.85f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
    [P_SYNC]       = { "SYNC",    0.0f,   1.0f,    1.0f,  CURVE_ENUM, UNIT_NONE,  sync_labels, 0.5f },
    [P_LFO_DIV]    = { "LFO DIV", 0.0f,   8.0f,    0.0f,  CURVE_ENUM, UNIT_NONE,  lfo_div_labels, 0.125f },
    [P_DLY_DIV]    = { "DLY DIV", 0.0f,   6.0f,    0.0f,  CURVE_ENUM, UNIT_NONE,  dly_div_labels, 0.17f },
    [P_STACK]      = { "STACK",   0.0f,   3.0f,    0.0f,  CURVE_ENUM, UNIT_NONE,  stack_labels, 0.34f },
    [P_REV_MODE]   = { "REV MODE", 0.0f,  4.0f,    0.0f,  CURVE_ENUM, UNIT_NONE,  rev_mode_labels, 0.25f },
    [P_SHIMMER]    = { "SHIMMER", 0.0f,   1.0f,    0.5f,  CURVE_LIN,  UNIT_PCT,   0, 0.01f },
};

const param_desc_t *param_desc(int id) { return &table[id]; }

/* ln(2) helpers via dsp_exp2 / a log2 approximation for the EXP curve. */
static float log2_approx(float x)
{
    union { float f; int32_t i; } u = { x };
    float e = (float)(((u.i >> 23) & 255) - 128);
    u.i = (u.i & ~(255 << 23)) | (127 << 23);          /* mantissa in [1, 2) */
    float m = u.f;
    return e + 1.0f + (m - 1.0f) * (1.4425449f + (m - 1.0f) * (-0.7181452f +
           (m - 1.0f) * (0.4575485f + (m - 1.0f) * (-0.2779042f +
           (m - 1.0f) * (0.1217970f - (m - 1.0f) * 0.02584474f)))));
}

float param_to_value(int id, float n)
{
    const param_desc_t *d = &table[id];
    n = dsp_clamp(n, 0.0f, 1.0f);
    switch (d->curve) {
    case CURVE_EXP:
        return d->min * dsp_exp2(n * log2_approx(d->max / d->min));
    case CURVE_INT:
    case CURVE_ENUM: {
        float v = d->min + n * (d->max - d->min);
        return (float)(int32_t)(v + (v < 0 ? -0.5f : 0.5f));
    }
    default:
        return d->min + n * (d->max - d->min);
    }
}

float param_to_norm(int id, float v)
{
    const param_desc_t *d = &table[id];
    if (d->curve == CURVE_EXP)
        return dsp_clamp(log2_approx(v / d->min) / log2_approx(d->max / d->min), 0.0f, 1.0f);
    return dsp_clamp((v - d->min) / (d->max - d->min), 0.0f, 1.0f);
}

/* ---- formatting (no printf on the Nano) ---- */
static char *put_int(char *b, int32_t v)
{
    char t[12];
    int n = 0;
    uint32_t u = v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v;
    if (v < 0) *b++ = '-';
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *b++ = t[--n];
    return b;
}

/* v with `dec` decimals (0..2), rounded. */
static char *put_fixed(char *b, float v, int dec)
{
    int32_t scale = dec == 2 ? 100 : dec == 1 ? 10 : 1;
    int32_t x = (int32_t)(v * (float)scale + (v < 0 ? -0.5f : 0.5f));
    if (x < 0) { *b++ = '-'; x = -x; }
    b = put_int(b, x / scale);
    if (dec) {
        *b++ = '.';
        int32_t f = x % scale;
        if (dec == 2 && f < 10) *b++ = '0';
        b = put_int(b, f);
    }
    return b;
}

static char *put_str(char *b, const char *s)
{
    while (*s) *b++ = *s++;
    return b;
}

void param_format(int id, float v, char *buf)
{
    const param_desc_t *d = &table[id];
    char *b = buf;
    switch (d->unit) {
    case UNIT_CENTS: b = put_fixed(b, v, v < 10 ? 1 : 0); b = put_str(b, "c"); break;
    case UNIT_HZ:
        if (v >= 1000) { b = put_fixed(b, v / 1000, v < 10000 ? 2 : 1); b = put_str(b, "k"); }
        else b = put_fixed(b, v, v < 1 ? 2 : v < 10 ? 1 : 0);
        b = put_str(b, "Hz");
        break;
    case UNIT_MS:
        if (v >= 1000) { b = put_fixed(b, v / 1000, 2); b = put_str(b, "s"); }
        else { b = put_fixed(b, v, 0); b = put_str(b, "ms"); }
        break;
    case UNIT_S: b = put_fixed(b, v, v < 10 ? 2 : 1); b = put_str(b, "s"); break;
    case UNIT_PCT: b = put_fixed(b, v * 100, 0); b = put_str(b, "%"); break;
    case UNIT_ST: if (v > 0) *b++ = '+'; b = put_int(b, (int32_t)v); b = put_str(b, "st"); break;
    default:
        if (d->curve == CURVE_ENUM) b = put_str(b, d->labels[(int32_t)v]);
        else b = put_fixed(b, v, d->curve == CURVE_INT ? 0 : 2);
        break;
    }
    *b = 0;
}

/* Common synth CCs, then CC 20..43 -> parameters 0..23 in page order. */
int param_for_cc(int cc)
{
    switch (cc) {
    case 1:  return P_FMOD_DEPTH;
    case 7:  return P_VOLUME;
    case 71: return P_RESO;
    case 72: return P_RELEASE;
    case 73: return P_ATTACK;
    case 74: return P_CUTOFF;
    case 91: return P_REV_MIX;
    case 93: return P_CHORUS;
    }
    if (cc >= 20 && cc < 20 + P_COUNT)
        return cc - 20;
    return -1;
}
