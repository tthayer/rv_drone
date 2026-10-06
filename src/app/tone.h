#ifndef TONE_H
#define TONE_H
#include <stdint.h>

/* M5 test source: a stereo sine, rendered in float (main-loop context only:
 * the trap entry does not save FP registers). */
void tone_init(float hz, float amp);
/* frames stereo frames, int32 L,R interleaved, 24-bit left-aligned (rvlink audio[]). */
void tone_render(int32_t *lr, unsigned frames);
#endif
