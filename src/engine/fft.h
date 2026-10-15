/* Radix-2 float FFT, in-place, freestanding (no libm). Twiddles come from the
 * engine's dsp_sin1, so it obeys the same no-libm rule as the rest of src/.
 *
 * Scalar only for now: XTheadVector has no cheap lane permute/gather for the
 * butterflies, so unlike the osc/voice kernels there is no RVV twin yet (see
 * docs/ENGINE.md). It is the foundation for the partitioned IR convolution
 * (issue #4) and the spectral effects of issue #5. */
#ifndef FFT_H
#define FFT_H
#include <stdint.h>

#define FFT_MAX 4096                      /* largest supported size, power of 2 */

void fft_init(void);                      /* build the twiddle tables (once) */
/* In-place size-n FFT; n is a power of two, n <= FFT_MAX. inverse = 1 applies
 * the true inverse (it divides by n). re/im are length n. */
void fft(float *re, float *im, int n, int inverse);

#endif
