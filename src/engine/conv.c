#include "conv.h"
#include "fft.h"

void conv_init(conv_t *c, const float *ir, int ir_len)
{
    int p, i;
    fft_init();                     /* idempotent: build the twiddles if needed */
    for (i = 0; i < CONV_N; i++) c->hist[i] = 0.0f;
    c->pos = 0;
    c->in_n = 0; c->out_n = 0; c->out_len = 0;
    c->acc_p = 0; c->acc_gap = 1; c->acc_ctr = 1;
    c->npart = (ir_len + CONV_B - 1) / CONV_B;
    if (c->npart < 1) c->npart = 1;
    if (c->npart > CONV_P) c->npart = CONV_P;
    c->acc_p = c->npart;            /* nothing accumulating until the first block */
    for (p = 0; p < CONV_P; p++)
        for (i = 0; i < CONV_N; i++) {
            c->ir_re[p][i] = 0.0f; c->ir_im[p][i] = 0.0f;
            c->x_re[p][i] = 0.0f; c->x_im[p][i] = 0.0f;
        }
    for (p = 0; p < c->npart; p++) {
        for (i = 0; i < CONV_N; i++) { c->xr[i] = 0.0f; c->xi[i] = 0.0f; }
        for (i = 0; i < CONV_B; i++) {
            int k = p * CONV_B + i;
            c->xr[i] = (k < ir_len) ? ir[k] : 0.0f;
        }
        fft(c->xr, c->xi, CONV_N, 0);
        for (i = 0; i < CONV_N; i++) { c->ir_re[p][i] = c->xr[i]; c->ir_im[p][i] = c->xi[i]; }
    }
    for (i = 0; i < CONV_N; i++) { c->yr[i] = 0.0f; c->yi[i] = 0.0f; }
    for (i = 0; i < CONV_B; i++) c->out[i] = 0.0f;
}

/* One sample in, one sample out. Doing the whole convolution in one call made a
 * single 64-frame render block take ~4-6 ms (budget 1333 us) and dropped audio,
 * so the partition MACs are spread: one partition every acc_gap = CONV_B/npart
 * samples. The forward FFT still happens on the input-block boundary and the
 * inverse FFT when the last partition lands; both are small (L1-resident). */
float conv_step(conv_t *c, float x)
{
    int i;
    float y = c->out_n < c->out_len ? c->out[c->out_n++] : 0.0f;
    c->in[c->in_n++] = x;

    if (c->acc_p < c->npart && --c->acc_ctr <= 0) {
        int q = c->pos - c->acc_p; if (q < 0) q += c->npart;
        const float *hr = c->ir_re[c->acc_p], *hi = c->ir_im[c->acc_p];
        const float *xr = c->x_re[q], *xi = c->x_im[q];
        for (int k = 0; k < CONV_N; k++) {
            c->yr[k] += hr[k] * xr[k] - hi[k] * xi[k];
            c->yi[k] += hr[k] * xi[k] + hi[k] * xr[k];
        }
        c->acc_p++;
        c->acc_ctr = c->acc_gap;
        if (c->acc_p >= c->npart) {                     /* all partitions in: emit */
            fft(c->yr, c->yi, CONV_N, 1);
            for (i = 0; i < CONV_B; i++) c->out[i] = c->yr[CONV_B + i];   /* overlap-save */
            c->out_len = CONV_B; c->out_n = 0;
        }
    }

    if (c->in_n >= CONV_B) {                            /* new input block */
        for (i = 0; i < CONV_N - CONV_B; i++) c->hist[i] = c->hist[i + CONV_B];
        for (i = 0; i < CONV_B; i++) c->hist[CONV_N - CONV_B + i] = c->in[i];
        for (i = 0; i < CONV_N; i++) { c->xr[i] = c->hist[i]; c->xi[i] = 0.0f; }
        fft(c->xr, c->xi, CONV_N, 0);
        c->pos++; if (c->pos >= c->npart) c->pos = 0;
        for (i = 0; i < CONV_N; i++) { c->x_re[c->pos][i] = c->xr[i]; c->x_im[c->pos][i] = c->xi[i]; }
        for (i = 0; i < CONV_N; i++) { c->yr[i] = 0.0f; c->yi[i] = 0.0f; }
        c->acc_gap = CONV_B / c->npart; if (c->acc_gap < 1) c->acc_gap = 1;
        c->acc_p = 0;
        c->acc_ctr = c->acc_gap;
        c->in_n = 0;
    }
    return y;
}

void conv_process(conv_t *c, const float *in, float *out)
{
    for (int i = 0; i < CONV_B; i++) out[i] = conv_step(c, in[i]);
}
