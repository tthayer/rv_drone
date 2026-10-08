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

    printf(fails ? "test_engine: %d FAILED\n" : "test_engine: all passed (exp2 %.1e, %d steals faded)\n",
           fails ? fails : worst, steals);
    return fails != 0;
}
