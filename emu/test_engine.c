/* Host tests for engine behaviour that once regressed silently:
 *  1. dsp_exp2 accuracy (Taylor coefficients made envelopes ~6x too short).
 *  2. Envelope timing: RELEASE r -> level e^(-t/(r/3)) after the note-off.
 *  3. Voice stealing: a stolen voice fades out before its new note starts.
 *  4. STACK intervals land on the expected partials.
 *  5. Shimmer: each mode adds energy at its shifted pitch, and the feedback
 *     loop decays at its worst-case settings instead of sustaining itself.
 *  6. FREEZE holds the reverb; HALL lets it die away. */
#include <math.h>
#include <stdio.h>
#include "dsp.h"
#include "engine.h"
#include "params.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define SR 48000
static float tl[SR * 32], tr[SR * 32];

/* Goertzel magnitude per sample of one second starting at a. */
static double tone(const float *b, int a, double f)
{
    double w = 2 * 3.14159265358979 * f / SR, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = a; i < a + SR; i++) { double s0 = b[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / SR;
}

/* Energy within +-30 Hz of f: a delay-line shifter lands a few Hz off pitch. */
static double band(const float *b, int a, double f)
{
    double e = 0;
    for (double g = f - 30; g <= f + 30; g += 1) { double m = tone(b, a, g); e += m * m; }
    return sqrt(e);
}

static double rms(const float *b, int a)
{
    double e = 0;
    for (int i = a; i < a + SR; i++) e += (double)b[i] * b[i];
    return sqrt(e / SR);
}

/* One plain voice into the reverb only: sines, no detune, chorus or delay. */
static void plain_setup(int mode, float size, float damp)
{
    engine_init(SR);
    engine_set_param(P_LATCH, 0); engine_set_param(P_ATTACK, 0.01f); engine_set_param(P_RELEASE, 0.3f);
    engine_set_param(P_OSCS, 3); engine_set_param(P_DETUNE, 0); engine_set_param(P_DRIFT, 0);
    engine_set_param(P_SHAPE, 0); engine_set_param(P_SUB, 0); engine_set_param(P_SPREAD, 0);
    engine_set_param(P_CUTOFF, 12000); engine_set_param(P_FMOD_DEPTH, 0); engine_set_param(P_DRIVE, 0);
    engine_set_param(P_CHORUS, 0); engine_set_param(P_DLY_MIX, 0); engine_set_param(P_REV_MIX, 1);
    engine_set_param(P_REV_SIZE, size); engine_set_param(P_DAMP, damp);
    engine_set_param(P_REV_MODE, mode); engine_set_param(P_SHIMMER, 1.0f);
}

/* Renders `secs` seconds into tl/tr; notes off at off_s, mode -> mode_to at mode_s. */
static void run(const int *notes, int nn, int secs, float off_s, float mode_s, int mode_to)
{
    for (int i = 0; i < nn; i++) engine_note_on(notes[i], 100);
    for (int s = 0; s < secs * SR; s += 32) {
        if (s == (int)(mode_s * SR) / 32 * 32) engine_set_param(P_REV_MODE, mode_to);
        if (s == (int)(off_s * SR) / 32 * 32)
            for (int i = 0; i < nn; i++) engine_note_off(notes[i]);
        engine_render(tl + s, tr + s, 32);
    }
}

int main(void)
{
    /* 1. exp2 */
    double worst = 0;
    for (int i = -3000; i <= 3000; i++) {
        float x = (float)i / 100.0f;
        double e = fabs(dsp_exp2(x) / exp2(x) - 1.0);
        if (e > worst) worst = e;
    }
    for (int i = 1; i <= 1000; i++) {                 /* tiny negative exponents */
        float x = -(float)i * 1e-6f;
        double e = fabs(dsp_exp2(x) / exp2(x) - 1.0);
        if (e > worst) worst = e;
    }
    CHECK(worst < 2e-7, "dsp_exp2 rel error %.2e", worst);

    /* 2. release timing: note on (fast attack), off at 0.1 s, RELEASE 4 s */
    engine_init(48000);
    engine_set_param(P_LATCH, 0);
    engine_set_param(P_ATTACK, 0.01f);
    engine_set_param(P_RELEASE, 4.0f);
    float l[32], r[32];
    engine_note_on(60, 100);
    int note; float env = 0;
    for (long s = 0; s < 48000 * 3 / 2; s += 32) {
        if (s == 4800) engine_note_off(60);
        engine_render(l, r, 32);
    }
    engine_voice_info(0, &note, &env);
    double want = exp(-1.4 / (4.0 / 3.0));               /* 1.4 s of release */
    CHECK(fabs(env - want) < 0.03, "release env %.3f, want %.3f", env, want);

    /* 3. stealing: 24 rising notes, 16 voices fill with release tails */
    engine_init(48000);
    engine_set_param(P_LATCH, 0);
    engine_set_param(P_ATTACK, 0.01f);
    engine_set_param(P_RELEASE, 4.0f);
    int pn[ENGINE_VOICES]; float pe[ENGINE_VOICES];
    for (int i = 0; i < ENGINE_VOICES; i++) { pn[i] = -1; pe[i] = 0; }
    int k = 0, cur = -1, steals = 0, loud = 0;
    long next = 0, off = -1;
    for (long s = 0; s < 48000 * 6; s += 32) {
        if (s >= next && k < 24) { cur = 40 + k++; engine_note_on(cur, 100); off = s + 4800; next = s + 7200; }
        if (off >= 0 && s >= off) { engine_note_off(cur); off = -1; }
        engine_render(l, r, 32);
        for (int i = 0; i < ENGINE_VOICES; i++) {
            int n; float e;
            engine_voice_info(i, &n, &e);
            if (pn[i] >= 0 && n >= 0 && n != pn[i]) { steals++; if (pe[i] > 2e-3f) loud++; }
            pn[i] = n; pe[i] = e;
        }
    }
    CHECK(steals > 0, "no voice was stolen (test setup)");
    CHECK(loud == 0, "%d stolen voices switched note while still audible", loud);

    /* 4. STACK intervals: one voice, pure sines, no detune/drift/effects. Energy at
     * the expected partials of A3 (220 Hz): UNISON has no 440, OCTAVES does. */
    double unison440 = 0, unison220 = 0, oct[4] = { 0 };
    for (int mode = 0; mode < 2; mode++) {
        engine_init(48000);
        engine_set_param(P_ATTACK, 0.01f);
        engine_set_param(P_OSCS, 4); engine_set_param(P_STACK, mode ? 1 : 0);
        engine_set_param(P_DETUNE, 0); engine_set_param(P_DRIFT, 0); engine_set_param(P_SHAPE, 0);
        engine_set_param(P_SUB, 0); engine_set_param(P_SPREAD, 0); engine_set_param(P_CUTOFF, 12000);
        engine_set_param(P_FMOD_DEPTH, 0); engine_set_param(P_RESO, 0); engine_set_param(P_DRIVE, 0);
        engine_set_param(P_CHORUS, 0); engine_set_param(P_DLY_MIX, 0); engine_set_param(P_REV_MIX, 0);
        engine_note_on(57, 100);
        static float buf[48000];
        for (int s = 0; s < 48000; s += 32) engine_render(buf + s, r, 32);
        const double f[5] = { 110, 220, 440, 880, 330 };
        double e[5];
        for (int j = 0; j < 5; j++) {                 /* Goertzel over the last 0.5 s */
            double w = 2 * 3.14159265358979 * f[j] / 48000, c = 2 * cos(w), s1 = 0, s2 = 0;
            for (int i = 24000; i < 48000; i++) { double s0 = buf[i] + c * s1 - s2; s2 = s1; s1 = s0; }
            e[j] = sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
        }
        if (!mode) { unison220 = e[1]; unison440 = e[2]; }
        else for (int j = 0; j < 4; j++) oct[j] = e[j];
        if (mode) CHECK(e[4] < 0.05 * e[1], "OCTAVES: unexpected energy at 330 Hz");
    }
    CHECK(unison440 < 0.05 * unison220, "UNISON: energy at 440 Hz (%.3g vs %.3g)", unison440, unison220);
    CHECK(oct[0] > 0.2 * oct[1] && oct[2] > 0.2 * oct[1] && oct[3] > 0.2 * oct[1],
          "OCTAVES: missing partials (110 %.3g, 220 %.3g, 440 %.3g, 880 %.3g)", oct[0], oct[1], oct[2], oct[3]);

    /* 5. Shimmer: A3 held for 3 s. Each mode's shifted band (2-3 s) vs HALL's. */
    {
        static const double shifted[4] = { 0, 440, 329.6, 110 };
        static const char *const name[4] = { "HALL", "SHIM OCT", "SHIM 5TH", "SUB OCT" };
        const int a3 = 57;
        double hall[4] = { 0 };
        for (int mode = 0; mode < 4; mode++) {
            plain_setup(mode, 0.6f, 0.5f);
            run(&a3, 1, 3, 3.0f, -1, 0);
            if (!mode) {
                for (int m = 1; m < 4; m++) hall[m] = band(tl, 2 * SR, shifted[m]);
                continue;
            }
            double b = band(tl, 2 * SR, shifted[mode]), f0 = tone(tl, 2 * SR, 220);
            CHECK(b > 10 * hall[mode] && b > 0.05 * f0, "%s: shifted band %.4f (HALL %.4f, 220 Hz %.4f)",
                  name[mode], b, hall[mode], f0);
        }
        /* worst case for loop gain: 3-note chord, SIZE 0.75, DAMP 0, SHIMMER 100 % */
        const int chord[3] = { 45, 57, 64 };
        for (int mode = 1; mode < 4; mode++) {
            plain_setup(mode, 0.75f, 0.0f);
            run(chord, 3, 30, 1.0f, -1, 0);
            double early = rms(tl, 2 * SR), late = rms(tl, 29 * SR);
            CHECK(late < 0.01 * early, "%s: tail did not decay (rms %.4f at 2 s, %.4f at 29 s)",
                  name[mode], early, late);
        }
    }

    /* 6. FREEZE: switch at 2 s while the note sounds, release at 2.5 s. The
     * reverb holds (20 s vs 4 s); the same run in HALL dies away. */
    {
        const int a3 = 57;
        double hold[2];
        for (int f = 0; f < 2; f++) {
            plain_setup(0, 0.5f, 0.5f);
            run(&a3, 1, 21, 2.5f, f ? 2.0f : -1, 4);
            hold[f] = rms(tl, 20 * SR) / rms(tl, 4 * SR);
        }
        CHECK(hold[1] > 0.7, "FREEZE: level fell to %.3f of its 4 s value by 20 s", hold[1]);
        CHECK(hold[0] < 0.01, "HALL: level still %.3f of its 4 s value at 20 s", hold[0]);
    }

    if (fails) printf("test_engine: %d FAILED\n", fails);
    else printf("test_engine: all passed (exp2 %.1e, %d steals faded, STACK partials, shimmer, freeze ok)\n", worst, steals);
    return fails != 0;
}
