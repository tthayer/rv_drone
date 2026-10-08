/* Host tests for engine behaviour that once regressed silently:
 *  1. dsp_exp2 accuracy (Taylor coefficients made envelopes ~6x too short).
 *  2. Envelope timing: RELEASE r -> level e^(-t/(r/3)) after the note-off.
 *  3. Voice stealing: a stolen voice fades out before its new note starts. */
#include <math.h>
#include <stdio.h>
#include "dsp.h"
#include "engine.h"
#include "params.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

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

    if (fails) printf("test_engine: %d FAILED\n", fails);
    else printf("test_engine: all passed (exp2 %.1e, %d steals faded, STACK partials ok)\n", worst, steals);
    return fails != 0;
}
