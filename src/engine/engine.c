#include <string.h>

#include "engine.h"
#include "dsp.h"
#include "osc.h"
#include "voice.h"

#define MAX_OSC     OSC_MAX
#define CTRL        32                  /* control-rate block */
#define DLY_N       131072              /* > 2 s at 48 kHz, power of 2 */
#define CHO_N       2048
#define REV_LINES   8
#define REV_N       8192
/* Cache-set staggering: buffers whose sizes are powers of two and that are
 * accessed at the same index (all 8 reverb lines are written at rev_w; the
 * delay's L/R pair at dly_w) otherwise map to the same D-cache set and evict
 * each other on every sample. On the C906 that cost ~130 ns per reverb access
 * (137 us/block). Padding each reverb row by one 64 B line puts line j j sets
 * further on; the delay's R buffer is pushed 5 lines away from L. */
#define REV_PAD     16                  /* floats = one 64 B cache line */
/* Shimmer: a two-tap pitch shifter on the reverb's output, fed back into its
 * input. Each tap's delay sweeps across SHIM_W samples (85 ms) and the taps are
 * half a sweep apart, with Hann crossfades that sum to 1. Like any delay-line
 * shifter it detunes a pure tone by a few Hz (the grain rate), which the reverb
 * smears into the shimmer; more taps or longer sweeps cancel on some notes. */
#define SHIM_N      8192
#define SHIM_W      4096.0f
#define SHIM_MIN    32.0f               /* shortest tap delay, samples: >= CTRL (see the pre-pass) */
_Static_assert((int)SHIM_MIN >= CTRL, "the shimmer pre-pass must only read earlier blocks");
/* Feedback at SHIMMER 100 %. Host sweep (3-note chord, every mode, SIZE 0..1,
 * DAMP 0 / 0.5): the loop sustains itself above 0.19 at worst (OCT, DAMP 0,
 * SIZE 0.75); 0.15 keeps every tail decaying. */
#define SHIM_FB     0.15f
#define FREEZE_G    0.99998f            /* per pass: holds for minutes, can't grow */
enum { REV_HALL, REV_SHIM_OCT, REV_SHIM_5TH, REV_SUB_OCT, REV_FREEZE };
#define DLY_PAD     (16 * 5)

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
    int pending;                        /* note to start once a steal fade ends, -1 = none */
} voice_t;

/* A stolen voice fades to silence over this long before its new note starts,
 * instead of being cut off; at 30 ms the late start is hard to hear on a drone. */
#define STEAL_FADE_S   0.03f
#define SCALAR_MAX_OSC 7                /* oscillators per voice when RVV is off (Nano) */
#define STEAL_DONE_ENV 1e-3f            /* "silent enough" to switch notes */

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
    float dly_l[DLY_N];
    float dly_pad[DLY_PAD];
    float dly_r[DLY_N];
    uint32_t dly_w;
    float dly_lp_l, dly_lp_r;
    float rev[REV_LINES][REV_N + REV_PAD];
    uint32_t rev_w;
    float rev_lp[REV_LINES];
    uint32_t rev_lenI[REV_LINES];
    float rev_g[REV_LINES];
    float shim[SHIM_N];
    uint32_t shim_w;
    float shim_ph, shim_lp, shim_dc;
    float rev_in_s;                     /* smoothed reverb input gain (0 in FREEZE) */
    float shim_s;                       /* smoothed shimmer feedback */
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
    float mix_s;                        /* smoothed voice-sum gain */
} E;

static voice_lanes_t VL;                /* lane buffers for the voice path */

/* Output gain before the soft clipper. Measured worst case (16 voices x 7 osc,
 * drive 60 %, reso 90 %) peaks at 2.7x full scale before this stage at
 * VOLUME 100 %. 0.4 puts that at about -3 dBFS, so the clipper is only a safety
 * net. It was 1.5 until 2026-10-07, which clipped hard even at 4 voices. */
#define OUT_GAIN 0.4f

/* Voice-sum gain: unity up to 4 voices (as before), then 2/sqrt(n), so 16 voices
 * at once don't drive the output clipper. No libm: a table. */
static const float mix_gain[ENGINE_VOICES + 1] = {
    1, 1, 1, 1, 1, 0.8944272f, 0.8164966f, 0.7559289f, 0.7071068f,
    0.6666667f, 0.6324555f, 0.6030227f, 0.5773503f, 0.5547002f, 0.5345225f,
    0.5163978f, 0.5f };

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
    float x = 1.0f / tc;                /* per-sample coef = 1 - e^-x */
    if (x < 0.05f)                      /* series: exact where 1 - 2^(-tiny) loses precision */
        return x * (1.0f - x * 0.5f * (1.0f - x * (1.0f / 3.0f)));
    return 1.0f - dsp_exp2(-1.4426950f * x);
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
        if (E.v[i].note == note && E.v[i].pending < 0) best = &E.v[i];
    if (best) { voice_start(best, note); return; }
    for (int i = 0; i < ENGINE_VOICES; i++)        /* already queued on a fading voice */
        if (E.v[i].pending == note) return;
    for (int i = 0; i < ENGINE_VOICES; i++)        /* a silent voice */
        if (E.v[i].pending < 0 && !E.v[i].gate && E.v[i].env < 1e-4f) {
            voice_start(&E.v[i], note);
            return;
        }
    /* Steal: the quietest releasing voice, else the quietest held one (ties: oldest).
     * It fades out over STEAL_FADE_S and then starts the new note. */
    for (int i = 0; i < ENGINE_VOICES; i++) {
        voice_t *v = &E.v[i];
        if (!best) { best = v; continue; }
        int vr = !v->gate, br = !best->gate;
        if (vr != br) { if (vr) best = v; continue; }
        if (v->env < best->env || (v->env == best->env && v->age < best->age)) best = v;
    }
    if (best->env < STEAL_DONE_ENV) {
        best->env = 0.0f;
        best->pending = -1;
        voice_start(best, note);
    } else {
        best->gate = 0;
        best->pending = note;                      /* voice_control() fades it fast */
    }
}

void engine_note_off(int note)
{
    if (E.held > 0) E.held--;
    if (E.p[P_LATCH] >= 0.5f) return;
    for (int i = 0; i < ENGINE_VOICES; i++) {
        if (E.v[i].note == note && E.v[i].pending < 0) E.v[i].gate = 0;
        if (E.v[i].pending == note) E.v[i].pending = -1;   /* released before it started */
    }
}

void engine_all_off(void)
{
    E.held = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) {
        E.v[i].gate = 0;
        E.v[i].pending = -1;
    }
}

void engine_voice_info(int i, int *note, float *env)
{
    if (i < 0 || i >= ENGINE_VOICES) { *note = -1; *env = 0.0f; return; }
    *note = E.v[i].note;
    *env = E.v[i].env;
}

int engine_voices_active(void)
{
    int n = 0;
    for (int i = 0; i < ENGINE_VOICES; i++) n += E.v[i].gate || E.v[i].env > 1e-4f;
    return n;
}

void engine_init(float sr)
{
    /* Clear everything, effect buffers included, so a re-init (tests, emulator)
     * starts silent; keep the timer hook and the kernel choice. */
    uint64_t (*now)(void) = E.now;
    int simd = E.simd;
    memset(&E, 0, sizeof E);
    E.now = now;
    E.simd = simd;
    E.sr = sr;
    E.inv_sr = 1.0f / sr;
    E.rng = 0x12345678u;
    for (int i = 0; i < P_COUNT; i++) E.p[i] = param_desc(i)->def;
    for (int i = 0; i < ENGINE_VOICES; i++) {
        E.v[i].note = -1; E.v[i].env = 0; E.v[i].gate = 0; E.v[i].pending = -1;
    }
    E.held = 0;
    E.cut_s = E.p[P_CUTOFF];
    E.mix_s = 1.0f;
    E.vol_s = E.p[P_VOLUME];
    E.dly_s = E.p[P_DLY_TIME] * 0.001f * sr;
    E.rev_in_s = 0.3f;
    /* silent until a note arrives (MIDI, or the emulator's keyboard / --notes) */
}

/* STACK modes: semitone interval and level weight per oscillator (cycling).
 * Detune spread and drift still apply on top. */
typedef struct { int n; signed char semi[8]; float w[8]; } osc_stack_t;
static const osc_stack_t stacks[4] = {
    { 1, { 0 }, { 1.0f } },                                              /* UNISON */
    { 4, { 0, 12, -12, 24 }, { 1.0f, 0.7f, 0.8f, 0.5f } },               /* OCTAVES */
    { 6, { 0, 7, 12, 19, -12, 24 }, { 1.0f, 0.8f, 0.7f, 0.55f, 0.75f, 0.45f } },   /* FIFTHS */
    { 8, { 0, 12, 19, 24, 28, 31, 36, -12 },                              /* ORGAN: 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' 16' */
         { 1.0f, 0.8f, 0.6f, 0.55f, 0.45f, 0.4f, 0.35f, 0.7f } },
};

/* Control-rate update of one voice: increments, pans, envelope coefficient. */
static void voice_control(voice_t *v, int nosc)
{
    float f0 = dsp_mtof((float)v->note + E.p[P_TRANSPOSE]);
    float det = E.p[P_DETUNE], drift = E.p[P_DRIFT] * 10.0f;   /* cents */
    float spread = E.p[P_SPREAD];
    float half = (float)(nosc - 1) * 0.5f;
    int sm = (int)E.p[P_STACK];
    const osc_stack_t *st = &stacks[sm < 0 || sm > 3 ? 0 : sm];
    for (int k = 0; k < nosc; k++) {
        int si = k % st->n;
        float pos = half > 0 ? ((float)k - half) / half : 0.0f;        /* -1..1 */
        v->drift_ph[k] += v->drift_rate[k] * (float)CTRL * E.inv_sr;
        if (v->drift_ph[k] >= 1.0f) v->drift_ph[k] -= 1.0f;
        float cents = det * pos + drift * dsp_sin1(v->drift_ph[k]) + 100.0f * (float)st->semi[si];
        float dt = f0 * dsp_exp2(cents * (1.0f / 1200.0f)) * E.inv_sr;
        float w = st->w[si];
        if (dt > 0.45f) { dt = 0.45f; w = 0.0f; }      /* above the ceiling: mute, don't mistune */
        v->bank.inc[k] = (uint32_t)(dt * 4294967296.0f);
        v->bank.dt[k] = dt;
        v->bank.idt[k] = 1.0f / dt;
        /* alternate sides so neighbours in pitch sit apart; equal power */
        float pan = spread * ((k & 1) ? pos : -pos);
        float a = (pan + 1.0f) * 0.125f;                                /* 0..0.25 */
        v->bank.gl[k] = w * dsp_sin1(0.25f - a);
        v->bank.gr[k] = w * dsp_sin1(a);
    }
    v->bank.n = nosc;
    v->sub_inc = (uint32_t)(f0 * 0.5f * E.inv_sr * 4294967296.0f);
    v->env_target = v->gate ? 1.0f : 0.0f;
    v->env_coef = coef_for_time(v->pending >= 0 ? STEAL_FADE_S
                                : v->gate ? E.p[P_ATTACK] : E.p[P_RELEASE], E.sr);
}

/* Level per oscillator count, ~1/sqrt(n), so changing OSCS doesn't jump the level. */
static const float osc_norm[MAX_OSC + 1] = { 0, 1, 0.71f, 0.58f, 0.5f, 0.45f, 0.41f, 0.38f,
    0.35f, 0.33f, 0.32f, 0.30f, 0.29f, 0.28f, 0.27f, 0.26f, 0.25f };


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
    /* Without the RVV kernels, 16 voices x 16 oscillators takes ~1800 us per block
     * on the C906, over the 1333 us real-time budget. Plain C stays within budget
     * up to 7 (measured 976 us), so cap there. The host emulator is unaffected. */
#ifdef ENGINE_RVV
    if (!E.simd && nosc > SCALAR_MAX_OSC) nosc = SCALAR_MAX_OSC;
#endif
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

    /* ---- voices: oscillators per voice (vector over the block), then every
     * voice's filter/drive/envelope in one lane kernel (vector over voices) ---- */
    int map[ENGINE_VOICES], nact = 0;
    uint64_t t_osc = 0;
    for (int vi = 0; vi < ENGINE_VOICES; vi++) {
        voice_t *v = &E.v[vi];
        if (v->note < 0 || (!v->gate && v->env < 1e-4f)) continue;
        voice_control(v, nosc);
        /* filter LFO: voices 0-3 keep the original spread; later voices are
         * interleaved between them so 16 voices stay within ~1.5x in rate */
        float spread = 0.13f * (float)(vi & 3) + 0.037f * (float)(vi >> 2);
        float phoff = 0.25f * (float)(vi & 3) + 0.0625f * (float)(vi >> 2);
        if (lfo_beats > 0.0f) {                       /* synced: phase from the beat */
            float ph = E.beat_pos / lfo_beats + phoff;
            v->lfo_ph = ph - (float)(int32_t)ph;
            if (v->lfo_ph < 0.0f) v->lfo_ph += 1.0f;
        } else {
            v->lfo_ph += E.p[P_FMOD_RATE] * (1.0f + spread) * (float)n * E.inv_sr;
            if (v->lfo_ph >= 1.0f) v->lfo_ph -= 1.0f;
        }
        float fc = E.cut_s * dsp_exp2(E.p[P_FMOD_DEPTH] * 3.0f * dsp_sin1(v->lfo_ph));
        fc = dsp_clamp(fc, 20.0f, 0.42f * E.sr);
        float w = fc * E.inv_sr * 0.5f;                     /* tan(pi fc / sr) */
        float g = dsp_sin1(w) / dsp_sin1(0.25f - w);
        float a1 = 1.0f / (1.0f + g * (g + k_damp)), a2 = g * a1, a3 = g * a2;

        /* oscillator bank + sub (a one-oscillator sine bank, centred) */
        float bl[CTRL], br[CTRL];
        for (int i = 0; i < n; i++) bl[i] = br[i] = 0.0f;
        osc_bank_t sb = { .n = 0 };
        if (sub > 0.0f) {
            float dt = (float)v->sub_inc * (1.0f / 4294967296.0f);
            float gsub = sub * 1.2f / norm;
            sb = (osc_bank_t){ .ph = { v->sub_ph }, .inc = { v->sub_inc }, .dt = { dt },
                               .idt = { 1.0f / dt }, .gl = { gsub }, .gr = { gsub }, .n = 1 };
        }
        uint64_t t0 = E.now ? E.now() : 0;
#ifdef ENGINE_RVV
        if (E.simd) {
            osc_bank_rvv(&v->bank, shape, n, bl, br);
            if (sb.n) osc_bank_rvv(&sb, 0.0f, n, bl, br);
        } else
#endif
        {
            osc_bank_scalar(&v->bank, shape, n, bl, br);
            if (sb.n) osc_bank_scalar(&sb, 0.0f, n, bl, br);
        }
        if (E.now) t_osc += E.now() - t0;
        v->sub_ph = sb.n ? sb.ph[0] : v->sub_ph + (uint32_t)n * v->sub_inc;

        /* two lanes for this voice */
        int L = 2 * nact;
        for (int c = 0; c < 2; c++) {
            VL.ic1[L + c] = v->f[c].ic1;
            VL.ic2[L + c] = v->f[c].ic2;
            VL.env[L + c] = v->env;
            VL.a1[L + c] = a1;
            VL.a2[L + c] = a2;
            VL.a3[L + c] = a3;
            VL.env_tgt[L + c] = v->env_target;
            VL.env_coef[L + c] = v->env_coef;
        }
        for (int i = 0; i < n; i++) {
            VL.x[i][L] = bl[i] * norm;
            VL.x[i][L + 1] = br[i] * norm;
        }
        map[nact++] = vi;
    }
    uint64_t tv = E.now ? E.now() : 0;
    VL.lanes = 2 * nact;
    VL.k = k_damp;
    VL.bp = bp;
    VL.drive = drive;
    VL.drive_out = drive_out;
    if (nact) {
#ifdef ENGINE_RVV
        if (E.simd) voice_lanes_rvv(&VL, n);
        else
#endif
        voice_lanes_scalar(&VL, n);
    }
    E.mix_s += (mix_gain[nact] - E.mix_s) * 0.05f;
    for (int i = 0; i < n; i++) {
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < nact; k++) {
            l += VL.y[i][2 * k];
            r += VL.y[i][2 * k + 1];
        }
        outl[i] = l * E.mix_s;
        outr[i] = r * E.mix_s;
    }
    for (int k = 0; k < nact; k++) {
        voice_t *v = &E.v[map[k]];
        for (int c = 0; c < 2; c++) {
            v->f[c].ic1 = dsp_flush(VL.ic1[2 * k + c]);
            v->f[c].ic2 = dsp_flush(VL.ic2[2 * k + c]);
        }
        v->env = VL.env[2 * k];
        if (v->pending >= 0 && v->env < STEAL_DONE_ENV) {   /* steal fade done: new note */
            int n2 = v->pending;
            v->pending = -1;
            v->env = 0.0f;                                  /* fresh phases, attack from 0 */
            voice_start(v, n2);
        } else if (!v->gate && v->env < 1e-4f) {
            v->env = 0; v->note = -1;
        }
    }
    if (E.now) {
        E.prof.osc += t_osc;
        E.prof.voice += E.now() - tv;
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
    int rmode = (int)E.p[P_REV_MODE];
    int freeze = rmode == REV_FREEZE;
    if (freeze) damp = 0.0f;                              /* hold the spectrum as it is */
    for (int j = 0; j < REV_LINES; j++) {
        float len = rev_base[j] * (0.5f + 1.3f * size);
        E.rev_lenI[j] = (uint32_t)len;
        E.rev_g[j] = freeze ? FREEZE_G : dsp_exp2(-9.9657843f * len / (t60 * E.sr));
    }
    static const float shim_ratio[REV_FREEZE] = { 1.0f, 2.0f, 1.4983071f, 0.5f };
    float shim_t = (rmode > REV_HALL && rmode < REV_FREEZE) ? E.p[P_SHIMMER] : 0.0f;
    float shim_step = rmode > REV_HALL && rmode < REV_FREEZE ? (1.0f - shim_ratio[rmode]) / SHIM_W : 0.0f;
    float rin_t = freeze ? 0.0f : 0.3f;
    int shim_on = shim_t > 0.0f || E.shim_s > 1e-4f;
    if (!shim_on) E.shim_s = 0.0f;
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
    /* Shimmer pre-pass: pitch-shift the reverb's wet sum (written below, one
     * sample per frame), band-limit it and turn it into feedback for this
     * block. Every tap is at least SHIM_MIN >= CTRL samples old, so it only
     * reads earlier blocks and can run ahead of the reverb loop. Skipped in
     * HALL/FREEZE once the feedback has faded out. */
    float shim_fb[CTRL];
    for (int i = 0; i < n; i++) shim_fb[i] = 0.0f;
    if (shim_on) {
        for (int i = 0; i < n; i++) {
            uint32_t w = E.shim_w + (uint32_t)i;
            float p1 = E.shim_ph, p2 = p1 + 0.5f;
            if (p2 >= 1.0f) p2 -= 1.0f;
            float s1 = tap(E.shim, SHIM_N - 1, w, SHIM_MIN + p1 * SHIM_W);
            float s2 = tap(E.shim, SHIM_N - 1, w, SHIM_MIN + p2 * SHIM_W);
            float g1 = 0.5f - 0.5f * dsp_sin1(p1 + 0.25f);     /* Hann: sin^2(pi p1) */
            float sh = s1 * g1 + s2 * (1.0f - g1);
            E.shim_ph += shim_step;
            if (E.shim_ph < 0.0f) E.shim_ph += 1.0f;
            else if (E.shim_ph >= 1.0f) E.shim_ph -= 1.0f;
            E.shim_lp += (sh - E.shim_lp) * 0.5f;              /* ~4.6 kHz lowpass */
            E.shim_dc += (E.shim_lp - E.shim_dc) * 0.01f;      /* ~77 Hz highpass */
            E.shim_s += (shim_t - E.shim_s) * 0.001f;
            shim_fb[i] = dsp_tanh((E.shim_lp - E.shim_dc) * E.shim_s * SHIM_FB);
        }
    }
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
        E.shim[(E.shim_w + i) & (SHIM_N - 1)] = wetl + wetr;
        float fb = shim_fb[i];
        for (int s = 1; s < REV_LINES; s <<= 1)            /* fast Hadamard */
            for (int j = 0; j < REV_LINES; j += s << 1)
                for (int q = j; q < j + s; q++) {
                    float a = x[q], b = x[q + s];
                    x[q] = a + b;
                    x[q + s] = a - b;
                }
        E.rev_in_s += (rin_t - E.rev_in_s) * 0.001f;
        float inl = l * E.rev_in_s + fb, inr = r * E.rev_in_s + fb;
        for (int j = 0; j < REV_LINES; j++)
            E.rev[j][E.rev_w & (REV_N - 1)] = dsp_flush(x[j] * 0.35355339f + ((j & 1) ? inr : inl) * ((j & 2) ? -1.0f : 1.0f));
        E.rev_w++;
        l += rmix * wetl * 0.35f;
        r += rmix * wetr * 0.35f;
        E.vol_s += (vol_t - E.vol_s) * 0.001f;
        outl[i] = dsp_tanh(l * E.vol_s * OUT_GAIN);
        outr[i] = dsp_tanh(r * E.vol_s * OUT_GAIN);
    }
    if (E.now) {
        uint64_t t3 = E.now();
        E.prof.chorus += t1 - t0;
        E.prof.delay += t2 - t1;
        E.prof.reverb += t3 - t2;
    }
    E.shim_w += (uint32_t)n;
    for (int j = 0; j < REV_LINES; j++) E.rev_lp[j] = dsp_flush(E.rev_lp[j]);
    E.shim_lp = dsp_flush(E.shim_lp);
    E.shim_dc = dsp_flush(E.shim_dc);
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
