/* Uniform partitioned overlap-save FFT convolution (issue #4, stage 2). The IR
 * is split into CONV_B-sample partitions, each transformed once; the live
 * signal is convolved block by block with the radix-2 FFT (fft.c). Latency is
 * one block (CONV_B samples) — fine for a reverb send. Mono IR; run one
 * instance per channel. conv_init() calls fft_init() itself. */
#ifndef CONV_H
#define CONV_H
#include <stdint.h>

#define CONV_B 512                  /* partition / block size (samples) */
#define CONV_N 1024                 /* FFT size = 2*CONV_B */
#define CONV_P 32                   /* partitions -> IR up to CONV_P*CONV_B */

typedef struct {
    int npart, pos;
    /* Distributed accumulation (issue #4): the partition MACs are spread across
     * the CONV_B samples that follow each input block, so no single render
     * block carries the whole convolution (which overran the block deadline by
     * 3-5x and dropped audio). */
    int acc_p, acc_gap, acc_ctr;            /* next partition, spread, countdown */
    int in_n, out_n, out_len;               /* per-sample staging */
    float ir_re[CONV_P][CONV_N], ir_im[CONV_P][CONV_N];   /* partitioned IR spectra */
    float x_re[CONV_P][CONV_N], x_im[CONV_P][CONV_N];     /* input spectra ring */
    float xr[CONV_N], xi[CONV_N], yr[CONV_N], yi[CONV_N], hist[CONV_N];
    float in[CONV_B], out[CONV_B];
} conv_t;

void conv_init(conv_t *c, const float *ir, int ir_len);
/* One sample in, one sample out. The output lags the input by two blocks (one
 * to spread the MACs over, one to emit): fine for a reverb send. */
float conv_step(conv_t *c, float x);
void conv_process(conv_t *c, const float *in, float *out);   /* CONV_B in/out */

#endif
