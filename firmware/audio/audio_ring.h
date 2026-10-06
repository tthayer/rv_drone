// Block ring between the rvlink receiver (producer, link IRQ) and the I2S
// refill (consumer, audio IRQ, higher priority) on the same core. Single
// producer, single consumer: each index is written by one side only. Pure C
// (no Pico SDK) so the host test runs it.
#ifndef AUDIO_RING_H
#define AUDIO_RING_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rvlink.h"

#define AUDIO_RING_BLOCKS   8u      // power of two
#define AUDIO_RING_TARGET   4u      // fill to prime to / keep requesting up to (5.3 ms)
#define AUDIO_RING_WORDS    (RVLINK_BLOCK_FRAMES * 2u)

typedef struct {
    int32_t blk[AUDIO_RING_BLOCKS][AUDIO_RING_WORDS];
    volatile uint32_t head;         // producer: blocks pushed
    volatile uint32_t tail;         // consumer: blocks popped
    uint32_t overflows;             // producer: push into a full ring (dropped)
    uint32_t underruns;             // consumer: pop while playing and empty
    bool playing;                   // consumer: primed, popping
} audio_ring_t;

#define AUDIO_RING_BARRIER() __asm__ volatile("" ::: "memory")

static inline uint32_t audio_ring_fill(const audio_ring_t *r) {
    return r->head - r->tail;
}

static inline bool audio_ring_push(audio_ring_t *r, const int32_t *src) {
    if (audio_ring_fill(r) >= AUDIO_RING_BLOCKS) { r->overflows++; return false; }
    memcpy(r->blk[r->head % AUDIO_RING_BLOCKS], src, sizeof r->blk[0]);
    AUDIO_RING_BARRIER();
    r->head++;
    return true;
}

// Always fills dst: a block from the ring, or silence while priming / on underrun.
// Playback starts once AUDIO_RING_TARGET blocks are queued and stops (re-primes)
// on an underrun.
static inline void audio_ring_pop(audio_ring_t *r, int32_t *dst) {
    uint32_t fill = audio_ring_fill(r);
    if (!r->playing && fill >= AUDIO_RING_TARGET) r->playing = true;
    if (r->playing && fill == 0) { r->underruns++; r->playing = false; }
    if (!r->playing) { memset(dst, 0, sizeof r->blk[0]); return; }
    AUDIO_RING_BARRIER();
    memcpy(dst, r->blk[r->tail % AUDIO_RING_BLOCKS], sizeof r->blk[0]);
    AUDIO_RING_BARRIER();
    r->tail++;
}

#endif
