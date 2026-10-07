#include "engine.h"
#include "dsp.h"
#include "osc.h"

#define MAX_OSC     OSC_MAX
#define CTRL        32                  /* control-rate block */
#define DLY_N       131072              /* > 2 s at 48 kHz, power of 2 */
#define CHO_N       2048
#define REV_LINES   8
#define REV_N       8192

typedef struct { float ic1, ic2; } svf_t;

typedef struct {
    int note;                           /* -1 = unused */
    int gate;
    float env, env_target, env_coef;
    osc_bank_t bank;
    float drift_ph[MAX_OSC], drift_rate[MAX_OSC];
    uint32_t sub_ph, sub_inc;
    float lfo_ph;
    svf_t f[2];
    uint32_t age;
} voice_t;

static struct {
    float sr, inv_sr;
    float p[P_COUNT];
    voice_t v[ENGINE_VOICES];
    uint32_t age, rng;
    int held;                           /* keys physically down (latch logic) */
    /* smoothed controls */
    float cut_s, vol_s, dly_s;
    /* fx state */
    float cho_l[CHO_N], cho_r[CHO_N];
    uint32_t cho_w;
    float cho_ph;
    float dly_l[DLY_N], dly_r[DLY_N];
    uint32_t dly_w;
    float dly_lp_l, dly_lp_r;
    float rev[REV_LINES][REV_N];
    uint32_t rev_w;
    float rev_lp[REV_LINES];
    uint32_t rev_lenI[REV_LINES];
    float rev_g[REV_LINES];
    /* MIDI clock */
    uint32_t clk_last_t;
    int clk_have_last;
    float clk_iv[24];                   /* last tick intervals, us */
    int clk_iv_n, clk_iv_i;
    float clk_period_us;                /* mean tick interval */
    int32_t clk_ticks;                  /* since Start, wrapped at CLK_WRAP */
    float beat_pos;                     /* beats since Start, follows clk_ticks */
    uint32_t clk_age;                   /* samples since the last tick */
    int clk_running;
    /* SIMD + profiling */
    int simd;
    uint64_t (*now)(void);
    engine_profile_t prof;
} E;

#define CLK_WRAP_BEATS 96               /* multiple of every division (<= 32 beats) */

static const float rev_base[REV_LINES] = { 1031, 1327, 1523, 1871, 2053, 2377, 2617, 2969 };

void engine_clock(int kind, uint32_t t)
{
    switch (kind) {
    case ENGINE_CLK_START:
        E.clk_ticks = -1;               /* the next tick is beat 0 */
        E.beat_pos = 0.0f;
        E.clk_running = 1;
        return;
    case ENGINE_CLK_CONTINUE: E.clk_running = 1; return;
    case ENGINE_CLK_STOP:     E.clk_running = 0; return;
    }
    if (E.clk_have_last) {
        float iv = (float)(uint32_t)(t - E.clk_last_t);
        if (iv > 2000.0f && iv < 250000.0f) {          /* 10..1250 BPM */
            E.clk_iv[E.clk_iv_i] = iv;
            E.clk_iv_i = (E.clk_iv_i + 1) % 24;
            if (E.clk_iv_n < 24) E.clk_iv_n++;
            float sum = 0;
            for (int i = 0; i < E.clk_iv_n; i++) sum += E.clk_iv[i];
            E.clk_period_us = sum / (float)E.clk_iv_n;
        } else if (iv >= 250000.0f) {
            E.clk_iv_n = 0;                            /* gap: start averaging afresh */
        }
    }
    E.clk_last_t = t;
    E.clk_have_last = 1;
    E.clk_age = 0;
    E.clk_ticks++;
    if (E.clk_ticks >= CLK_WRAP_BEATS * 24) {
        E.clk_ticks -= CLK_WRAP_BEATS * 24;
        E.beat_pos -= (float)CLK_WRAP_BEATS;
    }
    /* pull the free-running beat position onto the tick grid */
    float err = (float)E.clk_ticks * (1.0f / 24.0f) - E.beat_pos;
    if (err > 1.0f || err < -1.0f) E.beat_pos += err;
    else E.beat_pos += 0.25f * err;
}

static int clock_valid(void)
{
    return E.clk_period_us > 0.0f && E.clk_iv_n >= 4 && E.clk_age < (uint32_t)(E.sr * 0.5f);
}

float engine_bpm(void) { return clock_valid() ? 60.0e6f / (24.0f * E.clk_period_us) : 0.0f; }
int engine_clock_running(void) { return E.clk_running; }

static float coef_for_time(float seconds, float rate_hz)
{
    /* one-pole: reach ~95% (3 time constants) in `seconds` */
    float tc = seconds * rate_hz / 3.0f;
    if (tc < 1.0f) return 1.0f;
    return 1.0f - dsp_exp2(-1.4426950f / tc);
}

void engine_set_param(int id, float value)
{
    if (id < 0 || id >= P_COUNT) return;
    const param_desc_t *d = param_desc(id);
    E.p[id] = dsp_clamp(value, d->min, d->max);
    if (id == P_LATCH && E.p[id] < 0.5f) {         /* latch off: drop latched notes */
        for (int i = 0; i < ENGINE_VOICES; i++)
            if (E.v[i].note >= 0 && E.held == 0) E.v[i].gate = 0;
    }
}

float engine_param(int id) { return (id >= 0 && id < P_COUNT) ? E.p[id] : 0.0f; }

static void voice_start(voice_t *v, int note)
{
    v->note = note;
    v->gate = 1;
    v->age = ++E.age;
    if (v->env < 1e-4f) {                          /* fresh voice: new random phases */
        for (int k = 0; k < MAX_OSC; k++) {
            v->bank.ph[k] = (uint32_t)(int32_t)(dsp_noise(&E.rng) * 2147483647.0f);
            v->drift_ph[k] = 0.5f + 0.5f * dsp_noise(&E.rng);
            v->drift_rate[k] = 0.05f + 0.12f * (0.5f + 0.5f * dsp_noise(&E.rng));
        }
        v->sub_ph = 0;
        v->f[0] = v->f[1] = (svf_t){ 0, 0 };
    }
}

void engine_note_on(int note, int vel)
{
    if (vel == 0) { engine_note_off(note); return; }
    int latch = E.p[P_LATCH] >= 0.5f;
    if (latch && E.held == 0) {                    /* first key of a new chord */
        for (int i = 0; i < ENGINE_VOICES; i++) E.v[i].gate = 0;
    }
    E.held++;
    voice_t *best = 0;
    for (int i = 0; i < ENGINE_VOICES; i++)        /* same note: retrigger it */
        if (E.v[i].note == note) best = &E.v[i];
    if (!best)
        for (int i = 0; i < ENGINE_VOICES; i++)    /* a silent voice */
            if (!E.v[i].gate && E.v[i].env < 1e-4f) { best = &E.v[i]; break; }
    if (!best)
        for (int i = 0; i < ENGINE_VOICES; i++)    /* a releasing one, else the oldest */
            if (!best || (!E.v[i].gate && best->gate) ||
                (E.v[i].gate == best->gate && E.v[i].age < best->age))
                best = &E.v[i];
    voice_start(best, note);
}

void engine_note_off(int note)
{
    if (E.held > 0) E.held--;
    if (E.p[P_LATCH] >= 0.5f) return;
    for (int i = 0; i < ENGINE_VOICES; i++)
        if (E.v[i].note == note) E.v[i].gate = 0;
}

void engine_all_off(void)
{
    E.held = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) E.v[i].gate = 0;
}

int engine_voices_active(void)
{
    int n = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) n += E.v[i].gate || E.v[i].env > 1e-4f;
    return n;
}

void engine_init(float sr)
{
    /* static storage is zeroed at load (bss); reset what matters */
    E.sr = sr;
    E.inv_sr = 1.0f / sr;
    E.rng = 0x12345678u;
    for (int i = 0; i < P_COUNT; i++) E.p[i] = param_desc(i)->def;
    for (int i = 0; i < ENGINE_VOICES; i++) { E.v[i].note = -1; E.v[i].env = 0; E.v[i].gate = 0; }
    E.held = 0;
    E.cut_s = E.p[P_CUTOFF];
    E.vol_s = E.p[P_VOLUME];
    E.dly_s = E.p[P_DLY_TIME] * 0.001f * sr;
    engine_note_on(38, 100);                       /* D2 + A2, latched */
    engine_note_on(45, 100);
    E.held = 0;
}

/* Control-rate update of one voice: increments, pans, envelope coefficient. */
static void voice_control(voice_t *v, int nosc)
{
    float f0 = dsp_mtof((float)v->note + E.p[P_TRANSPOSE]);
    float det = E.p[P_DETUNE], drift = E.p[P_DRIFT] * 10.0f;   /* cents */
    float spread = E.p[P_SPREAD];
    float half = (float)(nosc - 1) * 0.5f;
    for (int k = 0; k < nosc; k++) {
        float pos = half > 0 ? ((float)k - half) / half : 0.0f;        /* -1..1 */
        v->drift_ph[k] += v->drift_rate[k] * (float)CTRL * E.inv_sr;
        if (v->drift_ph[k] >= 1.0f) v->drift_ph[k] -= 1.0f;
        float cents = det * pos + drift * dsp_sin1(v->drift_ph[k]);
        float dt = f0 * dsp_exp2(cents * (1.0f / 1200.0f)) * E.inv_sr;
        if (dt > 0.45f) dt = 0.45f;
        v->bank.inc[k] = (uint32_t)(dt * 4294967296.0f);
        v->bank.dt[k] = dt;
        v->bank.idt[k] = 1.0f / dt;
        /* alternate sides so neighbours in pitch sit apart; equal power */
        float pan = spread * ((k & 1) ? pos : -pos);
        float a = (pan + 1.0f) * 0.125f;                                /* 0..0.25 */
        v->bank.gl[k] = dsp_sin1(0.25f - a);
        v->bank.gr[k] = dsp_sin1(a);
    }
    v->bank.n = nosc;
    v->sub_inc = (uint32_t)(f0 * 0.5f * E.inv_sr * 4294967296.0f);
    v->env_target = v->gate ? 1.0f : 0.0f;
    v->env_coef = coef_for_time(v->gate ? E.p[P_ATTACK] : E.p[P_RELEASE], E.sr);
}

static const float osc_norm[MAX_OSC + 1] = { 0, 1, 0.71f, 0.58f, 0.5f, 0.45f, 0.41f, 0.38f };

/* Read `d` (fractional, >= 1) samples behind write index w, linear interpolation.
 * Integer index math: a float write counter would lose precision after 2^24. */
static inline float tap(const float *buf, uint32_t mask, uint32_t w, float d)
{
    uint32_t id = (uint32_t)d;
    float f = d - (float)id;
    float a = buf[(w - id) & mask], b = buf[(w - id - 1) & mask];
    return a + f * (b - a);
}

static void render_block(float *outl, float *outr, int n)
{
    int nosc = (int)E.p[P_OSCS];
    if (nosc < 1) nosc = 1;
    if (nosc > MAX_OSC) nosc = MAX_OSC;
    float shape = E.p[P_SHAPE], sub = E.p[P_SUB];
    float norm = osc_norm[nosc] * 0.8f;
    E.cut_s += (E.p[P_CUTOFF] - E.cut_s) * 0.15f;
    float k_damp = 2.0f - 2.0f * E.p[P_RESO];
    float drive = 1.0f + 3.0f * E.p[P_DRIVE];
    float drive_out = 1.0f / (1.0f + E.p[P_DRIVE]);

    /* clock: advance the beat position at the measured tempo */
    E.clk_age += (uint32_t)n;
    int synced = E.p[P_SYNC] >= 0.5f && clock_valid();
    float beat_s = synced ? 24.0f * E.clk_period_us * 1e-6f : 0.0f;   /* seconds per beat */
    if (synced) {
        E.beat_pos += (float)n * E.inv_sr / beat_s;
        if (E.beat_pos >= (float)CLK_WRAP_BEATS) E.beat_pos -= (float)CLK_WRAP_BEATS;
    }
    float lfo_beats = synced ? param_lfo_div_beats((int)E.p[P_LFO_DIV]) : 0.0f;
    float dly_beats = synced ? param_dly_div_beats((int)E.p[P_DLY_DIV]) : 0.0f;
    int bp = E.p[P_FMODE] >= 0.5f;

    for (int i = 0; i < n; i++) outl[i] = outr[i] = 0.0f;

    for (int vi = 0; vi < ENGINE_VOICES; vi++) {
        voice_t *v = &E.v[vi];
        if (v->note < 0 || (!v->gate && v->env < 1e-4f)) continue;
        voice_control(v, nosc);
        /* filter coefficients for this block */
        if (lfo_beats > 0.0f) {                       /* synced: phase from the beat */
            float ph = E.beat_pos / lfo_beats + 0.25f * (float)vi;
            v->lfo_ph = ph - (float)(int32_t)ph;
            if (v->lfo_ph < 0.0f) v->lfo_ph += 1.0f;
        } else {
            v->lfo_ph += E.p[P_FMOD_RATE] * (1.0f + 0.13f * (float)vi) * (float)n * E.inv_sr;
            if (v->lfo_ph >= 1.0f) v->lfo_ph -= 1.0f;
        }
        float fc = E.cut_s * dsp_exp2(E.p[P_FMOD_DEPTH] * 3.0f * dsp_sin1(v->lfo_ph));
        fc = dsp_clamp(fc, 20.0f, 0.42f * E.sr);
        float w = fc * E.inv_sr * 0.5f;                     /* tan(pi fc / sr) */
        float g = dsp_sin1(w) / dsp_sin1(0.25f - w);
        float a1 = 1.0f / (1.0f + g * (g + k_damp)), a2 = g * a1, a3 = g * a2;

        /* oscillator bank for the whole block, then the per-sample voice path */
        float bl[CTRL], br[CTRL];
        for (int i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
        uint64_t t0 = E.now ? E.now() : 0;
#ifdef ENGINE_RVV
        if (E.simd) osc_bank_rvv(&v->bank, shape, n, bl, br);
        else
#endif
        osc_bank_scalar(&v->bank, shape, n, bl, br);
        uint64_t t1 = E.now ? E.now() : 0;
        for (int i = 0; i < n; i++) {
            float sb = sub * dsp_sin1((float)v->sub_ph * (1.0f / 4294967296.0f)) * 1.2f;
            v->sub_ph += v->sub_inc;
            float l = bl[i] * norm + sb;
            float r = br[i] * norm + sb;
            /* TPT SVF per channel */
            float in[2] = { l, r }, out[2];
            for (int c = 0; c < 2; c++) {
                svf_t *f = &v->f[c];
                float v3 = in[c] - f->ic2;
                float v1 = a1 * f->ic1 + a2 * v3;
                float v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
                f->ic1 = 2.0f * v1 - f->ic1;
                f->ic2 = 2.0f * v2 - f->ic2;
                out[c] = bp ? v1 * k_damp : v2;
            }
            v->env += (v->env_target - v->env) * v->env_coef;
            float gn = v->env * drive_out;
            outl[i] += dsp_tanh(out[0] * drive) * gn;
            outr[i] += dsp_tanh(out[1] * drive) * gn;
        }
        if (E.now) {
            E.prof.osc += t1 - t0;
            E.prof.voice += E.now() - t1;
        }
        if (!v->gate && v->env < 1e-4f) { v->env = 0; v->note = -1; }
        v->f[0].ic1 = dsp_flush(v->f[0].ic1); v->f[0].ic2 = dsp_flush(v->f[0].ic2);
        v->f[1].ic1 = dsp_flush(v->f[1].ic1); v->f[1].ic2 = dsp_flush(v->f[1].ic2);
    }

    /* ---- chorus: two modulated taps per side, quadrature LFOs ---- */
    float cmix = E.p[P_CHORUS];
    float dly_target = dly_beats > 0.0f ? dly_beats * beat_s * E.sr : E.p[P_DLY_TIME] * 0.001f * E.sr;
    if (dly_target > 2.0f * E.sr) dly_target = 2.0f * E.sr;
    float dfb = E.p[P_DLY_FB], dmix = E.p[P_DLY_MIX];
    float rmix = E.p[P_REV_MIX];
    float size = E.p[P_REV_SIZE];
    float t60 = 1.5f + 12.0f * size * size;
    float damp = 0.05f + 0.85f * E.p[P_DAMP];             /* lowpass amount in the loop */
    for (int j = 0; j < REV_LINES; j++) {
        float len = rev_base[j] * (0.5f + 1.3f * size);
        E.rev_lenI[j] = (uint32_t)len;
        E.rev_g[j] = dsp_exp2(-9.9657843f * len / (t60 * E.sr));
    }
    E.cho_ph += 0.23f * (float)n * E.inv_sr;
    if (E.cho_ph >= 1.0f) E.cho_ph -= 1.0f;
    float vol_t = E.p[P_VOLUME] * E.p[P_VOLUME];

    uint64_t t0 = E.now ? E.now() : 0;
    /* chorus pass */
    for (int i = 0; i < n; i++) {
        float l = outl[i] * 0.45f, r = outr[i] * 0.45f;
        E.cho_l[E.cho_w & (CHO_N - 1)] = l;
        E.cho_r[E.cho_w & (CHO_N - 1)] = r;
        float ph = E.cho_ph + (float)i * 0.23f * E.inv_sr;
        float m1 = dsp_sin1(ph), m2 = dsp_sin1(ph + 0.25f);
        float dl = (0.012f + 0.004f * m1) * E.sr, dr = (0.012f + 0.004f * m2) * E.sr;
        float wl = tap(E.cho_l, CHO_N - 1, E.cho_w, dl), wr = tap(E.cho_r, CHO_N - 1, E.cho_w, dr);
        float wl2 = tap(E.cho_l, CHO_N - 1, E.cho_w, dr * 1.37f);
        float wr2 = tap(E.cho_r, CHO_N - 1, E.cho_w, dl * 1.37f);
        E.cho_w++;
        outl[i] = l + cmix * 0.6f * (wl + wr2);
        outr[i] = r + cmix * 0.6f * (wr + wl2);
    }
    uint64_t t1 = E.now ? E.now() : 0;
    /* feedback delay pass: cross-fed (ping-pong-ish), damped, smoothed time */
    for (int i = 0; i < n; i++) {
        float l = outl[i], r = outr[i];
        E.dly_s += (dly_target - E.dly_s) * 0.0005f;
        float yl = tap(E.dly_l, DLY_N - 1, E.dly_w, E.dly_s);
        float yr = tap(E.dly_r, DLY_N - 1, E.dly_w, E.dly_s);
        E.dly_lp_l += (yl - E.dly_lp_l) * 0.35f;
        E.dly_lp_r += (yr - E.dly_lp_r) * 0.35f;
        E.dly_l[E.dly_w & (DLY_N - 1)] = dsp_flush(l + dfb * E.dly_lp_r);
        E.dly_r[E.dly_w & (DLY_N - 1)] = dsp_flush(r + dfb * E.dly_lp_l);
        E.dly_w++;
        outl[i] = l + dmix * yl;
        outr[i] = r + dmix * yr;
    }
    uint64_t t2 = E.now ? E.now() : 0;
    /* FDN reverb pass (8 lines, Hadamard feedback, per-line damping) + output */
    for (int i = 0; i < n; i++) {
        float l = outl[i], r = outr[i];
        float x[REV_LINES];
        for (int j = 0; j < REV_LINES; j++) {
            float y = E.rev[j][(E.rev_w - E.rev_lenI[j]) & (REV_N - 1)];
            E.rev_lp[j] += (y - E.rev_lp[j]) * (1.0f - damp);
            x[j] = E.rev_lp[j] * E.rev_g[j];
        }
        float wetl = x[0] + x[2] + x[4] + x[6], wetr = x[1] + x[3] + x[5] + x[7];
        for (int s = 1; s < REV_LINES; s <<= 1)            /* fast Hadamard */
            for (int j = 0; j < REV_LINES; j += s << 1)
                for (int q = j; q < j + s; q++) {
                    float a = x[q], b = x[q + s];
                    x[q] = a + b;
                    x[q + s] = a - b;
                }
        float inl = l * 0.3f, inr = r * 0.3f;
        for (int j = 0; j < REV_LINES; j++)
            E.rev[j][E.rev_w & (REV_N - 1)] = dsp_flush(x[j] * 0.35355339f + ((j & 1) ? inr : inl) * ((j & 2) ? -1.0f : 1.0f));
        E.rev_w++;
        l += rmix * wetl * 0.35f;
        r += rmix * wetr * 0.35f;
        E.vol_s += (vol_t - E.vol_s) * 0.001f;
        outl[i] = dsp_tanh(l * E.vol_s * 1.5f);
        outr[i] = dsp_tanh(r * E.vol_s * 1.5f);
    }
    if (E.now) {
        uint64_t t3 = E.now();
        E.prof.chorus += t1 - t0;
        E.prof.delay += t2 - t1;
        E.prof.reverb += t3 - t2;
    }
    for (int j = 0; j < REV_LINES; j++) E.rev_lp[j] = dsp_flush(E.rev_lp[j]);
    E.dly_lp_l = dsp_flush(E.dly_lp_l);
    E.dly_lp_r = dsp_flush(E.dly_lp_r);
}

void engine_render(float *l, float *r, int n)
{
    uint64_t t0 = E.now ? E.now() : 0;
    int frames = n;
    while (n > 0) {
        int m = n < CTRL ? n : CTRL;
        render_block(l, r, m);
        l += m;
        r += m;
        n -= m;
    }
    if (E.now) {
        E.prof.total += E.now() - t0;
        E.prof.frames += (uint32_t)frames;
    }
}

void engine_set_timer(uint64_t (*now)(void)) { E.now = now; }

void engine_profile_take(engine_profile_t *out)
{
    *out = E.prof;
    E.prof = (engine_profile_t){ 0 };
}

int engine_set_simd(int on)
{
#ifdef ENGINE_RVV
    E.simd = on != 0;
#else
    (void)on;
    E.simd = 0;
#endif
    return E.simd;
}

int engine_simd(void) { return E.simd; }

_Static_assert(ENGINE_CLK_TICK == 0 && ENGINE_CLK_START == 1 && ENGINE_CLK_CONTINUE == 2 &&
               ENGINE_CLK_STOP == 3, "engine clock kinds must match RVPANEL_CLK_*");
