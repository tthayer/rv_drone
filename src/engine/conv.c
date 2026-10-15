#include "conv.h"
#include "fft.h"

void conv_init(conv_t *c, const float *ir, int ir_len)
{
    int p, i;
    fft_init();                     /* idempotent: build the twiddles if needed */
    for (i = 0; i < CONV_N; i++) c->hist[i] = 0.0f;
    c->pos = 0;
    c->npart = (ir_len + CONV_B - 1) / CONV_B;
    if (c->npart < 1) c->npart = 1;
    if (c->npart > CONV_P) c->npart = CONV_P;
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
}

void conv_process(conv_t *c, const float *in, float *out)
{
    int i, k, p;
    /* slide the input history left by one block, append the new samples */
    for (i = 0; i < CONV_N - CONV_B; i++) c->hist[i] = c->hist[i + CONV_B];
    for (i = 0; i < CONV_B; i++) c->hist[CONV_N - CONV_B + i] = in[i];
    /* FFT of the N-sample window; store it in the ring at `pos` */
    for (i = 0; i < CONV_N; i++) { c->xr[i] = c->hist[i]; c->xi[i] = 0.0f; }
    fft(c->xr, c->xi, CONV_N, 0);
    for (i = 0; i < CONV_N; i++) { c->x_re[c->pos][i] = c->xr[i]; c->x_im[c->pos][i] = c->xi[i]; }
    /* Y = sum_p H_p * X_{pos-p} */
    for (i = 0; i < CONV_N; i++) { c->yr[i] = 0.0f; c->yi[i] = 0.0f; }
    for (p = 0; p < c->npart; p++) {
        int q = c->pos - p; if (q < 0) q += c->npart;
        const float *hr = c->ir_re[p], *hi = c->ir_im[p];
        const float *xr = c->x_re[q], *xi = c->x_im[q];
        for (k = 0; k < CONV_N; k++) {
            c->yr[k] += hr[k] * xr[k] - hi[k] * xi[k];
            c->yi[k] += hr[k] * xi[k] + hi[k] * xr[k];
        }
    }
    fft(c->yr, c->yi, CONV_N, 1);
    for (i = 0; i < CONV_B; i++) out[i] = c->yr[CONV_B + i];  /* overlap-save: last block */
    if (++c->pos >= c->npart) c->pos = 0;
}
